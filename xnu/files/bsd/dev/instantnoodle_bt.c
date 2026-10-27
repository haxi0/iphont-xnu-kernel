/*
 * The OnePlus 8's Bluetooth (instantnoodle, SM8250): the Bluetooth half of the QCA6390 -- the
 * radio whose Wi-Fi half is on PCIe (instantnoodle_wifi.c) -- and the UART it talks on, serial
 * engine 6 of the SoC's first QUP (qupv3_se6_4uart, qcom,qup_uart@998000). This file powers the
 * chip's Bluetooth side and moves bytes; what the bytes say (HCI, the chip's own commands to load
 * its patch and configuration) is bsd/dev/hk_bt.c's.
 *
 * The numbers are the phone's own (its device tree: bt_qca6390, qcom,qup_uart@998000,
 * qupv3_se6_4uart_pins, bt_en_sleep), the register layouts Linux's (drivers/tty/serial/
 * qcom_geni_serial.c, drivers/soc/qcom/qcom-geni-se.c, include/linux/soc/qcom/geni-se.h,
 * drivers/clk/qcom/gcc-sm8250.c), read as data; the power order Linux's for this chip
 * (drivers/power/sequencing/pwrseq-qcom-wcn.c, "qcom,qca6390-pmu": the enables of the chip's two
 * halves at least 100 ms apart, 60 ms after the last for it to come up).
 *
 *   power    the chip's supplies, shared with its Wi-Fi half (instantnoodle_qca6390_supplies():
 *            pm8150 S6 0.95 V aon, pm8009 S2 0.95 V dig, pm8150 S5 1.9 V rfa1, pm8150a S8
 *            1.35 V rfa2) and the Bluetooth half's own antenna switch supply, pm8150 L16 3.0 V
 *            ("asd"); then BT_EN (gpio21) high. SW_CTRL (gpio124) is the chip's own output: read
 *            and told, never driven.
 *   clocks   QUP 0's AHB master and slave and core clocks and engine 6's own, by vote (GCC
 *            0x52008 bits 6-9 and 16); the engine's rate is one of the levels its clock
 *            generator has preset (DFS), picked by the engine's GENI_CLK_SEL.
 *   pins     gpio16 CTS, 17 RTS, 18 TX, 19 RX to function 1 (qup6), as the tree's "active" state.
 *   engine   UART firmware in the serial engine (protocol 2), FIFO mode: four bytes a word, 8N1,
 *            RTS/CTS by the engine; reading is one long command of the secondary sequencer whose
 *            FIFO watermark and "stale" interrupts (SPI 607) move what came into a ring; writing
 *            is a command of the primary sequencer per write, its FIFO filled as it empties
 *            (polled: the interrupt is the receiver's only).
 *
 * Nothing here is touched unless hk_bt.c asks (bt=1 on board=instantnoodle). A register of the
 * engine is not read before its clocks are voted on: as with every block of this SoC, a read of an
 * unclocked one is a NoC error and Qualcomm's crash dump mode. bt_probe=1: what the firmware left
 * (the engine's protocol and FIFO, the clock levels, the pins), read only, at hk_bt.c's start.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/instantnoodle.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void hk_bt_say(const char *format, ...) __printflike(1, 2);

#define say hk_bt_say

#define PIN(n)                  ((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_NORTH, (n) })
#define PIN_CTS                 PIN(16)
#define PIN_RTS                 PIN(17)
#define PIN_TX                  PIN(18)
#define PIN_RX                  PIN(19)
#define PIN_BT_EN               PIN(21)
#define PIN_SW_CTRL             PIN(124)
#define FUNCTION_QUP6           1

/* GCC (Linux gcc-sm8250.c): the pages this file maps, and the registers in them */
#define GCC_PHYS                0x00100000UL
#define GCC_VOTE                0x52008         /* the application processor's votes for branch clocks */
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_SE6           (1u << 16)
#define VOTES_ALL               (VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB | VOTE_QUP0_CORE | VOTE_QUP0_CORE_2X | VOTE_QUP0_SE6)
#define GCC_QUP0_M_AHB_CBCR     0x17004         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     0x17008
#define GCC_QUP0_CORE_CBCR      0x23000
#define GCC_QUP0_CORE_2X_CBCR   0x23008
#define GCC_QUP0_SE6_CBCR       0x1772c
#define GCC_QUP0_SE6_CMD        0x17730         /* the engine's clock generator (RCG) */
#define GCC_QUP0_SE6_CFG        0x17734         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode, 20 hardware control */
#define GCC_QUP0_SE6_M          0x17738
#define GCC_QUP0_SE6_N          0x1773c         /* ~(n - m) */
#define GCC_QUP0_SE6_D          0x17740         /* ~2d */
#define GCC_QUP0_SE6_DFSR       0x17744         /* bit 0: the rate is one of eight presets, picked by the engine */
#define GCC_QUP0_SE6_PERF_CFG(l) (0x1774c + 4 * (l))
#define GCC_QUP0_SE6_PERF_M(l)  (0x1778c + 4 * (l))
#define GCC_QUP0_SE6_PERF_N(l)  (0x177cc + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define CLOCK_HW_CTL            (1u << 1)       /* the branch gated by the hardware: its "off" bit says nothing */
#define XO_HZ                   19200000u

/* QUP 0 (the wrapper) and its serial engine 6 */
#define QUP0_PHYS               0x009c0000UL
#define QUPV3_HW_VER            0x4             /* 0x20050000 and up: the UART samples 16 times a bit, not 32 */
#define SE_PHYS                 0x00998000UL
#define SE_SIZE                 0x4000UL
#define SE_INTID                (607 + 32)      /* dt: interrupts-extended <GIC SPI 607 level> */

#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define GENI_STATUS             0x040           /* bit 0: the primary sequencer is busy, bit 12: the secondary */
#define GENI_SER_M_CLK_CFG      0x048           /* bit 0 enable, [15:4] divider */
#define GENI_SER_S_CLK_CFG      0x04c
#define GENI_IF_DISABLE_RO      0x064           /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     0x068           /* [15:8] protocol: 2 = UART */
#define GENI_CLK_SEL            0x07c
#define UART_LOOPBACK_CFG       0x22c
#define UART_TX_TRANS_CFG       0x25c           /* bit 1: CTS ignored */
#define UART_TX_WORD_LEN        0x268
#define UART_TX_STOP_BIT_LEN    0x26c
#define UART_TX_TRANS_LEN       0x270
#define UART_RX_TRANS_CFG       0x280
#define UART_RX_WORD_LEN        0x28c
#define UART_RX_STALE_CNT       0x294
#define UART_TX_PARITY_CFG      0x2a4
#define UART_RX_PARITY_CFG      0x2a8
#define UART_MANUAL_RFR         0x2ac           /* 0: RFR (our RTS) by the engine's RX FIFO */
#define GENI_BYTE_GRAN          0x254
#define GENI_DMA_MODE_EN        0x258
#define GENI_TX_PACKING_CFG0    0x260
#define GENI_TX_PACKING_CFG1    0x264
#define GENI_RX_PACKING_CFG0    0x284
#define GENI_RX_PACKING_CFG1    0x288
#define GENI_M_CMD0             0x600           /* [31:27] operation: 1 = start TX */
#define GENI_M_CMD_CTRL         0x604           /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_S_CMD0             0x630           /* [31:27] operation: 1 = start RX */
#define GENI_S_CMD_CTRL         0x634
#define GENI_S_IRQ_STATUS       0x640
#define GENI_S_IRQ_EN           0x644
#define GENI_S_IRQ_CLEAR        0x648
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_TX_FIFO_STATUS     0x800           /* [27:0] words in it */
#define GENI_RX_FIFO_STATUS     0x804           /* bit 31 last, [30:28] valid bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       0x80c
#define GENI_RX_WATERMARK       0x810
#define GENI_RX_RFR_WATERMARK   0x814
#define GENI_IOS                0x908           /* bit 0: the RX line, bit 1: CTS */
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c           /* bit 2: the primary's interrupts reach the GIC, bit 3: the secondary's */
#define SE_HW_PARAM_0           0xe24           /* TX FIFO: [29:24] width in bits, [21:16] depth in words */
#define SE_HW_PARAM_1           0xe28           /* RX FIFO, the same */
#define SE_DMA_GENERAL_CFG      0xe30

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_TX_FIFO_WATERMARK     (1u << 30)
#define S_CMD_CANCEL            (1u << 4)
#define S_CMD_ABORT             (1u << 5)
#define S_RX_FIFO_WR_ERR        (1u << 25)
#define S_RX_FIFO_WATERMARK     (1u << 26)
#define S_RX_FIFO_LAST          (1u << 27)
#define S_PARITY_BREAK          (0xfu << 9)     /* GP IRQ 0-3: parity, framing, break */
#define PROTOCOL_UART           2

static vm_offset_t gcc_vote_page, gcc_qup_page, gcc_core_page, qup, se;
static uint32_t fifo_depth_tx, fifo_depth_rx, sampling = 32, current_baud;
static bool powered, open, irq_attached;

static LCK_GRP_DECLARE(bt_lck_grp, "instantnoodle-bt");
static LCK_MTX_DECLARE(bt_tx_lock, &bt_lck_grp);

/* What came in: a ring the interrupt fills and the reader empties (one of each). */
#define RING_SIZE               16384u
static uint8_t ring[RING_SIZE];
static volatile uint32_t ring_head, ring_tail;  /* free running; head the interrupt's, tail the reader's */
static volatile uint32_t rx_irqs, rx_overruns, rx_dropped, rx_errors;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static inline uint32_t
gcc(uint32_t offset)
{
	vm_offset_t page = (offset & ~0xfffu) == 0x52000 ? gcc_vote_page : (offset & ~0xfffu) == 0x17000 ? gcc_qup_page : gcc_core_page;

	return rd(page + (offset & 0xfff));
}

static inline void
gcc_wr(uint32_t offset, uint32_t value)
{
	vm_offset_t page = (offset & ~0xfffu) == 0x52000 ? gcc_vote_page : (offset & ~0xfffu) == 0x17000 ? gcc_qup_page : gcc_core_page;

	wr(page + (offset & 0xfff), value);
}

static inline uint32_t
se_rd(uint32_t offset)
{
	return rd(se + offset);
}

static inline void
se_wr(uint32_t offset, uint32_t value)
{
	wr(se + offset, value);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* Small pages only: the IO submap is 8 MB for the kernel's whole life (hardware-qualcomm.md §1). */
static bool
map_all(void)
{
	if (se != 0) {
		return true;
	}
	gcc_vote_page = ml_io_map(GCC_PHYS + 0x52000, PAGE_SIZE);
	gcc_qup_page = ml_io_map(GCC_PHYS + 0x17000, PAGE_SIZE);
	gcc_core_page = ml_io_map(GCC_PHYS + 0x23000, PAGE_SIZE);
	qup = ml_io_map(QUP0_PHYS, PAGE_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	return gcc_vote_page != 0 && gcc_qup_page != 0 && gcc_core_page != 0 && qup != 0 && se != 0;
}

/* ---- clocks --------------------------------------------------------------------------------------- */

static bool
clocks_on(void)
{
	gcc_wr(GCC_VOTE, gcc(GCC_VOTE) | VOTES_ALL);
	for (int i = 0; i < 1000; i++) {
		uint32_t s_ahb = gcc(GCC_QUP0_S_AHB_CBCR);

		if (!((gcc(GCC_QUP0_M_AHB_CBCR) | ((s_ahb & CLOCK_HW_CTL) ? 0 : s_ahb) | gcc(GCC_QUP0_CORE_CBCR) |
		    gcc(GCC_QUP0_CORE_2X_CBCR) | gcc(GCC_QUP0_SE6_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	say("bt: QUP 0's clocks stay off: m_ahb %#x s_ahb %#x core %#x core2x %#x se6 %#x\n", gcc(GCC_QUP0_M_AHB_CBCR),
	    gcc(GCC_QUP0_S_AHB_CBCR), gcc(GCC_QUP0_CORE_CBCR), gcc(GCC_QUP0_CORE_2X_CBCR), gcc(GCC_QUP0_SE6_CBCR));
	return false;
}

/* Only the engine's own: QUP 0's AHB and core clocks may be other engines' too (the firmware's console, SE2). */
static void
clocks_off(void)
{
	gcc_wr(GCC_VOTE, gcc(GCC_VOTE) & ~VOTE_QUP0_SE6);
}

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };   /* XO, GPLL0, GPLL0 even */
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static uint32_t
level_rate(uint32_t level)
{
	return rcg_rate(gcc(GCC_QUP0_SE6_PERF_CFG(level)), gcc(GCC_QUP0_SE6_PERF_M(level)), gcc(GCC_QUP0_SE6_PERF_N(level)));
}

/*
 * The engine's clock generator set to one of the rates Linux's table for it has (gcc-sm8250.c,
 * ftbl_gcc_qupv3_wrap0_s2_clk_src: source, divider, M/N), the firmware having left it on the crystal
 * alone and not in its preset (DFS) mode: for `baud' the slowest rate that is a whole multiple of baud
 * x sampling, the engine's divider the rest (geni_serial_set_rate()). The update is waited for; until
 * it is taken nothing of the engine is touched. false: no rate fits, or the generator did not take it.
 */
static const struct {
	uint32_t hz, source, divider, m, n;     /* source: 0 the crystal, 1 GPLL0, 6 GPLL0's even output (300 MHz) */
} rates[] = {
	{ 7372800, 6, 1, 384, 15625 }, { 14745600, 6, 1, 768, 15625 }, { 19200000, 0, 1, 0, 0 },
	{ 29491200, 6, 1, 1536, 15625 }, { 32000000, 6, 1, 8, 75 }, { 48000000, 6, 1, 4, 25 },
	{ 64000000, 6, 1, 16, 75 }, { 80000000, 6, 1, 4, 15 }, { 96000000, 6, 1, 8, 25 },
};

static bool
rcg_set(unsigned i)
{
	uint32_t cfg = rates[i].divider << 0 | rates[i].source << 8;

	if (rates[i].n != 0 && rates[i].m != rates[i].n) {
		uint32_t m = rates[i].m, n = rates[i].n, d = n;

		if (d > 2 * (n - m)) {
			d = 2 * (n - m);
		}
		if (d < m) {
			d = m;
		}
		gcc_wr(GCC_QUP0_SE6_M, m);
		gcc_wr(GCC_QUP0_SE6_N, ~(n - m) & 0xffff);
		gcc_wr(GCC_QUP0_SE6_D, ~d & 0xffff);
		cfg |= 2u << 12;                /* dual edge: the M/N counter in */
	}
	gcc_wr(GCC_QUP0_SE6_CFG, (gcc(GCC_QUP0_SE6_CFG) & ~(0x1fu | 7u << 8 | 3u << 12 | 1u << 20)) | cfg);
	gcc_wr(GCC_QUP0_SE6_CMD, gcc(GCC_QUP0_SE6_CMD) | 1);
	for (int t = 0; t < 500 && (gcc(GCC_QUP0_SE6_CMD) & 1); t++) {
		sleep_us(2);
	}
	if (gcc(GCC_QUP0_SE6_CMD) & 1) {
		say("bt: the engine's clock generator does not take %u Hz (cmd %#x cfg %#x)\n", rates[i].hz, gcc(GCC_QUP0_SE6_CMD),
		    gcc(GCC_QUP0_SE6_CFG));
		return false;
	}
	return true;
}

static bool
set_rate(uint32_t baud)
{
	uint32_t want = baud * sampling;

	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		uint32_t div = rates[i].hz / want;

		if (rates[i].hz % want != 0 || div == 0 || div > 0xfff) {
			continue;
		}
		if (!rcg_set(i)) {
			return false;
		}
		se_wr(GENI_SER_M_CLK_CFG, div << 4 | 1);
		se_wr(GENI_SER_S_CLK_CFG, div << 4 | 1);
		se_wr(GENI_CLK_SEL, 0);
		current_baud = baud;
		return true;
	}
	say("bt: no clock rate gives %u baud (x%u)\n", baud, sampling);
	return false;
}

/* ---- the engine ----------------------------------------------------------------------------------- */

static void
rx_stop(void)
{
	se_wr(GENI_S_IRQ_EN, se_rd(GENI_S_IRQ_EN) & ~(S_RX_FIFO_WATERMARK | S_RX_FIFO_LAST));
	if (!(se_rd(GENI_STATUS) & (1u << 12))) {
		return;
	}
	se_wr(GENI_S_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 1000 && !(se_rd(GENI_S_IRQ_STATUS) & S_CMD_CANCEL); i++) {
		sleep_us(10);
	}
	if (se_rd(GENI_STATUS) & (1u << 12)) {
		se_wr(GENI_S_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 1000 && !(se_rd(GENI_S_IRQ_STATUS) & S_CMD_ABORT); i++) {
			sleep_us(10);
		}
	}
	se_wr(GENI_S_IRQ_CLEAR, 0xffffffff);
}

/*
 * The secondary sequencer's long read, with RFR held open (Android's msm_geni_serial.c for this engine:
 * UART_PARAM_RFR_OPEN; without it the end of a packet stayed in the engine until the next one came).
 */
static void
rx_start(void)
{
	se_wr(GENI_S_CMD0, 1u << 27 | 1u << 7);
	se_wr(GENI_S_IRQ_EN, se_rd(GENI_S_IRQ_EN) | S_RX_FIFO_WATERMARK | S_RX_FIFO_LAST | S_RX_FIFO_WR_ERR | S_PARITY_BREAK);
}

static void
tx_recover(void)
{
	se_wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 1000 && !(se_rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(10);
	}
	if (se_rd(GENI_STATUS) & 1) {
		se_wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 1000 && !(se_rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(10);
		}
	}
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* the FIFO's words into the ring: from the interrupt, and from the reader when no interrupt came */
static void
drain(void)
{
	uint32_t status = se_rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes;

	if (words == 0) {
		return;
	}
	bytes = 4 * (words - 1) + ((status & (1u << 31)) && (status >> 28 & 7) ? status >> 28 & 7 : 4);
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = se_rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			uint32_t head = ring_head;

			if (head - ring_tail >= RING_SIZE) {
				rx_dropped++;
				continue;
			}
			ring[head % RING_SIZE] = (uint8_t)(word >> (8 * j));
			__atomic_store_n(&ring_head, head + 1, __ATOMIC_RELEASE);
		}
	}
}

static void
se_interrupt(__unused void *context, __unused uint32_t intid)
{
	uint32_t status = se_rd(GENI_S_IRQ_STATUS);

	se_wr(GENI_S_IRQ_CLEAR, status);
	se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	rx_irqs++;
	if (status & S_RX_FIFO_WR_ERR) {
		rx_overruns++;
	}
	if (status & S_PARITY_BREAK) {
		rx_errors++;
	}
	if (status & (S_RX_FIFO_WATERMARK | S_RX_FIFO_LAST)) {
		drain();
	}
	thread_wakeup((event_t)&ring_head);
}

static const char *
engine_setup(void)
{
	uint32_t proto = se_rd(GENI_FW_REVISION_RO) >> 8 & 0xff;

	if (proto != PROTOCOL_UART) {
		return proto == 0 ? "the serial engine has no firmware" : "the serial engine's firmware is not the UART's";
	}
	if (se_rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled (DMA only)";
	}
	fifo_depth_tx = se_rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	fifo_depth_rx = se_rd(SE_HW_PARAM_1) >> 16 & 0x3f;
	if (fifo_depth_tx < 4 || fifo_depth_rx < 4) {
		return "the serial engine reports no FIFO";
	}
	sampling = rd(qup + QUPV3_HW_VER) >= 0x20050000 ? 16 : 32;
	rx_stop();
	if (se_rd(GENI_STATUS) & 1) {
		tx_recover();
	}
	se_wr(UART_RX_STALE_CNT, 10 * 16);
	/* eight bits a character, four characters a FIFO word, the first in the low byte (geni_se_config_packing) */
	se_wr(GENI_TX_PACKING_CFG0, 0x0004380e);
	se_wr(GENI_TX_PACKING_CFG1, 0x000c3e0e);
	se_wr(GENI_RX_PACKING_CFG0, 0x0004380e);
	se_wr(GENI_RX_PACKING_CFG1, 0x000c3e0e);
	se_wr(GENI_BYTE_GRAN, 0);
	/* geni_se_init(): interrupts cleared, clock gating on, the I/O to the engine, FIFO mode */
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	se_wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	se_wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	se_wr(GENI_CGC_CTRL, se_rd(GENI_CGC_CTRL) | 0x7f);
	se_wr(SE_DMA_GENERAL_CFG, se_rd(SE_DMA_GENERAL_CFG) | 0xf);
	se_wr(GENI_OUTPUT_CTRL, 0x7f);
	se_wr(GENI_FORCE_DEFAULT, 1);
	se_wr(GENI_DMA_MODE_EN, se_rd(GENI_DMA_MODE_EN) & ~1u);
	/* the secondary's interrupts to the GIC, the primary's only latched (the writer polls them) */
	se_wr(SE_IRQ_EN, (se_rd(SE_IRQ_EN) | 1u << 3) & ~(1u << 2 | 3u));
	se_wr(GENI_RX_WATERMARK, 2);
	se_wr(GENI_RX_RFR_WATERMARK, fifo_depth_rx - 2);
	se_wr(GENI_TX_WATERMARK, 0);
	se_wr(GENI_M_IRQ_EN, se_rd(GENI_M_IRQ_EN) | M_CMD_DONE | M_CMD_CANCEL | M_CMD_ABORT | 0x7e);
	/* 8N1, CTS obeyed, RFR by the engine */
	se_wr(UART_LOOPBACK_CFG, 0);
	se_wr(UART_TX_TRANS_CFG, se_rd(UART_TX_TRANS_CFG) & ~3u);
	se_wr(UART_RX_TRANS_CFG, se_rd(UART_RX_TRANS_CFG) & ~(1u << 4));
	se_wr(UART_TX_PARITY_CFG, se_rd(UART_TX_PARITY_CFG) & ~1u);
	se_wr(UART_RX_PARITY_CFG, se_rd(UART_RX_PARITY_CFG) & ~1u);
	se_wr(UART_TX_WORD_LEN, 8);
	se_wr(UART_RX_WORD_LEN, 8);
	se_wr(UART_TX_STOP_BIT_LEN, 0);
	se_wr(UART_MANUAL_RFR, 0);
	return NULL;
}

/* ---- for hk_bt.c ---------------------------------------------------------------------------------- */

/*
 * What the firmware left, read only (bt_probe=1): the chip's pins, then -- with QUP 0's clocks voted
 * on, which is a write to GCC only -- the engine's protocol, FIFO and clock levels.
 */
void
instantnoodle_bt_probe(void)
{
	if (!map_all()) {
		say("bt: probe: cannot map the registers\n");
		return;
	}
	say("bt: probe: pins: CTS %#x RTS %#x TX %#x RX %#x BT_EN %#x (level %d) SW_CTRL %#x (level %d)\n",
	    instantnoodle_gpio_cfg(PIN_CTS), instantnoodle_gpio_cfg(PIN_RTS), instantnoodle_gpio_cfg(PIN_TX),
	    instantnoodle_gpio_cfg(PIN_RX), instantnoodle_gpio_cfg(PIN_BT_EN), instantnoodle_gpio_level(PIN_BT_EN),
	    instantnoodle_gpio_cfg(PIN_SW_CTRL), instantnoodle_gpio_level(PIN_SW_CTRL));
	say("bt: probe: GCC vote %#x, cbcr m_ahb %#x s_ahb %#x core %#x core2x %#x se6 %#x; se6 rcg cmd %#x cfg %#x dfsr %#x\n",
	    gcc(GCC_VOTE), gcc(GCC_QUP0_M_AHB_CBCR), gcc(GCC_QUP0_S_AHB_CBCR), gcc(GCC_QUP0_CORE_CBCR),
	    gcc(GCC_QUP0_CORE_2X_CBCR), gcc(GCC_QUP0_SE6_CBCR), gcc(GCC_QUP0_SE6_CMD), gcc(GCC_QUP0_SE6_CFG),
	    gcc(GCC_QUP0_SE6_DFSR));
	for (uint32_t level = 0; level < 8; level++) {
		say("bt: probe: clock level %u: cfg %#x m %#x n %#x = %u Hz\n", level, gcc(GCC_QUP0_SE6_PERF_CFG(level)),
		    gcc(GCC_QUP0_SE6_PERF_M(level)), gcc(GCC_QUP0_SE6_PERF_N(level)), level_rate(level));
	}
	if (!clocks_on() || !rcg_set(0)) {
		return;
	}
	say("bt: probe: QUP 0 version %#x; engine: fw %#x (protocol %u), if_disable %#x, status %#x, hw_param %#x %#x, "
	    "dma_mode %#x, clk_sel %#x m_clk %#x, irq_en %#x m_irq_en %#x s_irq_en %#x, ios %#x, tx_trans %#x\n",
	    rd(qup + QUPV3_HW_VER), se_rd(GENI_FW_REVISION_RO), se_rd(GENI_FW_REVISION_RO) >> 8 & 0xff,
	    se_rd(GENI_IF_DISABLE_RO), se_rd(GENI_STATUS), se_rd(SE_HW_PARAM_0), se_rd(SE_HW_PARAM_1),
	    se_rd(GENI_DMA_MODE_EN), se_rd(GENI_CLK_SEL), se_rd(GENI_SER_M_CLK_CFG), se_rd(SE_IRQ_EN),
	    se_rd(GENI_M_IRQ_EN), se_rd(GENI_S_IRQ_EN), se_rd(GENI_IOS), se_rd(UART_TX_TRANS_CFG));
}

/*
 * The chip's Bluetooth half on (supplies, then BT_EN; at least 100 ms after the Wi-Fi half's enable and
 * 60 ms for it to come up) or off (BT_EN low, its supplies given back). Thread context.
 */
bool
instantnoodle_bt_power(bool on)
{
	if (!map_all()) {
		return false;
	}
	if (on && !powered) {
		if (!instantnoodle_qca6390_supplies(INSTANTNOODLE_QCA6390_BT, true) || !instantnoodle_supply("ldoa16", 3024, true)) {
			say("bt: the chip's supplies did not come on\n");
			instantnoodle_qca6390_supplies(INSTANTNOODLE_QCA6390_BT, false);
			return false;
		}
		instantnoodle_qca6390_enable_gap();
		instantnoodle_gpio_output(PIN_BT_EN, true, INSTANTNOODLE_PULL_NONE, 2);
		sleep_ms(60);
		powered = true;
		say("bt: the chip's Bluetooth half is on (SW_CTRL %d)\n", instantnoodle_gpio_level(PIN_SW_CTRL));
	} else if (!on && powered) {
		instantnoodle_gpio_output(PIN_BT_EN, false, INSTANTNOODLE_PULL_DOWN, 2);
		instantnoodle_supply("ldoa16", 3024, false);
		instantnoodle_qca6390_supplies(INSTANTNOODLE_QCA6390_BT, false);
		powered = false;
		say("bt: the chip's Bluetooth half is off\n");
	}
	return true;
}

/* The UART at 115200 8N1 with RTS/CTS, reading. false: the engine cannot be used (said). */
bool
instantnoodle_bt_uart_open(void)
{
	const char *why;

	if (open) {
		return true;
	}
	if (!map_all() || !clocks_on() || !rcg_set(0)) {
		return false;
	}
	why = engine_setup();
	if (why != NULL) {
		say("bt: %s (fw %#x, if_disable %#x)\n", why, se_rd(GENI_FW_REVISION_RO), se_rd(GENI_IF_DISABLE_RO));
		clocks_off();
		return false;
	}
	if (!set_rate(115200)) {
		clocks_off();
		return false;
	}
	instantnoodle_gpio_function(PIN_CTS, FUNCTION_QUP6, INSTANTNOODLE_PULL_NONE, 2);
	instantnoodle_gpio_function(PIN_RTS, FUNCTION_QUP6, INSTANTNOODLE_PULL_DOWN, 2);
	instantnoodle_gpio_function(PIN_TX, FUNCTION_QUP6, INSTANTNOODLE_PULL_UP, 2);
	instantnoodle_gpio_function(PIN_RX, FUNCTION_QUP6, INSTANTNOODLE_PULL_NONE, 2);
	ring_head = ring_tail = 0;
	if (!irq_attached) {
		kern_return_t kr = gic_v3_spi_attach(SE_INTID, false, se_interrupt, NULL);

		if (kr != KERN_SUCCESS) {
			say("bt: the engine's interrupt (%u) cannot be had: %d; read by polling\n", SE_INTID, kr);
		} else {
			irq_attached = true;
		}
	}
	rx_start();
	open = true;
	say("bt: UART: FIFO %u/%u words, sampling x%u, 115200 baud\n", fifo_depth_tx, fifo_depth_rx, sampling);
	return true;
}

/* The host side's rate (the chip's own is changed by a command of the chip's, hk_bt.c). Nothing is in flight. */
bool
instantnoodle_bt_uart_speed(uint32_t baud)
{
	bool done;

	if (!open) {
		return false;
	}
	if (baud == current_baud) {
		return true;
	}
	lck_mtx_lock(&bt_tx_lock);
	rx_stop();
	done = set_rate(baud);
	rx_start();
	lck_mtx_unlock(&bt_tx_lock);
	return done;
}

/* `n' bytes out, CTS obeyed; 0 or ETIMEDOUT (the chip held CTS for longer than a second) or ENXIO. Thread context. */
int
instantnoodle_bt_uart_write(const uint8_t *bytes, uint32_t n)
{
	uint64_t deadline;
	uint32_t i = 0;
	int error = 0;

	if (!open) {
		return ENXIO;
	}
	if (n == 0) {
		return 0;
	}
	lck_mtx_lock(&bt_tx_lock);
	clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
	se_wr(GENI_M_IRQ_CLEAR, M_CMD_DONE | M_CMD_CANCEL | M_CMD_ABORT);
	se_wr(UART_TX_TRANS_LEN, n);
	se_wr(GENI_M_CMD0, 1u << 27);
	while (i < n) {
		uint32_t room = fifo_depth_tx - (se_rd(GENI_TX_FIFO_STATUS) & 0x0fffffff);

		if (room == 0) {
			if (mach_absolute_time() > deadline) {
				error = ETIMEDOUT;
				break;
			}
			sleep_us(n - i > 64 ? 50 : 10);
			continue;
		}
		while (room-- > 0 && i < n) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && i < n; j++) {
				word |= (uint32_t)bytes[i++] << (8 * j);
			}
			se_wr(GENI_TX_FIFO, word);
		}
	}
	while (error == 0 && !(se_rd(GENI_M_IRQ_STATUS) & M_CMD_DONE)) {
		if (mach_absolute_time() > deadline) {
			error = ETIMEDOUT;
			break;
		}
		sleep_us(10);
	}
	if (error != 0) {
		lck_mtx_unlock(&bt_tx_lock);
		instantnoodle_bt_uart_state("a write timed out");
		lck_mtx_lock(&bt_tx_lock);
		tx_recover();
	}
	se_wr(GENI_M_IRQ_CLEAR, M_CMD_DONE);
	lck_mtx_unlock(&bt_tx_lock);
	return error;
}

/*
 * Up to `max' bytes that came in, waiting at most `timeout_ms' for the first (0: not at all). The
 * number of bytes; 0 when none came. One reader. Thread context.
 */
uint32_t
instantnoodle_bt_uart_read(uint8_t *buffer, uint32_t max, uint32_t timeout_ms)
{
	uint32_t n = 0, tail = ring_tail;

	if (!open) {
		return 0;
	}
	if (__atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) == tail && timeout_ms != 0) {
		/* the interrupt says when; without one (or one lost), look every 5 ms */
		uint32_t step = irq_attached ? timeout_ms : 5;

		for (uint32_t waited = 0; waited < timeout_ms && __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) == tail;
		    waited += step) {
			assert_wait_timeout((event_t)&ring_head, THREAD_UNINT, step, NSEC_PER_MSEC);
			if (__atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) != tail) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
			if (!irq_attached) {
				lck_mtx_lock(&bt_tx_lock);
				drain();
				lck_mtx_unlock(&bt_tx_lock);
			}
		}
	}
	while (n < max && tail != __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE)) {
		buffer[n++] = ring[tail % RING_SIZE];
		tail++;
	}
	__atomic_store_n(&ring_tail, tail, __ATOMIC_RELEASE);
	return n;
}

/* What came in and was not read is dropped. */
void
instantnoodle_bt_uart_flush(void)
{
	__atomic_store_n(&ring_tail, __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

void
instantnoodle_bt_uart_close(void)
{
	if (!open) {
		return;
	}
	lck_mtx_lock(&bt_tx_lock);
	rx_stop();
	if (se_rd(GENI_STATUS) & 1) {
		tx_recover();
	}
	/* the pins as the tree's "default" state: plain inputs and outputs, no engine */
	instantnoodle_gpio_input(PIN_CTS, INSTANTNOODLE_PULL_NONE);
	instantnoodle_gpio_input(PIN_RTS, INSTANTNOODLE_PULL_DOWN);
	instantnoodle_gpio_input(PIN_TX, INSTANTNOODLE_PULL_UP);
	instantnoodle_gpio_input(PIN_RX, INSTANTNOODLE_PULL_DOWN);
	clocks_off();
	open = false;
	current_baud = 0;
	lck_mtx_unlock(&bt_tx_lock);
}

/* The engine's state as it is, for a stall: said in one line. */
void
instantnoodle_bt_uart_state(const char *why)
{
	if (!open) {
		say("bt: UART (%s): closed\n", why);
		return;
	}
	say("bt: UART (%s): status %#x s_irq %#x/%#x m_irq %#x/%#x rx_fifo %#x tx_fifo %#x ios %#x (CTS %s), ring %u..%u, "
	    "irqs %u overruns %u drops %u errors %u\n", why, se_rd(GENI_STATUS), se_rd(GENI_S_IRQ_STATUS), se_rd(GENI_S_IRQ_EN),
	    se_rd(GENI_M_IRQ_STATUS), se_rd(GENI_M_IRQ_EN), se_rd(GENI_RX_FIFO_STATUS), se_rd(GENI_TX_FIFO_STATUS), se_rd(GENI_IOS),
	    (se_rd(GENI_IOS) & 2) ? "high" : "low", ring_tail, ring_head, rx_irqs, rx_overruns, rx_dropped, rx_errors);
}

/* counters for kern.hackios.bt */
void
instantnoodle_bt_uart_counters(uint32_t *irqs, uint32_t *overruns, uint32_t *dropped, uint32_t *errors)
{
	*irqs = rx_irqs;
	*overruns = rx_overruns;
	*dropped = rx_dropped;
	*errors = rx_errors;
}
