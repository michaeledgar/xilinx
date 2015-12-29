#include "ethernet.h"

#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"

#include "xemacps_hw.h"

#include "sleep.h"

char EmacPsMAC[] = { 0x00, 0x0a, 0x35, 0x01, 0x02, 0x03 };

#define STATCLR (1 << 5)
#define PHYAD 0
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
	// Enable network interfaces
	Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWCTRL_OFFSET,
			  XEMACPS_NWCTRL_TXEN_MASK
			  | XEMACPS_NWCTRL_RXEN_MASK);
	xil_printf("enabled transmit/receive\r\n");

	// DEMO: Print PHY status for 5 seconds
	int i;
	for (i = 0; i < 5; i++) {
		while (Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWSR_OFFSET)
			    & XEMACPS_NWSR_MDIOIDLE_MASK) {
			sleep(0.01);
		}
		u32 Mgtcr = XEMACPS_PHYMNTNC_OP_MASK | XEMACPS_PHYMNTNC_OP_R_MASK |
				(PHYAD << XEMACPS_PHYMNTNC_PHYAD_SHIFT_MASK) |
				(1 << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
		Xil_Out32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET, Mgtcr);
		while (Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_NWSR_OFFSET)
			    & XEMACPS_NWSR_MDIOIDLE_MASK) {
			sleep(0.01);
		}
		u32 Status = Xil_In32(XPAR_XEMACPS_0_BASEADDR + XEMACPS_PHYMNTNC_OFFSET);
		xil_printf("PHY Status: %08x\r\n", Status);
		sleep(1.0);
	}

	xil_printf("done with ethernet demo2\r\n");
	return XST_SUCCESS;
}

#undef STATCLR
