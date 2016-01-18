#include "ethernet.h"

#include "xil_cache.h"
#include "xil_exception.h"
#include "xil_io.h"
#include "xil_mmu.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"
#include "xscugic.h"

#include "xemacps_hw.h"

#include "sleep.h"
#include "stdlib.h"

char EmacPsMAC[] = { 0x00, 0x0a, 0x35, 0x01, 0x02, 0x03 };

#define XSCUGIC_ENABLE_SET_OFFSET	0x00000100 /**< Enable Set Register */
#define XSCUGIC_DISABLE_OFFSET		0x00000180 /**< Enable Clear Register */

#define NUM_BD 128
#define MAX_FRAME_SIZE 1536

/** NONPORTABLE: GCC alignment extension */
typedef volatile char EthernetFrame[MAX_FRAME_SIZE]
	__attribute__ ((aligned(64)));

volatile EthernetFrame TxFrames[NUM_BD];
volatile EthernetFrame RxFrames[NUM_BD];

typedef volatile u32 BdEntry[2] __attribute__ ((aligned(0x08)));

BdEntry TxFrameList[NUM_BD];
BdEntry RxFrameList[NUM_BD];

static XScuGic IntcInstance;

static volatile u8 TxComplete = 0;

#define STATCLR (1 << 5)
#define PHYAD 0
#define STATREG 1
#define CTRLREG 0
#define PHY_AUTONEGOTIATE_DONE_MASK (1 << 5)

void waitForIdleMDIO() {
	volatile u32 nwsr = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWSR_OFFSET);
	while (!(nwsr & XEMACPS_NWSR_MDIOIDLE_MASK)) {
		usleep(100);
		nwsr = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWSR_OFFSET);
	}
}

void Ethernet_PrintBd(volatile EthernetFrame *frame, int BdLen) {
	int cnt = 0;
	volatile char *BdData = (volatile char *)*frame;
	while (BdLen--) {
		if (cnt % 8 == 0) {
			xil_printf("%08x ", cnt);
		}
		xil_printf("%02x ", *BdData++);
		if (++cnt % 8 == 0) {
			xil_printf("\r\n");
		}
	}
}

void Ethernet_ProcessRxBds() {
	int i;
	for (i = 0; i < NUM_BD; i++) {
		u32 AddrWrapUsed = RxFrameList[i][0];
		if (AddrWrapUsed & XEMACPS_RXBUF_NEW_MASK) {
			xil_printf("Rx BD %d is new\r\n", i);
			xil_printf("Rx BD %d has AddrWrapUsed of %08x and Status %08x\r\n",
				i, AddrWrapUsed, RxFrameList[i][1]);
			u32 BdLen = RxFrameList[i][1] & 0x1fff;
			volatile EthernetFrame *frame = &RxFrames[i];
			Ethernet_PrintBd(frame, BdLen);
			RxFrameList[i][0] &= ~XEMACPS_RXBUF_NEW_MASK;
		}
	}
}

void Ethernet_ProcessTxBds() {
	int i;
	for (i = 0; i < NUM_BD; i++) {
		u32 Status = TxFrameList[i][1];
		u32 BdLen = Status & 0x1fff;
		if (BdLen && (Status & XEMACPS_TXBUF_USED_MASK)) {
			xil_printf("Tx BD %d has status %08X\r\n", i, Status);
			volatile EthernetFrame *frame = &TxFrames[i];
			Ethernet_PrintBd(frame, BdLen);
			//TxFrameList[i][1] = Status & 0x3;
		}
	}
}

void Ethernet_IntrHandler(void* ignored) {
	u32 IntStatus = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_ISR_OFFSET);
	xil_printf("interrupt status: %08X\r\n", IntStatus);
	if (IntStatus & XEMACPS_IXR_FRAMERX_MASK) {
		u32 RXSR = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXSR_OFFSET);
		xil_printf("Receive status: %08x\r\n", RXSR);
		Ethernet_ProcessRxBds();
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXSR_OFFSET, RXSR);
	}
	if (IntStatus & XEMACPS_IXR_TXCOMPL_MASK) {
		TxComplete = 1;
		u32 TXSR = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXSR_OFFSET);
		xil_printf("Transmit status: %08x\r\n", TXSR);
		Ethernet_ProcessTxBds();
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXSR_OFFSET, TXSR);
		TXSR = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXSR_OFFSET);
		xil_printf("Transmit status readback: %08x\r\n", TXSR);
	}
	if (IntStatus & XEMACPS_IXR_RX_ERR_MASK) {
		u32 RXSR = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXSR_OFFSET);
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXSR_OFFSET, RXSR);

		/* Fix for CR # 692702. Write to bit 18 of net_ctrl
		 * register to flush a packet out of Rx SRAM upon
		 * an error for receive buffer not available. */
		if (IntStatus & XEMACPS_IXR_RXUSED_MASK) {
			Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
					Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET)
					| XEMACPS_NWCTRL_FLUSH_DPRAM_MASK);
		}
		if (IntStatus != (XEMACPS_IXR_RX_ERR_MASK | XEMACPS_IXR_RXUSED_MASK)) {
			// If not just the workaround, we should report the error
			xil_printf("RXSR error: %08X\r\n", RXSR);
		}
		// TODO: Error handler?
	}
	if (IntStatus & (XEMACPS_IXR_TX_ERR_MASK) && !(IntStatus & (XEMACPS_IXR_TXCOMPL_MASK))) {
		/* Clear TX status register */
		u32 TXSR = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXSR_OFFSET);
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXSR_OFFSET, TXSR);
		// TODO: Error handler?
		xil_printf("TXSR error: %08X\r\n", TXSR);
	}
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_ISR_OFFSET, IntStatus);
	Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_ISR_OFFSET);
	xil_printf("interrupt status readback: %08X\r\n", IntStatus);
}

#define OP_MASK XEMACPS_PHYMNTNC_OP_MASK | (PHYAD << XEMACPS_PHYMNTNC_PHYAD_SHIFT_MASK)
#define OP_R_MASK OP_MASK | XEMACPS_PHYMNTNC_OP_R_MASK
#define OP_W_MASK OP_MASK | XEMACPS_PHYMNTNC_OP_W_MASK

u16 writeDiscover(volatile char* TxFrame, u16 idx) {
	u16 TotalLength = 30 + 29 + 240 + 8 + 20 + 14;

	char j = 0;
	for (j = 0; j < 6; ++j) {
		*(TxFrame + j) = 0xff;
	}
	for (j = 0; j < 6; ++j) {
		*(TxFrame + 6 + j) = EmacPsMAC[(int) j];
	}
	*(volatile u16*) (TxFrame + 12) = Xil_EndianSwap16(0x0800);
	volatile char* IpFrame = TxFrame + 14;
	*(volatile u16*) (IpFrame + 0) = Xil_EndianSwap16(((4 << 4) | 5) << 8); // version << 4 | headerWords
	*(volatile u16*) (IpFrame + 2) = Xil_EndianSwap16(TotalLength);
	*(volatile u16*) (IpFrame + 4) = 0; // IP ID
	*(volatile u16*) (IpFrame + 6) = Xil_EndianSwap16(0x4000);
	*(IpFrame + 8) = 255; // TTL
	*(IpFrame + 9) = 0x11; // protocol=UDP
	*(volatile u16*) (IpFrame + 10) = Xil_EndianSwap16(0x7a98); // checksum lol
	*(IpFrame + 12) = 0; // Src IP
	*(IpFrame + 13) = 0;
	*(IpFrame + 14) = 0;
	*(IpFrame + 15) = 0;
	*(IpFrame + 16) = 0xff; // Dest IP
	*(IpFrame + 17) = 0xff;
	*(IpFrame + 18) = 0xff;
	*(IpFrame + 19) = 0xff;
	volatile char* UdpDatagram = IpFrame + 20;
	*(volatile u16*) (UdpDatagram + 0) = Xil_EndianSwap16(68);
	*(volatile u16*) (UdpDatagram + 2) = Xil_EndianSwap16(67);
	*(volatile u16*) (UdpDatagram + 4) = Xil_EndianSwap16(
			TotalLength - (20 + 14));
	*(volatile u16*) (UdpDatagram + 6) = Xil_EndianSwap16(0x59bb - (idx << 8));
	volatile char* DhcpDiscover = UdpDatagram + 8;
	*(DhcpDiscover + 0) = 0x01; // op; 1 = request
	*(DhcpDiscover + 1) = 0x01; // htype; 1 = ethernet
	*(DhcpDiscover + 2) = 0x06; // hlen; 6 bytes
	*(DhcpDiscover + 3) = 0x00; // hops; ?
	*(volatile u16*) (DhcpDiscover + 4) = 0x7391; // xid; random, used to identify responses
	*(volatile u16*) (DhcpDiscover + 6) = 0xd5b0 + idx; // xid; random
	*(volatile u16*) (DhcpDiscover + 8) = 0; // secs
	*(volatile u16*) (DhcpDiscover + 10) = Xil_EndianSwap16(0x0000); // flags; 0x0000 = unicast
	for (j = 12; j < 12 + 16; ++j) {
		*(DhcpDiscover + j) = 0;
	}
	for (j = 0; j < 6; ++j) {
		*(DhcpDiscover + 28 + j) = EmacPsMAC[(int) j];
	}
	for (j = 34; j < 34 + 202; ++j) {
		*(DhcpDiscover + j) = 0;
	}
	*(DhcpDiscover + 236) = 0x63;
	*(DhcpDiscover + 237) = 0x82;
	*(DhcpDiscover + 238) = 0x53;
	*(DhcpDiscover + 239) = 0x63;
	volatile char* DhcpOptions = DhcpDiscover + 240;
	*(DhcpOptions + 0) = 53; // option 53: dhcp message type
	*(DhcpOptions + 1) = 1; // 1 byte
	*(DhcpOptions + 2) = 0x01; // discover = 1
	*(DhcpOptions + 3) = 55; // option 55: parameter request list
	*(DhcpOptions + 4) = 4; // 1 byte/parameter, 4 params
	*(DhcpOptions + 5) = 0x01; // subnet mask
	*(DhcpOptions + 6) = 0x03; // router
	*(DhcpOptions + 7) = 0x06; // domain name server
	*(DhcpOptions + 8) = 0x0f; // domain name
	*(DhcpOptions + 9) = 51; // option 51: lease length
	*(DhcpOptions + 10) = 4; // 4 bytes unsigned int big endian
	*(DhcpOptions + 11) = 0;
	*(DhcpOptions + 12) = 0;
	*(DhcpOptions + 13) = 10;
	*(DhcpOptions + 14) = 0;
	*(DhcpOptions + 15) = 57; // option 57: maximum DHCP message size
	*(DhcpOptions + 16) = 2; // 2 bytes unsigned int big endian
	*(DhcpOptions + 17) = 0x05; // 1500
	*(DhcpOptions + 18) = 0xdc;
	*(DhcpOptions + 19) = 61; // option 61: client identifier
	*(DhcpOptions + 20) = 7; // 1 byte type + 6 bytes MAC address
	*(DhcpOptions + 21) = 0x01; // 1 = ethernet
	*(DhcpOptions + 22) = EmacPsMAC[0];
	*(DhcpOptions + 23) = EmacPsMAC[1];
	*(DhcpOptions + 24) = EmacPsMAC[2];
	*(DhcpOptions + 25) = EmacPsMAC[3];
	*(DhcpOptions + 26) = EmacPsMAC[4];
	*(DhcpOptions + 27) = EmacPsMAC[5];
	*(DhcpOptions + 28) = 255; // option 255: End
	for (j = 29; j < 29 + 30; j++) {
		*(DhcpOptions + j) = 0; // option 0: padding
	}
	Xil_DCacheFlush();

	return TotalLength;
}

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
	Xil_SetTlbAttributes((u32)&RxFrameList, 0xc02);
	Xil_SetTlbAttributes((u32)&TxFrameList, 0xc02);
	for (i = 0; i < NUM_BD; i++) {
		if ((u32)(&RxFrames[i]) & 0x3) {
			xil_printf("Invalid RxFrames[%d] address: %08X\r\n", i, (u32)&RxFrames[i]);
			return XST_FAILURE;
		}
		RxFrameList[i][0] = (u32)&RxFrames[i];
		TxFrameList[i][0] = (u32)&TxFrames[i];
		TxFrameList[i][1] = XEMACPS_TXBUF_USED_MASK;
	}
	RxFrameList[NUM_BD-1][0] |= (1 << 1);   // Rx Wrap bit
	TxFrameList[NUM_BD-1][1] |= (1 << 30);  // Tx Wrap bit

	Xil_DCacheFlushRange((u32)&RxFrameList, NUM_BD*8);
	Xil_DCacheFlushRange((u32)&TxFrameList, NUM_BD*8);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_RXQBASE_OFFSET, (u32)&RxFrameList);
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_TXQBASE_OFFSET, (u32)&TxFrameList);

	// Setup interrupts
	//// Enable GIC.
	Xil_ExceptionInit();

	/*
	 * Initialize the interrupt controller driver so that it is ready to
	 * use.
	 */
	XScuGic_Config *GicConfig = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
	if (NULL == GicConfig) {
		xil_printf("looking up GIC config\r\n");
		return XST_FAILURE;
	}

	int Status = XScuGic_CfgInitialize(&IntcInstance, GicConfig, GicConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS) {
		xil_printf("CfgInitialize\r\n");
		return XST_FAILURE;
	}

	/* Connect the interrupt controller interrupt handler to the hardware
	 * interrupt handling logic in the processor.
	 */
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
			(Xil_ExceptionHandler)XScuGic_InterruptHandler,
			&IntcInstance);

	/* Connect a device driver handler that will be called when an
	 * interrupt for the device occurs, the device driver handler performs
	 * the specific interrupt processing for the device.
	 */
	u32 Int = XPAR_XEMACPS_0_INTR;
	Status = XScuGic_Connect(&IntcInstance, Int,
			(Xil_InterruptHandler) Ethernet_IntrHandler,
			(void *) NULL);

	//// Enable overall gigabit ethernet interrupt in global interrupt handler
	u32 IntOffset = ((Int / 32) * 4);
	u32 IntMask = (1 << (Int % 32));
	u32 CurInt1Mask = Xil_In32(XPAR_SCUGIC_0_DIST_BASEADDR + XSCUGIC_ENABLE_SET_OFFSET + IntOffset);
	Xil_Out32(XPAR_SCUGIC_0_DIST_BASEADDR + XSCUGIC_ENABLE_SET_OFFSET + IntOffset,
			CurInt1Mask | IntMask);
	//// Enable all non-PTP interrupt causes in gigabit interrupt configuration
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_IER_OFFSET, 0xFFFF);

	Xil_ExceptionEnable();

	// Disable management interfaces, enable Tx/Rx
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
			XEMACPS_NWCTRL_TXEN_MASK
			| XEMACPS_NWCTRL_RXEN_MASK);
	xil_printf("enabled transmit/receive (disabled management)\r\n");

	// Send a DHCP discover message
	volatile EthernetFrame* frame = &TxFrames[0];
	volatile char *TxFrame = (volatile char *)*frame;
	u16 TotalLength = writeDiscover(TxFrame, 0);
	xil_printf("populated frame 0\r\n");
	// Send the frame
	TxFrameList[0][1] = (TxFrameList[0][1] | TotalLength | XEMACPS_TXBUF_LAST_MASK) & ~XEMACPS_TXBUF_USED_MASK;
	xil_printf("marked frame 0 for transit\r\n");
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
			XEMACPS_NWCTRL_TXEN_MASK
			| XEMACPS_NWCTRL_STARTTX_MASK
			| XEMACPS_NWCTRL_RXEN_MASK);
	xil_printf("enabled starttx 0, waiting for transmission\r\n");
	while (!TxComplete) {
		sleep(1);
	}
	sleep(1);
	xil_printf("transmit done. starting transmission for frame 1\r\n");
	TxComplete = 0;
	TotalLength = writeDiscover((volatile char *)*&TxFrames[1], 1);
	xil_printf("populated frame 1\r\n");
	TxFrameList[1][1] = (TxFrameList[1][1] | TotalLength | XEMACPS_TXBUF_LAST_MASK) & ~XEMACPS_TXBUF_USED_MASK;
	xil_printf("marked frame 1 for transit\r\n");
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
			XEMACPS_NWCTRL_TXEN_MASK
			| XEMACPS_NWCTRL_STARTTX_MASK
			| XEMACPS_NWCTRL_RXEN_MASK);
	xil_printf("enabled starttx 1\r\n");

	// Watch interrupts fly by?
	sleep(58);

	Xil_ExceptionDisable();
	// Disable Tx/Rx.
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET, 0);
	return XST_SUCCESS;
}
