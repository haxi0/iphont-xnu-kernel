/*
 * The OnePlus 8's Wi-Fi radio, a QCA6390, and the bus it is on: PCIe 0 of the SM8250. This file
 * powers the radio, brings the SoC's PCIe controller and its PHY up, trains the link, places the
 * device's memory window and answers for its configuration space; everything above that -- the
 * driver (ath11k's pci.c, its firmware loaded through MHI), QMI over the device's IPCR channel,
 * the station -- is bsd/dev/hk_wifi.c's and the driver's object's (wifi/lx in the repository).
 *
 * The order and the numbers are Android's on this phone (its device tree: qcom,pcie@1c00000,
 * qcom,cnss-qca6390, pinctrl; its kernel: drivers/pci/controller/pci-msm.c msm_pcie_enable() and
 * msm_pcie_link_train(), drivers/net/wireless/cnss2/power.c; register offsets from
 * drivers/clk/qcom/gcc-kona.c and Linux's gcc-sm8250.c), and its log of a boot shows the same
 * steps: "Assert the reset of endpoint", "PHY is ready", "Release the reset", "link initialized"
 * at 5 GT/s x1, BAR 0 at 0x60300000 (1 MB), MSI from GIC 800 on.
 *
 *   power    the radio's supplies through RPMh -- pm8150 S6 0.95 V (aon), pm8009 S2 0.95 V (dig),
 *            pm8150 S5 1.9 V (rfa1), pm8150a S8 1.35 V (rfa2), pm8150a L5 1.8 V (the antenna
 *            switch); its 1.8 V I/O (pm8150 S4) is the UFS's too and always on -- then WLAN_EN
 *            (gpio20) high, 1 ms; for the PHY pm8150 L9 1.2 V and L5 0.88 V (the UFS PHY's as
 *            well), and the PCIe NoC's bus clock manager SN12 voted;
 *   clocks   PCIe 0's power domain (GDSC 0x6b004); its branches (aux 19.2 MHz, cfg AHB, master
 *            and slave AXI, slave Q2A) through the vote register, the PHY's reference (100 MHz),
 *            the Wi-Fi's reference clock buffer, the SMMU's two TBU clocks for PCIe; then the
 *            core's and the PHY's resets pulsed;
 *   PERST    held (gpio79 low) from the start; CLKREQ (gpio80) to its PCIe function, WAKE
 *            (gpio81) an input;
 *   PARF     root complex mode, the PHY powered, DBI at 0, the AXI windows;
 *   PHY      the device tree's sequence of 147 writes, the pipe clock, PHY ready;
 *   link     PERST let go, the controller's Gen3 settings and a target of Gen2, LTSSM on, the
 *            link up (XMLH_LINK_UP and data link active) within 100 ms;
 *   then     the BDF-to-SID table (the device's DMA is SMMU stream 0x1c01: iommu-map), the iATU
 *            for the device's configuration space, the bridge's bus numbers and memory window,
 *            the device's BAR 0 at 0x60300000.
 * Off is the reverse: PERST, the PHY powered down, the clocks and the domain, WLAN_EN low, the
 * radio's own supplies off. The PHY's supplies are the UFS's and stay; S5 (the cameras'
 * analogue supply) and S8 (the UFS's parent) stay.
 *
 * Nothing here is touched unless hk_wifi.c asks (wifi=1 on board=instantnoodle). A register of
 * PCIe 0 is not read before its domain and clocks are on: on this SoC that is a NoC error and
 * Qualcomm's crash dump mode.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/instantnoodle.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hk_wifi_say(const char *format, ...) __printflike(1, 2);

#define say hk_wifi_say

#define PIN(n)                  ((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, (n) })
#define PIN_WLAN_EN             PIN(20)
#define PIN_PERST               PIN(79)
#define PIN_CLKREQ              PIN(80)
#define PIN_WAKE                PIN(81)
#define FUNCTION_PCI_E0         1

/* GCC (0x100000), a page at a time */
#define GCC_PHYS                0x00100000ul
#define GCC_VOTE                0x52008         /* APCS's branch votes: bit 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux, 4 pipe, 5 slv_q2a */
#define GCC_PCIE_0_BCR          0x6b000
#define GCC_PCIE_0_GDSCR        0x6b004
#define GCC_PCIE_0_SLV_Q2A_CBCR 0x6b010
#define GCC_PCIE_0_SLV_AXI_CBCR 0x6b014
#define GCC_PCIE_0_MSTR_AXI_CBCR 0x6b01c
#define GCC_PCIE_0_CFG_AHB_CBCR 0x6b024
#define GCC_PCIE_0_AUX_CBCR     0x6b028
#define GCC_PCIE_0_PIPE_CBCR    0x6b02c
#define GCC_PCIE_0_AUX_RCG      0x6b038         /* command, then configuration at +4 */
#define GCC_PCIE_0_PHY_BCR      0x6c01c
#define GCC_PCIE_REFGEN_RCG     0x6f014
#define GCC_PCIE0_REFGEN_CBCR   0x6f02c
#define GCC_PCIE_WIFI_CLKREF    0x8c004
#define GCC_DDRSS_PCIE_SF_TBU   0x8d058
#define GCC_AGGRE_NOC_PCIE_TBU  0x9000c
#define CBCR_OFF                (1u << 31)
#define GDSC_COLLAPSE           (1u << 0)
#define GDSC_ON                 (1u << 31)
#define RCG_UPDATE              (1u << 0)
#define VOTES_ALL               0x3fu

/* PCIe 0 */
#define PARF_PHYS               0x01c00000ul    /* 0x3000 */
#define PHY_PHYS                0x01c06000ul
#define DBI_PHYS                0x60000000ul    /* the root complex's own configuration space, and ELBI at 0xf20 */
#define IATU_PHYS               0x60001000ul
#define CONF_PHYS               0x60100000ul    /* the device's configuration space, through iATU region 0 */
#define BAR_PHYS                0x60300000ul    /* the memory window: 0x60300000..0x63ffffff */
#define BAR_WINDOW              0x03d00000ul
#define PARF_SYS_CTRL           0x000
#define PARF_PM_CTRL            0x020
#define PARF_PHY_CTRL           0x040
#define PARF_MHI_CLOCK_RESET_CTRL 0x174
#define PARF_AXI_MSTR_WR_ADDR_HALT 0x1a8
#define PARF_LTSSM              0x1b0
#define PARF_INT_ALL_MASK       0x22c
#define PARF_DBI_BASE_ADDR      0x350
#define PARF_SLV_ADDR_SPACE_SIZE 0x358
#define PARF_DEVICE_TYPE        0x1000
#define PARF_BDF_TO_SID         0x2000
#define ELBI_SYS_STTS           0xf28
#define XMLH_LINK_UP            0x400
#define DBI_LINK_STATUS         0x080           /* the express capability's link control and status (0x70 + 0x10) */
#define DBI_DEVICE_CONTROL      0x078
#define DBI_LINK_CONTROL2       0x0a0
#define DBI_SPCIE_CAP           0x154
#define DBI_ACK_F_ASPM_CTRL     0x70c
#define DBI_GEN2_CTRL           0x80c
#define DBI_GEN3_RELATED        0x890
#define DBI_MISC_CONTROL        0x8bc
#define DBI_AUX_CLK_FREQ        0xb40
#define IATU_CTRL1              0x00
#define IATU_CTRL2              0x04
#define IATU_LBAR               0x08
#define IATU_UBAR               0x0c
#define IATU_LAR                0x10
#define IATU_LTAR               0x14
#define IATU_UTAR               0x18
#define PHY_STATUS              0x814           /* bit 6 set: not ready */
#define PHY_POWER_DOWN          0x840
#define SID_BASE                0x1c00
#define EP_BDF                  0x0100          /* bus 1, device 0, function 0 */

/* The PHY's sequence (dt: qcom,phy-sequence of qcom,pcie@1c00000): offset, value; none of its delays is set. */
static const uint16_t phy_sequence[][2] = {
	{ 0x840, 0x03 }, { 0x094, 0x08 }, { 0x154, 0x34 }, { 0x16c, 0x08 }, { 0x058, 0x0f }, { 0x0a4, 0x42 }, { 0x110, 0x24 },
	{ 0x11c, 0x03 }, { 0x118, 0xb4 }, { 0x10c, 0x02 }, { 0x1bc, 0x11 }, { 0x0bc, 0x82 }, { 0x0d4, 0x03 }, { 0x0d0, 0x55 },
	{ 0x0cc, 0x55 }, { 0x0b0, 0x1a }, { 0x0ac, 0x0a }, { 0x0c4, 0x68 }, { 0x0e0, 0x02 }, { 0x0dc, 0xaa }, { 0x0d8, 0xab },
	{ 0x0b8, 0x34 }, { 0x0b4, 0x14 }, { 0x158, 0x01 }, { 0x074, 0x06 }, { 0x07c, 0x16 }, { 0x084, 0x36 }, { 0x078, 0x06 },
	{ 0x080, 0x16 }, { 0x088, 0x36 }, { 0x1b0, 0x1e }, { 0x1ac, 0xca }, { 0x1b8, 0x18 }, { 0x1b4, 0xa2 }, { 0x050, 0x07 },
	{ 0x010, 0x01 }, { 0x01c, 0x31 }, { 0x020, 0x01 }, { 0x024, 0xde }, { 0x028, 0x07 }, { 0x030, 0x4c }, { 0x034, 0x06 },
	{ 0x29c, 0x12 }, { 0x284, 0x35 }, { 0x23c, 0x11 }, { 0x51c, 0x03 }, { 0x518, 0x1c }, { 0x524, 0x1e }, { 0x4e8, 0x00 },
	{ 0x4ec, 0x0e }, { 0x4f0, 0x4a }, { 0x4f4, 0x0f }, { 0x5b4, 0x04 }, { 0x434, 0x7f }, { 0x444, 0x70 }, { 0x510, 0x17 },
	{ 0x4d4, 0x04 }, { 0x4d8, 0x07 }, { 0x598, 0xd4 }, { 0x59c, 0x54 }, { 0x5a0, 0xdb }, { 0x5a4, 0x3b }, { 0x5a8, 0x31 },
	{ 0x584, 0x24 }, { 0x588, 0xe4 }, { 0x58c, 0xec }, { 0x590, 0x3b }, { 0x594, 0x36 }, { 0x570, 0x3f }, { 0x574, 0x3f },
	{ 0x578, 0xff }, { 0x57c, 0x7f }, { 0x580, 0x14 }, { 0x4fc, 0x00 }, { 0x4f8, 0xc0 }, { 0x460, 0x30 }, { 0x464, 0x00 },
	{ 0x5bc, 0x0c }, { 0x4dc, 0x1b }, { 0x408, 0x0c }, { 0x414, 0x03 }, { 0x5b8, 0x30 }, { 0x9a4, 0x01 }, { 0xc90, 0x00 },
	{ 0xc40, 0x01 }, { 0xc48, 0x01 }, { 0xc50, 0x00 }, { 0xcb4, 0x33 }, { 0xcbc, 0x00 }, { 0xce0, 0x58 }, { 0xca4, 0x0f },
	{ 0x048, 0x90 }, { 0xc1c, 0xc1 }, { 0x988, 0x77 }, { 0x998, 0x0b }, { 0x8dc, 0x0d }, { 0x9ec, 0x12 }, { 0x800, 0x00 },
	{ 0x844, 0x03 },
};

static LCK_GRP_DECLARE(wifi_pcie_lck_grp, "instantnoodle-wifi");
static LCK_MTX_DECLARE(wifi_pcie_lock, &wifi_pcie_lck_grp);

static struct {
	vm_offset_t gcc52, gcc6b, gcc6c, gcc6f, gcc8c, gcc8d, gcc90;
	vm_offset_t parf, phy, dbi, iatu, conf;
	bool mapped;
	bool powered;                   /* the radio's supplies and WLAN_EN */
	bool clocked;                   /* PCIe 0's domain and clocks: its registers may be touched */
	volatile bool up;               /* the link: the device's configuration space may be touched */
	uint64_t bar_size;
	unsigned ups, failures;
} pcie;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
set_bits(vm_offset_t address, uint32_t clear, uint32_t set)
{
	wr(address, (rd(address) & ~clear) | set);
	(void)rd(address);              /* (pci-msm.c reads every write back: it has reached the block) */
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* A GCC register's mapping: the page it is in, mapped with the others once. */
static vm_offset_t
gcc(uint32_t offset)
{
	vm_offset_t page = (offset & ~0xfffu) == 0x52000 ? pcie.gcc52 : (offset & ~0xfffu) == 0x6b000 ? pcie.gcc6b :
	    (offset & ~0xfffu) == 0x6c000 ? pcie.gcc6c : (offset & ~0xfffu) == 0x6f000 ? pcie.gcc6f :
	    (offset & ~0xfffu) == 0x8c000 ? pcie.gcc8c : (offset & ~0xfffu) == 0x8d000 ? pcie.gcc8d : pcie.gcc90;

	return page + (offset & 0xfff);
}

static bool
map_all(void)
{
	if (pcie.mapped) {
		return true;
	}
	pcie.gcc52 = ml_io_map(GCC_PHYS + 0x52000, 0x1000);
	pcie.gcc6b = ml_io_map(GCC_PHYS + 0x6b000, 0x1000);
	pcie.gcc6c = ml_io_map(GCC_PHYS + 0x6c000, 0x1000);
	pcie.gcc6f = ml_io_map(GCC_PHYS + 0x6f000, 0x1000);
	pcie.gcc8c = ml_io_map(GCC_PHYS + 0x8c000, 0x1000);
	pcie.gcc8d = ml_io_map(GCC_PHYS + 0x8d000, 0x1000);
	pcie.gcc90 = ml_io_map(GCC_PHYS + 0x90000, 0x1000);
	pcie.parf = ml_io_map(PARF_PHYS, 0x3000);
	pcie.phy = ml_io_map(PHY_PHYS, 0x1000);
	pcie.dbi = ml_io_map(DBI_PHYS, 0x1000);
	pcie.iatu = ml_io_map(IATU_PHYS, 0x1000);
	pcie.conf = ml_io_map(CONF_PHYS, 0x1000);
	pcie.mapped = pcie.gcc52 && pcie.gcc6b && pcie.gcc6c && pcie.gcc6f && pcie.gcc8c && pcie.gcc8d && pcie.gcc90 && pcie.parf &&
	    pcie.phy && pcie.dbi && pcie.iatu && pcie.conf;
	if (!pcie.mapped) {
		say("wifi: pcie: the registers could not be mapped\n");
	}
	return pcie.mapped;
}

/* A branch clock on (its enable bit at `enable`) and running (its CBCR at `cbcr`, unless 0: the pipe clock, which the PHY makes). */
static bool
branch_on(const char *name, vm_offset_t enable, uint32_t bit, uint32_t cbcr)
{
	set_bits(enable, 0, bit);
	for (unsigned us = 0; cbcr != 0 && (rd(gcc(cbcr)) & CBCR_OFF); us += 10) {
		if (us > 2000) {
			say("wifi: pcie: the clock %s does not start (cbcr %#x)\n", name, rd(gcc(cbcr)));
			return false;
		}
		sleep_us(10);
	}
	return true;
}

/* An RCG to `source` divided by `half_divider` (2 x divider - 1, as gcc's tables have it). */
static bool
rcg_set(uint32_t rcg, uint32_t source, uint32_t half_divider)
{
	uint32_t cfg = rd(gcc(rcg + 4));

	if ((cfg & 0x71f) == (source << 8 | half_divider)) {
		return true;
	}
	wr(gcc(rcg + 4), (cfg & ~0x371fu) | source << 8 | half_divider);
	set_bits(gcc(rcg), 0, RCG_UPDATE);
	for (unsigned us = 0; rd(gcc(rcg)) & RCG_UPDATE; us += 10) {
		if (us > 2000) {
			say("wifi: pcie: the clock source at %#x does not take its configuration\n", rcg);
			return false;
		}
		sleep_us(10);
	}
	return true;
}

static void
reset_pulse(uint32_t bcr)
{
	wr(gcc(bcr), 1);
	sleep_us(1000);
	wr(gcc(bcr), 0);
	sleep_us(1000);
}

/* ---- power and clocks ------------------------------------------------------------------------------ */

static bool
radio_supplies(bool on)
{
	if (!on) {
		/* (only the radio's own: the others are shared, see the top of this file) */
		instantnoodle_supply("ldoc5", 1800, false);
		instantnoodle_supply("smpf2", 950, false);
		instantnoodle_supply("smpa6", 950, false);
		return true;
	}
	return instantnoodle_supply("smpa6", 950, true) && instantnoodle_supply("smpf2", 950, true) &&
	       instantnoodle_supply("smpa5", 1900, true) && instantnoodle_supply("smpc8", 1350, true) &&
	       instantnoodle_supply("ldoc5", 1800, true) && instantnoodle_supply("ldoa9", 1200, true) &&
	       instantnoodle_supply("ldoa5", 880, true);
}

static void
clocks_off(void)
{
	set_bits(gcc(GCC_VOTE), VOTES_ALL, 0);
	set_bits(gcc(GCC_PCIE0_REFGEN_CBCR), 1, 0);
	set_bits(gcc(GCC_PCIE_WIFI_CLKREF), 1, 0);
	set_bits(gcc(GCC_DDRSS_PCIE_SF_TBU), 1, 0);
	set_bits(gcc(GCC_AGGRE_NOC_PCIE_TBU), 1, 0);
	sleep_us(100);
	set_bits(gcc(GCC_PCIE_0_GDSCR), 0, GDSC_COLLAPSE);
	pcie.clocked = false;
	instantnoodle_bcm_vote("SN12", 0, 0);
}

static bool
clocks_on(void)
{
	uint32_t gdsc = rd(gcc(GCC_PCIE_0_GDSCR));

	say("wifi: pcie: PCIe 0's domain %#x, its votes %#x, aux %#x, cfg %#x: power and clocks on\n", gdsc, rd(gcc(GCC_VOTE)),
	    rd(gcc(GCC_PCIE_0_AUX_CBCR)), rd(gcc(GCC_PCIE_0_CFG_AHB_CBCR)));
	instantnoodle_bcm_vote("SN12", 1, 1);
	set_bits(gcc(GCC_PCIE_0_GDSCR), GDSC_COLLAPSE, 0);
	for (unsigned us = 0; !(rd(gcc(GCC_PCIE_0_GDSCR)) & GDSC_ON); us += 10) {
		if (us > 5000) {
			say("wifi: pcie: PCIe 0's power domain does not come on (%#x)\n", rd(gcc(GCC_PCIE_0_GDSCR)));
			return false;
		}
		sleep_us(10);
	}
	pcie.clocked = true;
	/* aux 19.2 MHz from the crystal; the PHY's reference 100 MHz from GPLL0 (600 / 6) */
	if (!rcg_set(GCC_PCIE_0_AUX_RCG, 0, 1) || !rcg_set(GCC_PCIE_REFGEN_RCG, 1, 11)) {
		return false;
	}
	if (!branch_on("aggre noc pcie tbu", gcc(GCC_AGGRE_NOC_PCIE_TBU), 1, GCC_AGGRE_NOC_PCIE_TBU) ||
	    !branch_on("ddrss pcie sf tbu", gcc(GCC_DDRSS_PCIE_SF_TBU), 1, GCC_DDRSS_PCIE_SF_TBU) ||
	    !branch_on("pcie wifi clkref", gcc(GCC_PCIE_WIFI_CLKREF), 1, GCC_PCIE_WIFI_CLKREF) ||
	    !branch_on("pcie0 phy refgen", gcc(GCC_PCIE0_REFGEN_CBCR), 1, GCC_PCIE0_REFGEN_CBCR) ||
	    !branch_on("pcie_0_aux", gcc(GCC_VOTE), 1u << 3, GCC_PCIE_0_AUX_CBCR) ||
	    !branch_on("pcie_0_cfg_ahb", gcc(GCC_VOTE), 1u << 2, GCC_PCIE_0_CFG_AHB_CBCR) ||
	    !branch_on("pcie_0_mstr_axi", gcc(GCC_VOTE), 1u << 1, GCC_PCIE_0_MSTR_AXI_CBCR) ||
	    !branch_on("pcie_0_slv_axi", gcc(GCC_VOTE), 1u << 0, GCC_PCIE_0_SLV_AXI_CBCR) ||
	    !branch_on("pcie_0_slv_q2a_axi", gcc(GCC_VOTE), 1u << 5, GCC_PCIE_0_SLV_Q2A_CBCR)) {
		return false;
	}
	reset_pulse(GCC_PCIE_0_BCR);
	reset_pulse(GCC_PCIE_0_PHY_BCR);
	return true;
}

/* ---- the link ------------------------------------------------------------------------------------ */

/* CRC-8, polynomial 0x07, most significant bit first: the controller's hash of a BDF (big endian) into its SID table. */
static uint8_t
bdf_hash(uint16_t bdf)
{
	uint8_t bytes[2] = { (uint8_t)(bdf >> 8), (uint8_t)bdf }, crc = 0;

	for (unsigned b = 0; b < 2; b++) {
		crc ^= bytes[b];
		for (unsigned i = 0; i < 8; i++) {
			crc = (uint8_t)(crc & 0x80 ? crc << 1 ^ 0x07 : crc << 1);
		}
	}
	return crc;
}

/* The SMMU streams of the bus's two functions (dt iommu-map: the bridge 0x1c00, the radio 0x1c01), as pci-msm.c enters them. */
static void
sid_table(void)
{
	static const uint16_t bdfs[2] = { 0x0000, EP_BDF };

	for (unsigned i = 0; i < 256; i++) {
		wr(pcie.parf + PARF_BDF_TO_SID + 4 * i, 0);
	}
	for (unsigned i = 0; i < 2; i++) {
		uint8_t hash = bdf_hash(bdfs[i]);

		/* a collision is chained to the next free entry: BDF [31:16] | SID [15:8] | NEXT [7:0] */
		while (rd(pcie.parf + PARF_BDF_TO_SID + 4u * hash) != 0) {
			uint32_t entry = rd(pcie.parf + PARF_BDF_TO_SID + 4u * hash);
			uint8_t following = (uint8_t)(hash + 1);

			if ((entry & 0xff) == 0) {
				wr(pcie.parf + PARF_BDF_TO_SID + 4u * hash, entry | following);
			}
			hash = following;
		}
		wr(pcie.parf + PARF_BDF_TO_SID + 4u * hash, (uint32_t)bdfs[i] << 16 | (uint32_t)i << 8);
	}
}

static bool
link_train(void)
{
	uint32_t status = 0, link = 0;

	set_bits(pcie.dbi + DBI_GEN2_CTRL, 0x1f00, 1u << 8);           /* one lane */
	set_bits(pcie.dbi + DBI_GEN3_RELATED, 1, 0);
	set_bits(pcie.dbi + DBI_MISC_CONTROL, 0, 1);                    /* (read-only registers writable) */
	wr(pcie.dbi + DBI_SPCIE_CAP, 0x55555555);                       /* the Gen3 presets */
	set_bits(pcie.dbi + DBI_MISC_CONTROL, 1, 0);
	set_bits(pcie.dbi + DBI_LINK_CONTROL2, 0xf, 2);                 /* target: 5 GT/s (dt qcom,target-link-speed) */
	set_bits(pcie.dbi + DBI_DEVICE_CONTROL, 0x7000, 5u << 12);      /* the largest read request: 4 KB */
	set_bits(pcie.parf + PARF_LTSSM, 0, 1u << 8);
	for (unsigned tries = 0; tries <= 20; tries++) {
		sleep_us(5000);
		status = rd(pcie.dbi + ELBI_SYS_STTS);
		link = rd(pcie.dbi + DBI_LINK_STATUS);
		if ((status & XMLH_LINK_UP) && (link & 1u << 29) && rd(pcie.dbi) != 0xffffffffu) {
			say("wifi: pcie: THE LINK IS UP after %u ms: %s, x%u (LTSSM %#x)\n", 5 * (tries + 1), (link >> 16 & 0xf) == 1 ?
			    "2.5 GT/s" : (link >> 16 & 0xf) == 2 ? "5 GT/s" : "8 GT/s", link >> 20 & 0x3f, status >> 12 & 0x3f);
			return true;
		}
	}
	say("wifi: pcie: the link does not come up (ELBI status %#x, LTSSM %#x, link status %#x)\n", status, status >> 12 & 0x3f, link);
	return false;
}

static void
iatu_config_window(void)
{
	wr(pcie.iatu + IATU_CTRL2, 0);
	wr(pcie.iatu + IATU_CTRL1, 0x4);                                /* configuration requests, type 0 */
	wr(pcie.iatu + IATU_LBAR, (uint32_t)CONF_PHYS);
	wr(pcie.iatu + IATU_UBAR, 0);
	wr(pcie.iatu + IATU_LAR, (uint32_t)CONF_PHYS + 0xfff);
	wr(pcie.iatu + IATU_LTAR, (uint32_t)EP_BDF << 16);             /* to bus 1, device 0 */
	wr(pcie.iatu + IATU_UTAR, 0);
	wr(pcie.iatu + IATU_CTRL2, 1u << 31);
}

/* The bridge's bus numbers and window, and the device's BAR 0 placed in it. */
static bool
enumerate(void)
{
	uint32_t ids, bar, size_mask;
	bool wide;

	for (unsigned ms = 0; (ids = rd(pcie.conf)) == 0xffffffffu; ms++) {
		if (ms > 1000) {
			say("wifi: pcie: the device's configuration space does not answer\n");
			return false;
		}
		sleep_us(1000);
	}
	wr(pcie.dbi + 0x18, 0x00010100);                                /* buses: primary 0, secondary and subordinate 1 */
	wr(pcie.dbi + 0x1c, 0x000000f0);                                /* no I/O window */
	wr(pcie.dbi + 0x20, (uint32_t)((BAR_PHYS + BAR_WINDOW - 1) >> 16 & 0xfff0) << 16 | (uint32_t)(BAR_PHYS >> 16 & 0xfff0));
	wr(pcie.dbi + 0x24, 0x0000fff0);                                /* no prefetchable window */
	set_bits(pcie.dbi + 0x04, 0, 0x6);                              /* memory, bus master */

	/* the device's BAR 0: sized, and placed at the window's start */
	bar = rd(pcie.conf + 0x10);
	wide = (bar & 0x6) == 0x4;
	wr(pcie.conf + 0x10, 0xffffffffu);
	size_mask = rd(pcie.conf + 0x10) & ~0xfu;
	pcie.bar_size = size_mask == 0 ? 0 : (uint64_t)(~size_mask) + 1;
	if (pcie.bar_size == 0 || pcie.bar_size > BAR_WINDOW) {
		say("wifi: pcie: the device's BAR 0 is not one this places (%#x, size %#llx)\n", bar, pcie.bar_size);
		return false;
	}
	wr(pcie.conf + 0x10, (uint32_t)BAR_PHYS | (bar & 0xf));
	if (wide) {
		wr(pcie.conf + 0x14, 0);
	}
	say("wifi: pcie: device %04x:%04x behind the bridge; its BAR 0 (%s) at %#lx, %llu KB\n", ids & 0xffff, ids >> 16,
	    wide ? "64-bit" : "32-bit", BAR_PHYS, pcie.bar_size >> 10);
	return true;
}

/* PCIe 0 off: PERST, the PHY down, its clocks and domain. */
static void
pcie_off(void)
{
	pcie.up = false;
	instantnoodle_gpio_output(PIN_PERST, false, INSTANTNOODLE_PULL_DOWN, 2);
	if (pcie.clocked) {
		wr(pcie.phy + PHY_POWER_DOWN, 0);
		set_bits(pcie.parf + PARF_PHY_CTRL, 0, 1);
		clocks_off();
	}
}

static bool
pcie_on(void)
{
	uint32_t ready = 0;

	/* the device in reset while everything comes up */
	instantnoodle_gpio_output(PIN_PERST, false, INSTANTNOODLE_PULL_DOWN, 2);
	instantnoodle_gpio_function(PIN_CLKREQ, FUNCTION_PCI_E0, INSTANTNOODLE_PULL_UP, 2);
	instantnoodle_gpio_input(PIN_WAKE, INSTANTNOODLE_PULL_UP);
	sleep_us(1000);
	if (!clocks_on()) {
		return false;
	}
	say("wifi: pcie: PARF %#x, PHY status %#x: the controller set up as a root complex\n", rd(pcie.parf + PARF_SYS_CTRL),
	    rd(pcie.phy + PHY_STATUS));
	wr(pcie.parf + PARF_DEVICE_TYPE, 0x4);
	set_bits(pcie.parf + PARF_PM_CTRL, 1u << 5, 0);                 /* L1 allowed */
	set_bits(pcie.parf + PARF_PHY_CTRL, 1, 0);                      /* the PHY powered */
	wr(pcie.parf + PARF_DBI_BASE_ADDR, 0);
	wr(pcie.parf + PARF_SYS_CTRL, 0x365e);
	set_bits(pcie.parf + PARF_MHI_CLOCK_RESET_CTRL, 0, 1u << 4);
	wr(pcie.parf + PARF_INT_ALL_MASK, 0);                           /* (the controller's own interrupts are not taken) */
	wr(pcie.parf + PARF_SLV_ADDR_SPACE_SIZE, 0x4000000);
	set_bits(pcie.parf + PARF_AXI_MSTR_WR_ADDR_HALT, 0, 1u << 31);

	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(pcie.phy + phy_sequence[i][0], phy_sequence[i][1]);
		(void)rd(pcie.phy + phy_sequence[i][0]);
	}
	sleep_us(1000);
	branch_on("pcie_0_pipe", gcc(GCC_VOTE), 1u << 4, 0);
	for (unsigned tries = 0; tries <= 10 && ((ready = rd(pcie.phy + PHY_STATUS)) & 1u << 6); tries++) {
		sleep_us(1000);
	}
	if (ready & 1u << 6) {
		say("wifi: pcie: the PHY is not ready (status %#x)\n", ready);
		return false;
	}
	say("wifi: pcie: the PHY is ready\n");
	sleep_us(10000);                                                /* dt qcom,ep-latency */
	instantnoodle_gpio_output(PIN_PERST, true, INSTANTNOODLE_PULL_DOWN, 2);
	sleep_us(1000);
	if (!link_train()) {
		return false;
	}
	sid_table();
	iatu_config_window();
	set_bits(pcie.dbi + DBI_ACK_F_ASPM_CTRL, 0, 1u << 15);
	wr(pcie.dbi + DBI_AUX_CLK_FREQ, 0x14);                          /* the aux clock is 19.2 MHz */
	if (!enumerate()) {
		return false;
	}
	pcie.up = true;
	return true;
}

/* ---- for hk_wifi.c --------------------------------------------------------------------------------- */

bool
instantnoodle_wifi_power(bool on)
{
	bool done = true;

	lck_mtx_lock(&wifi_pcie_lock);
	if (!map_all()) {
		lck_mtx_unlock(&wifi_pcie_lock);
		return false;
	}
	if (on && !pcie.up) {
		say("wifi: pcie: WLAN_EN %#x, PERST %#x, CLKREQ %#x: the radio's supplies, then WLAN_EN\n",
		    instantnoodle_gpio_cfg(PIN_WLAN_EN), instantnoodle_gpio_cfg(PIN_PERST), instantnoodle_gpio_cfg(PIN_CLKREQ));
		done = radio_supplies(true);
		if (done) {
			instantnoodle_gpio_output(PIN_WLAN_EN, true, INSTANTNOODLE_PULL_UP, 16);
			sleep_us(1000);
			pcie.powered = true;
			done = pcie_on();
		}
		if (!done) {
			pcie.failures++;
			pcie_off();
			instantnoodle_gpio_output(PIN_WLAN_EN, false, INSTANTNOODLE_PULL_DOWN, 2);
			radio_supplies(false);
			pcie.powered = false;
		} else {
			pcie.ups++;
		}
	} else if (!on && (pcie.up || pcie.powered || pcie.clocked)) {
		pcie_off();
		instantnoodle_gpio_output(PIN_WLAN_EN, false, INSTANTNOODLE_PULL_DOWN, 2);
		radio_supplies(false);
		pcie.powered = false;
		say("wifi: pcie: the link is down and the radio off\n");
	}
	lck_mtx_unlock(&wifi_pcie_lock);
	return done;
}

/* The device's configuration space; all ones while the link is down (and nothing touched). */
uint32_t
instantnoodle_wifi_config_read(uint32_t offset, unsigned width)
{
	vm_offset_t at = pcie.conf + offset;

	if (!pcie.up || offset + width > 0x1000 || (offset & (width - 1)) != 0) {
		return width == 4 ? 0xffffffffu : (1u << (8 * width)) - 1;
	}
	return width == 1 ? *(volatile uint8_t *)at : width == 2 ? *(volatile uint16_t *)at : *(volatile uint32_t *)at;
}

void
instantnoodle_wifi_config_write(uint32_t offset, uint32_t value, unsigned width)
{
	vm_offset_t at = pcie.conf + offset;

	if (!pcie.up || offset + width > 0x1000 || (offset & (width - 1)) != 0) {
		return;
	}
	if (width == 1) {
		*(volatile uint8_t *)at = (uint8_t)value;
	} else if (width == 2) {
		*(volatile uint16_t *)at = (uint16_t)value;
	} else {
		*(volatile uint32_t *)at = value;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

bool
instantnoodle_wifi_bar(unsigned bar, uint64_t *physical, uint64_t *size)
{
	if (bar != 0 || pcie.bar_size == 0) {
		return false;
	}
	*physical = BAR_PHYS;
	*size = pcie.bar_size;
	return true;
}

/*
 * On a panic's way back to the loader, one processor left: the device held in reset and the link
 * down, so that nothing writes to memory the next kernel will have for something else. No lock is
 * taken, nothing sleeps that is not a loop.
 */
void
instantnoodle_wifi_quiesce(void)
{
	if (!pcie.mapped || !(pcie.up || pcie.clocked)) {
		return;
	}
	pcie.up = false;
	instantnoodle_gpio_output(PIN_PERST, false, INSTANTNOODLE_PULL_DOWN, 2);
	instantnoodle_gpio_output(PIN_WLAN_EN, false, INSTANTNOODLE_PULL_DOWN, 2);
}
