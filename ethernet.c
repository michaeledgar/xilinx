#include "ethernet.h"

#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"

#include "xemacps_hw.h"

#include "sleep.h"

char EmacPsMAC[] = { 0x00, 0x0a, 0x35, 0x01, 0x02, 0x03 };

#define NUM_BD 128
#define MAX_FRAME_SIZE 1536

/** NONPORTABLE: GCC alignment extension */
typedef char EthernetFrame[MAX_FRAME_SIZE]
	__attribute__ ((aligned(64)));

EthernetFrame TxFrames[NUM_BD];
EthernetFrame RxFrames[NUM_BD];

typedef u32 BdEntry[2] __attribute__ ((aligned(0x08)));

BdEntry TxFrameList[NUM_BD];
BdEntry RxFrameList[NUM_BD];

#define STATCLR (1 << 5)
#define PHYAD 0
#define STATREG 1
#define CTRLREG 0
#define PHY_AUTONEGOTIATE_DONE_MASK (1 << 5)

void waitForIdleMDIO() {
	volatile u32 nwsr = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWSR_OFFSET);
	while (!(nwsr & XEMACPS_NWSR_MDIOIDLE_MASK)) {
		usleep(100);
		xil_printf("waiting for idle MDIO: %08x\r\n", nwsr);
		nwsr = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWSR_OFFSET);
	}
}

#define OP_MASK XEMACPS_PHYMNTNC_OP_MASK \
		| (PHYAD << XEMACPS_PHYMNTNC_PHYAD_SHIFT_MASK)
#define OP_R_MASK OP_MASK | XEMACPS_PHYMNTNC_OP_R_MASK
#define OP_W_MASK OP_MASK | XEMACPS_PHYMNTNC_OP_W_MASK
int ethernet_demo2() {
	xil_printf("starting ethernet demo2\r\n");
	// Reset completely. (585: 16.3.1)
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET, 0);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET, STATCLR);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXSR_OFFSET, 0xFF);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXSR_OFFSET, 0x0F);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_IDR_OFFSET, 0x07FFFEFF);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXQBASE_OFFSET, 0);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXQBASE_OFFSET, 0);
	xil_printf("reset ethernet controller\r\n");
	// Configure the Controller (585: 16.3.2)
	// Program the Network Configuration register (gem.net_cfg).
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCFG_OFFSET,
			  XEMACPS_NWCFG_1536RXEN_MASK
			  | XEMACPS_NWCFG_FDEN_MASK
			  | (MDC_DIV_224 << XEMACPS_NWCFG_MDC_SHIFT_MASK));
	xil_printf("set net_cfg\r\n");
	// Set the MAC address gem.spec1_addr1_bot, gem.spec1_addr1_top
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_LADDR1L_OFFSET,
			  *(u32*)(char*)(EmacPsMAC));
	Xil_Out16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_LADDR1H_OFFSET,
			  *(u16*)((char*)(EmacPsMAC)+4));
	xil_printf("set MAC address\r\n");
	// Configure DMA engine
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_DMACR_OFFSET,
			  (0x19 << XEMACPS_DMACR_RXBUF_SHIFT)
			  | XEMACPS_DMACR_TCPCKSUM_MASK
			  | XEMACPS_DMACR_RXSIZE_MASK
			  | XEMACPS_DMACR_TXSIZE_MASK);
	xil_printf("set DMA config\r\n");
	// Enable management interfaces
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
			XEMACPS_NWCTRL_MDEN_MASK);
	xil_printf("enabled management interface\r\n");

	// DEMO: Print PHY status for 5 seconds
	int i;
	_Bool autodone = 0;
	for (i = 0; i < 30; i++) {
		waitForIdleMDIO();
		volatile u32 Mgtcr = OP_R_MASK
				| (STATREG << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);

		waitForIdleMDIO();
		volatile u32 Status = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);
		autodone = Status & PHY_AUTONEGOTIATE_DONE_MASK;
		if (autodone) {
			xil_printf("autoconfigured!\r\n");
			break;
		}
		xil_printf("PHY Status: %08x\r\n", Status);
		waitForIdleMDIO();

		Mgtcr = OP_R_MASK
			| (CTRLREG << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
		waitForIdleMDIO();
		volatile u16 Ctrl = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);
		Ctrl |= 0x8000  // Reset
			| 0x1000  // ANE
			| 0x0140  // Gigabit?
			//| 0x0200  // Restart_AN
			;
		Mgtcr = OP_W_MASK
			| (CTRLREG << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK)
			| Ctrl
			;
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
		waitForIdleMDIO();
		xil_printf("wrote ctrl register to reset: %04x\r\n", Ctrl);

		Mgtcr = OP_R_MASK
			| (CTRLREG << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
		waitForIdleMDIO();

		Ctrl = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);
		xil_printf("read back ctrl register: %04x\r\n", Ctrl);

		sleep(3);
	}

	xil_printf("done with ethernet demo2, autodone=%d\r\n", autodone);

	// Dump autoconfig results
	u32 Mgtcr = OP_R_MASK
			| (0x05 << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
	waitForIdleMDIO();
	volatile u16 ANLPAR = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);

	Mgtcr = OP_R_MASK
			| (0x06 << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
	waitForIdleMDIO();
	volatile u16 ANER = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);

	Mgtcr = OP_R_MASK
			| (0x08 << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
	waitForIdleMDIO();
	volatile u16 ANNPRR = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);

	Mgtcr = OP_R_MASK
			| (0x0A << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
	waitForIdleMDIO();
	volatile u16 GBSR = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);

	Mgtcr = OP_R_MASK
			| (0x11 << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
	waitForIdleMDIO();
	volatile u16 PHYSR = Xil_In16(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);

	xil_printf("ANLPAR:\t%04x\r\nANER:\t%04x\r\nANNPRR:\t%04x\r\nGBSR:\t%04x\r\nPHYSR:\t%04x\r\n",
			ANLPAR, ANER, ANNPRR, GBSR, PHYSR);

	/** Use autoconfigure results from PHY layer to update GEM
	 * network config register, at MAC layer
	 */
	u32 NewNetCfg = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCFG_OFFSET);
	if (PHYSR & (1 << 13 /* duplex */)) {
		xil_printf("Full duplex\r\n");
		NewNetCfg |= XEMACPS_NWCFG_FDEN_MASK;
	} else {
		xil_printf("Half duplex\r\n");
		NewNetCfg &= ~XEMACPS_NWCFG_FDEN_MASK;
	}
	switch ((PHYSR & 0xC000) >> 14 /* speed, two bits */) {
	case 0b11: // TODO: Error trap
		xil_printf("TODO: error, reserved link speed found\r\n");
		return XST_FAILURE;
	case 0b10: // Gigabit
		xil_printf("Gigabit\r\n");
		NewNetCfg |= XEMACPS_NWCFG_1000_MASK;
		break;
	case 0b01:
		xil_printf("100Mbps\r\n");
		NewNetCfg |= XEMACPS_NWCFG_100_MASK;
		break;
	case 0b00:
		xil_printf("10Mbps\r\n");
		NewNetCfg &= ~XEMACPS_NWCFG_100_MASK;
		break;
	}
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCFG_OFFSET, NewNetCfg);
	// Configure receive and transmit queues
	for (i = 0; i < NUM_BD; i++) {
		if ((u32)(&RxFrames[i]) & 0x3) {
			xil_printf("Invalid RxFrames[%d] address: %08X\r\n", i, (u32)&RxFrames[i]);
			return XST_FAILURE;
		}
		RxFrameList[i][0] =
			(u32)&RxFrames[i]  /* we already verified lower bits are 0, this is safe */
			| 1 /* used bit */
			;
		TxFrameList[i][0] = (u32)&TxFrames[i];
		TxFrameList[i][1] = (1 << 31);  // Used bit
	}
	RxFrameList[NUM_BD-1][0] |= (1 << 1);   // Rx Wrap bit
	TxFrameList[NUM_BD-1][1] |= (1 << 30);  // Tx Wrap bit

	Xil_DCacheFlushRange((u32)&RxFrameList, NUM_BD*8);
	Xil_DCacheFlushRange((u32)&TxFrameList, NUM_BD*8);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXQBASE_OFFSET, (u32)&RxFrameList);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXQBASE_OFFSET, (u32)&TxFrameList);

	// Setup interrupts

	// Disable management interfaces, enable Tx/Rx
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
			XEMACPS_NWCTRL_TXEN_MASK | XEMACPS_NWCTRL_RXEN_MASK);
	xil_printf("enabled transmit/receive (disabled management)\r\n");
	return XST_SUCCESS;
}
