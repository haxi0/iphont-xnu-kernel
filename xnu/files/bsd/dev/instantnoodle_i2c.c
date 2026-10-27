/*
 * The OnePlus 8 (instantnoodle, SM8250): its TLMM pins and I2C on two of them, by hand -- or, with boot-arg
 * i2c_geni=2, by the bus's own serial engine (below, "the bus's own serial engine")
 * (sys/instantnoodle.h). Taken out of instantnoodle_touch.c, which did it first (as
 * ios-instantnoodle and ios-guacamole did), so that the fuel gauge, the vibration motor's chip and
 * the amplifiers can be reached the same way.
 *
 * TLMM pin registers (Linux pinctrl-msm.c): CFG [1:0] pull, [5:2] function, [8:6] drive
 * strength (2 mA steps), [9] output enable; IN_OUT bit 0 the level in, bit 1 the level out.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/instantnoodle.h>
#include <sys/systm.h>

/*
 * Every line to the loader's log as well, as hk_buttons.c does: a kernel printf reaches only the kernel's message
 * buffer, which no longer comes back with a run's log.
 */
extern void loader_log_putc(char c);
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);

static LCK_GRP_DECLARE(i2c_lck_grp, "instantnoodle-i2c");
static LCK_MTX_DECLARE(map_lock, &i2c_lck_grp);           /* a transfer may map a tile */

extern void hk_tl(const char *format, ...) __printflike(1, 2);

static uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / NSEC_PER_USEC;
}

/* A clock-stretching fuel gauge must not hold up the touch screen on a different bus. */
static void
bus_lock(struct instantnoodle_i2c *b)
{
	if (__atomic_load_n(&b->lock, __ATOMIC_ACQUIRE) == NULL) {
		lck_mtx_lock(&map_lock);
		if (b->lock == NULL) {
			uint32_t trace = 0;
			lck_mtx_t *lock = lck_mtx_alloc_init(&i2c_lck_grp, LCK_ATTR_NULL);

			if (lock == NULL) {
				panic("instantnoodle: no lock for I2C bus %s", b->name);
			}
			PE_parse_boot_argn("touch_lat", &trace, sizeof(trace));
			b->trace = trace != 0;
			__atomic_store_n(&b->lock, lock, __ATOMIC_RELEASE);
		}
		lck_mtx_unlock(&map_lock);
	}
	lck_mtx_lock(b->lock);
}

bool
instantnoodle_board(void)
{
	char board[32] = "";

	return PE_parse_boot_argn("board", board, sizeof(board)) && strcmp(board, "instantnoodle") == 0;
}

/* ---- pins ------------------------------------------------------------------------------------- */

#define TLMM_TILE_SIZE  0x100000UL      /* 0x1000 per pin, 180 pins */

static vm_offset_t
pin_base(struct instantnoodle_pin p)
{
	static const uint64_t tiles[] = { INSTANTNOODLE_TLMM_WEST, INSTANTNOODLE_TLMM_SOUTH, INSTANTNOODLE_TLMM_NORTH };
	static vm_offset_t mapped[3];

	for (unsigned t = 0; t < 3; t++) {
		if (tiles[t] != p.tile) {
			continue;
		}
		if (__atomic_load_n(&mapped[t], __ATOMIC_ACQUIRE) == 0) {
			lck_mtx_lock(&map_lock);
			if (mapped[t] == 0) {
				__atomic_store_n(&mapped[t], ml_io_map(tiles[t], TLMM_TILE_SIZE), __ATOMIC_RELEASE);
			}
			lck_mtx_unlock(&map_lock);
		}
		return mapped[t] + 0x1000UL * p.pin;
	}
	panic("instantnoodle: pin %u in no TLMM tile (%#llx)", p.pin, p.tile);
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

uint32_t
instantnoodle_gpio_cfg(struct instantnoodle_pin p)
{
	return rd(pin_base(p));
}

static void
configure(struct instantnoodle_pin p, unsigned pull, unsigned ma, bool output)
{
	wr(pin_base(p), pull | 0u << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

void
instantnoodle_gpio_input(struct instantnoodle_pin p, unsigned pull)
{
	configure(p, pull, 2, false);
}

void
instantnoodle_gpio_output(struct instantnoodle_pin p, bool high, unsigned pull, unsigned ma)
{
	wr(pin_base(p) + 4, high ? 2 : 0);      /* the level first, so that the pin never drives the wrong one */
	configure(p, pull, ma, true);
}

void
instantnoodle_gpio_function(struct instantnoodle_pin p, unsigned function, unsigned pull, unsigned ma)
{
	wr(pin_base(p), pull | (function & 0xf) << 2 | (ma / 2 - 1) << 6);
}

bool
instantnoodle_gpio_level(struct instantnoodle_pin p)
{
	return rd(pin_base(p) + 4) & 1;
}

/* ---- I2C on two pins -------------------------------------------------------------------------- */

static void
line(struct instantnoodle_i2c *b, struct instantnoodle_pin p, bool high)
{
	if (high) {
		configure(p, b->pull, 2, false);
	} else {
		wr(pin_base(p) + 4, 0);
		configure(p, b->pull, 2, true);
	}
}

static void
bb_delay(void)
{
	delay(4);
}

/*
 * SCL let go, and held there until it is high: a target may hold it low while it works (clock
 * stretching; TI's fuel gauges do, for up to milliseconds). 0, or ETIMEDOUT after 25 ms.
 */
static int
scl_high(struct instantnoodle_i2c *b)
{
	line(b, b->scl, true);
	for (unsigned us = 0; !instantnoodle_gpio_level(b->scl); us++) {
		if (us == 25000) {
			return ETIMEDOUT;
		}
		delay(1);
	}
	return 0;
}

static void
bb_start(struct instantnoodle_i2c *b)
{
	line(b, b->sda, true);
	scl_high(b);
	bb_delay();
	line(b, b->sda, false);
	bb_delay();
	line(b, b->scl, false);
	bb_delay();
}

static void
bb_stop(struct instantnoodle_i2c *b)
{
	line(b, b->sda, false);
	bb_delay();
	scl_high(b);
	bb_delay();
	line(b, b->sda, true);
	bb_delay();
}

/* the byte, then 0 if the target pulled SDA low for it */
static int
bb_tx(struct instantnoodle_i2c *b, uint8_t byte)
{
	int nack;

	for (int i = 7; i >= 0; i--) {
		line(b, b->sda, (byte >> i) & 1);
		bb_delay();
		scl_high(b);
		bb_delay();
		line(b, b->scl, false);
		bb_delay();
	}
	line(b, b->sda, true);
	bb_delay();
	if (scl_high(b) != 0) {
		line(b, b->scl, false);
		return ETIMEDOUT;
	}
	bb_delay();
	nack = instantnoodle_gpio_level(b->sda);
	line(b, b->scl, false);
	bb_delay();
	return nack ? ENXIO : 0;
}

static uint8_t
bb_rx(struct instantnoodle_i2c *b, bool ack)
{
	uint8_t v = 0;

	line(b, b->sda, true);
	for (int i = 7; i >= 0; i--) {
		bb_delay();
		scl_high(b);
		bb_delay();
		v = (uint8_t)(v << 1 | instantnoodle_gpio_level(b->sda));
		line(b, b->scl, false);
	}
	line(b, b->sda, !ack);
	bb_delay();
	scl_high(b);
	bb_delay();
	line(b, b->scl, false);
	line(b, b->sda, true);
	bb_delay();
	return v;
}


/* ---- the bus's own serial engine (GENI) in FIFO mode -------------------------------------------- */

/*
 * Each of these buses has a serial engine of the SoC's QUPs (Qualcomm's "GENI") that does I2C itself: Android drives
 * them with drivers/i2c/busses/i2c-qcom-geni.c, in FIFO mode wherever the engine's FIFO interface is there (bit 0 of
 * GENI_IF_DISABLE_RO clear), else only through GPI DMA (its "GSI_ONLY"). The I2C above (by hand, on the pins) was
 * taken from ios-instantnoodle, whose OnePlus 7 Pro (SM8150) has that bit set on its touch engine; on this phone the
 * Bluetooth engine (QUP 0 SE 6) reads 0. So: boot-arg i2c_geni=
 *   0 (default)  the pins by hand, as before
 *   1            and once per bus, read only (with its clocks voted on, a write to GCC only): the engine's protocol,
 *                its FIFO interface, its FIFO depth and clock levels, to the log
 *   2            the engine itself, in FIFO mode, where it is there and loaded with I2C (protocol 3); by hand where not
 * What follows is Linux's for this engine, read as data (GPL): i2c-qcom-geni.c of OnePlus's own kernel
 * (android_kernel_oneplus_sm8250, its clock table) and of mainline (geni_i2c_init(), geni_i2c_fifo_xfer(),
 * geni_i2c_irq()), qcom-geni-se.c (geni_se_init(), geni_se_config_packing(), geni_se_select_fifo_mode()), geni-se.h
 * (the registers), gcc-sm8250.c (the clocks), pinctrl-sm8250.c (the pins' function); the buses' addresses, interrupts
 * and rates are the phone's device tree's. A transfer that the engine does not finish is cancelled, then aborted, as
 * there; a bus whose engine cannot be had stays by hand.
 */
#define GCC_PHYS                0x00100000UL
#define CBCR_OFF                (1u << 31)
#define CBCR_HW_CTL             (1u << 1)
#define XO_HZ                   19200000u
#define PROTOCOL_I2C            3

#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define GENI_STATUS             0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_IF_DISABLE_RO      0x064
#define GENI_FW_REVISION_RO     0x068
#define GENI_CLK_SEL            0x07c
#define GENI_BYTE_GRAN          0x254
#define GENI_DMA_MODE_EN        0x258
#define GENI_TX_PACKING_CFG0    0x260
#define GENI_TX_PACKING_CFG1    0x264
#define I2C_TX_TRANS_LEN        0x26c
#define I2C_RX_TRANS_LEN        0x270
#define I2C_SCL_COUNTERS        0x278
#define GENI_RX_PACKING_CFG0    0x284
#define GENI_RX_PACKING_CFG1    0x288
#define GENI_M_CMD0             0x600
#define GENI_M_CMD_CTRL         0x604
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_S_IRQ_EN           0x644
#define GENI_S_IRQ_CLEAR        0x648
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_RX_FIFO_STATUS     0x804
#define GENI_TX_WATERMARK       0x80c
#define GENI_RX_WATERMARK       0x810
#define GENI_RX_RFR_WATERMARK   0x814
#define GENI_M_GP_LENGTH        0x910
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_DMA_GENERAL_CFG      0xe30

#define M_CMD_DONE              (1u << 0)
#define M_CMD_OVERRUN           (1u << 1)
#define M_ILLEGAL_CMD           (1u << 2)
#define M_CMD_FAILURE           (1u << 3)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_GP_IRQ_0              (1u << 9)
#define M_GP_IRQ_1              (1u << 10)      /* a NACK */
#define M_GP_IRQ_3              (1u << 12)      /* bus protocol */
#define M_GP_IRQ_4              (1u << 13)      /* arbitration lost */
#define M_RX_FIFO_WATERMARK     (1u << 26)
#define M_RX_FIFO_LAST          (1u << 27)
#define M_TX_FIFO_WATERMARK     (1u << 30)
/* M_COMMON_GENI_M_IRQ_EN and S_COMMON_GENI_S_IRQ_EN (geni-se.h) */
#define M_COMMON_IRQS           (0x7eu | 1u << 22 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 28 | 1u << 29)
#define S_COMMON_IRQS           (0x3eu | 0x3e00u | 1u << 24 | 1u << 25)
#define M_ERRORS                (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_GP_IRQ_1 | M_GP_IRQ_3 | M_GP_IRQ_4)

#define OP_I2C_WRITE            1
#define OP_I2C_READ             2
#define STOP_STRETCH            (1u << 2)       /* no stop after it: the next message begins with a repeated start */
#define SLV_ADDR_SHIFT          9

struct geni_bus {
	unsigned sda;                   /* the bus: its data pin */
	const char *what;
	uint64_t se_phys;
	uint32_t vote_reg, votes;       /* GCC's branch votes: the wrapper's AHB and core clocks, the engine's */
	uint32_t cbcr[5];               /* their status: m_ahb, s_ahb, core, core_2x, the engine's */
	uint32_t rcg;                   /* the engine's clock generator (its cmd_rcgr) */
	uint32_t intid;
	uint32_t khz;                   /* qcom,clk-freq-out of the device tree, else Android's default 400 */
	/* from here on: the state */
	int state;                      /* 0 not looked at, 1 the engine in use, -1 by hand */
	vm_offset_t se;
	uint32_t tx_wm;
	bool irq;
	volatile uint32_t m_status;     /* what the interrupt has seen of the transfer */
	uint8_t *rx;
	const uint8_t *tx;
	unsigned len, at;
	bool active, reading;
};

/* (pinctrl-sm8250.c: function 1 of each pair is its QUP's; gcc-sm8250.c; the device tree's interrupts and rates) */
static struct geni_bus geni_buses[] = {
	{ 36, "touch (QUP 1 SE 5)", 0x00a94000, 0x52008, 1u << 20 | 1u << 21 | 1u << 19 | 1u << 18 | 1u << 27,
	  { 0x18004, 0x18008, 0x23138, 0x23140, 0x185fc }, 0x18600, 358 + 32, 400 },
	{ 48, "fuel gauge (QUP 2 SE 2)", 0x00888000, 0x52010, 1u << 2 | 1u << 1 | 1u << 0 | 1u << 3 | 1u << 6,
	  { 0x1e004, 0x1e008, 0x23270, 0x23278, 0x1e26c }, 0x1e270, 584 + 32, 100 },
	{ 44, "amplifiers (QUP 2 SE 1)", 0x00884000, 0x52010, 1u << 2 | 1u << 1 | 1u << 0 | 1u << 3 | 1u << 5,
	  { 0x1e004, 0x1e008, 0x23270, 0x23278, 0x1e13c }, 0x1e140, 583 + 32, 400 },
	{ 4, "alert slider (QUP 0 SE 1)", 0x00984000, 0x52008, 1u << 6 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 11,
	  { 0x17004, 0x17008, 0x23000, 0x23008, 0x1713c }, 0x17140, 602 + 32, 400 },
};

/* OnePlus's geni_i2c_clk_map (source 19.2 MHz): the bus's rate, the engine's divider, SCL high, low and cycle counts */
static const struct {
	uint32_t khz, div, t_high, t_low, t_cycle;
} geni_clk_map[] = { { 100, 7, 10, 11, 26 }, { 400, 2, 5, 12, 24 }, { 1000, 1, 3, 9, 18 } };

static uint32_t geni_mode;

/* GCC, a page at a time as it is needed (the IO submap is small: hardware-qualcomm.md) */
static vm_offset_t
gcc_reg(uint32_t offset)
{
	static struct {
		uint32_t page;
		vm_offset_t va;
	} pages[8];

	for (unsigned i = 0; i < 8; i++) {
		if (pages[i].va != 0 && pages[i].page == (offset & ~0xfffu)) {
			return pages[i].va + (offset & 0xfff);
		}
		if (pages[i].va == 0) {
			pages[i].page = offset & ~0xfffu;
			pages[i].va = ml_io_map(GCC_PHYS + pages[i].page, PAGE_SIZE);
			return pages[i].va + (offset & 0xfff);
		}
	}
	panic("instantnoodle: GCC pages");
}

static inline uint32_t
se_rd(struct geni_bus *g, uint32_t offset)
{
	return rd(g->se + offset);
}

static inline void
se_wr(struct geni_bus *g, uint32_t offset, uint32_t value)
{
	wr(g->se + offset, value);
}

/* The wrapper's and the engine's clocks voted on (GCC), and on: before any register of the engine is read. */
static bool
geni_clocks(struct geni_bus *g)
{
	wr(gcc_reg(g->vote_reg), rd(gcc_reg(g->vote_reg)) | g->votes);
	for (int t = 0; t < 1000; t++) {
		bool on = true;

		for (int c = 0; c < 5; c++) {
			uint32_t v = rd(gcc_reg(g->cbcr[c]));

			on &= !(v & CBCR_OFF) || (v & CBCR_HW_CTL);
		}
		if (on) {
			return true;
		}
		delay(10);
	}
	return false;
}

/* rcg2 (clk-rcg2.c): a level's or the generator's rate from its CFG, M and N */
static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
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

/*
 * The engine's clock at 19.2 MHz (geni_i2c_clk_map_idx(): Android's table is for that source). With the generator in
 * its preset mode (DFS, rcg + 0x14 bit 0) the engine picks the level by GENI_CLK_SEL; else the generator is set to the
 * crystal (cfg: source 0, divider 1) and level 0 is it. -1: no level gives 19.2 MHz.
 */
static int
geni_clock_level(struct geni_bus *g, bool set)
{
	if (rd(gcc_reg(g->rcg + 0x14)) & 1) {
		for (int l = 0; l < 8; l++) {
			if (rcg_rate(rd(gcc_reg(g->rcg + 0x1c + 4 * l)), rd(gcc_reg(g->rcg + 0x5c + 4 * l)),
			    rd(gcc_reg(g->rcg + 0x9c + 4 * l))) == XO_HZ) {
				return l;
			}
		}
		return -1;
	}
	if (set) {
		wr(gcc_reg(g->rcg + 4), (rd(gcc_reg(g->rcg + 4)) & ~(0x1fu | 7u << 8 | 3u << 12 | 1u << 20)) | 1u);
		wr(gcc_reg(g->rcg), rd(gcc_reg(g->rcg)) | 1);
		for (int t = 0; t < 500 && (rd(gcc_reg(g->rcg)) & 1); t++) {
			delay(2);
		}
		if (rd(gcc_reg(g->rcg)) & 1) {
			return -1;
		}
	}
	return 0;
}

/* geni_i2c_irq(), FIFO mode: the FIFO filled or drained as the engine asks; the waiter woken at the command's end */
static void
geni_interrupt(void *context, uint32_t intid __unused)
{
	struct geni_bus *g = context;
	uint32_t m = se_rd(g, GENI_M_IRQ_STATUS), rx = se_rd(g, GENI_RX_FIFO_STATUS);

	if (!g->active || (m & (M_CMD_FAILURE | M_CMD_ABORT))) {
		se_wr(g, GENI_TX_WATERMARK, 0);
	} else if (g->reading && (m & (M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST))) {
		for (uint32_t j = 0; j < (rx & 0x01ffffff); j++) {
			uint32_t word = se_rd(g, GENI_RX_FIFO);

			for (int p = 0; p < 4 && g->at < g->len; p++, word >>= 8) {
				g->rx[g->at++] = (uint8_t)word;
			}
			if (g->at == g->len) {
				break;
			}
		}
	} else if (!g->reading && (m & M_TX_FIFO_WATERMARK)) {
		for (uint32_t j = 0; j < g->tx_wm; j++) {
			uint32_t word = 0;

			for (int p = 0; p < 4 && g->at < g->len; p++) {
				word |= (uint32_t)g->tx[g->at++] << (8 * p);
			}
			se_wr(g, GENI_TX_FIFO, word);
			if (g->at == g->len) {
				se_wr(g, GENI_TX_WATERMARK, 0);
				break;
			}
		}
	}
	if (m) {
		se_wr(g, GENI_M_IRQ_CLEAR, m);
	}
	__atomic_fetch_or(&g->m_status, m, __ATOMIC_RELEASE);
	if (m & (M_CMD_DONE | M_CMD_CANCEL | M_CMD_ABORT)) {
		thread_wakeup((event_t)&g->m_status);
	}
}

/* until one of `bits' has been seen, at most `ms' (without the interrupt: the handler's work done by looking) */
static bool
geni_wait(struct geni_bus *g, uint32_t bits, uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (!(__atomic_load_n(&g->m_status, __ATOMIC_ACQUIRE) & bits)) {
		if (mach_absolute_time() >= deadline) {
			return false;
		}
		if (g->irq) {
			assert_wait_deadline((event_t)&g->m_status, THREAD_UNINT, deadline);
			if (__atomic_load_n(&g->m_status, __ATOMIC_ACQUIRE) & bits) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
		} else {
			geni_interrupt(g, g->intid);
			delay(20);
		}
	}
	return true;
}

/* geni_i2c_cancel_xfer(), then geni_i2c_abort_xfer() if the cancel is not taken */
static void
geni_cancel(struct geni_bus *g)
{
	g->active = false;
	se_wr(g, GENI_M_CMD_CTRL, 1u << 2);
	if (!geni_wait(g, M_CMD_CANCEL, 1000)) {
		se_wr(g, GENI_M_CMD_CTRL, 1u << 1);
		(void)geni_wait(g, M_CMD_ABORT, 1000);
	}
	se_wr(g, GENI_TX_WATERMARK, 0);
}

/*
 * One message (geni_i2c_rx_one_msg()/geni_i2c_tx_one_msg(), FIFO mode). 0, ENXIO for a NACK (the address's when the
 * engine sent no byte), EIO for another bus error, ETIMEDOUT. `stretch': no stop after it (a repeated start next).
 */
static int
geni_message(struct geni_bus *g, uint8_t addr, bool reading, uint8_t *rx, const uint8_t *tx, unsigned len, bool stretch)
{
	/* i2c_update_timeout(): ten times the bits' time, at least 300 ms */
	uint32_t ms = MAX(300u, (uint32_t)((uint64_t)10 * 9 * (len + 1) * 1000 / (g->khz * 1000) + 1));
	uint32_t status;

	g->rx = rx;
	g->tx = tx;
	g->len = len;
	g->at = 0;
	g->reading = reading;
	__atomic_store_n(&g->m_status, 0, __ATOMIC_RELEASE);
	g->active = true;
	se_wr(g, reading ? I2C_RX_TRANS_LEN : I2C_TX_TRANS_LEN, len);
	se_wr(g, GENI_M_CMD0, (reading ? OP_I2C_READ : OP_I2C_WRITE) << 27 |
	    ((stretch ? STOP_STRETCH : 0) | (uint32_t)addr << SLV_ADDR_SHIFT));
	if (!reading) {
		se_wr(g, GENI_TX_WATERMARK, 1);         /* (the FIFO's interrupt: filled from the handler) */
	}
	if (!geni_wait(g, M_CMD_DONE, ms)) {
		geni_cancel(g);
		return ETIMEDOUT;
	}
	status = __atomic_load_n(&g->m_status, __ATOMIC_ACQUIRE);
	g->active = false;
	if (status & M_ERRORS) {
		if ((status & M_ERRORS) == M_GP_IRQ_1) {
			return ENXIO;                   /* a NACK: the engine has stopped by itself (Linux: no cancel) */
		}
		geni_cancel(g);
		return EIO;
	}
	return (reading && g->at != len) ? EIO : 0;
}

/*
 * The engine looked at (i2c_geni=1: only that) and, with i2c_geni=2, set up for I2C in FIFO mode: geni_i2c_init(),
 * geni_se_init(), geni_se_config_packing(8 bits, 4 a word, MSB first), geni_se_select_fifo_mode(), qcom_geni_i2c_conf().
 * Under the bus's lock, at its first transfer.
 */
static void
geni_take(struct instantnoodle_i2c *b, struct geni_bus *g)
{
	uint32_t proto, ifd, depth;
	int level;
	unsigned rate;

	g->state = -1;
	if (!geni_clocks(g)) {
		printf("i2c: %s: its clocks stay off: by hand\n", g->what);
		return;
	}
	g->se = ml_io_map(g->se_phys, 0x4000);
	proto = se_rd(g, GENI_FW_REVISION_RO) >> 8 & 0xff;
	ifd = se_rd(g, GENI_IF_DISABLE_RO);
	depth = se_rd(g, SE_HW_PARAM_0) >> 16 & 0x3f;
	level = geni_clock_level(g, false);
	printf("i2c: %s: engine protocol %u (3: I2C), FIFO interface %s (if_disable %#x), TX FIFO %u words, "
	    "clock %s level %d, DMA mode %#x, status %#x\n", g->what, proto, (ifd & 1) ? "OFF (GPI DMA only)" : "there", ifd, depth,
	    (rd(gcc_reg(g->rcg + 0x14)) & 1) ? "preset (DFS)" : "direct", level, se_rd(g, GENI_DMA_MODE_EN), se_rd(g, GENI_STATUS));
	if (geni_mode < 2 || proto != PROTOCOL_I2C || (ifd & 1) || depth < 2) {
		return;
	}
	for (rate = 0; rate < sizeof(geni_clk_map) / sizeof(geni_clk_map[0]) && geni_clk_map[rate].khz != g->khz; rate++) {
	}
	level = geni_clock_level(g, true);
	if (rate == sizeof(geni_clk_map) / sizeof(geni_clk_map[0]) || level < 0) {
		printf("i2c: %s: no 19.2 MHz clock for %u kHz: by hand\n", g->what, g->khz);
		return;
	}
	g->tx_wm = depth - 1;
	/* geni_se_init(): geni_se_irq_clear(), geni_se_io_init(), geni_se_io_set_mode(), the watermarks, the common IRQs */
	se_wr(g, SE_GSI_EVENT_EN, 0);
	se_wr(g, GENI_M_IRQ_CLEAR, 0xffffffff);
	se_wr(g, GENI_S_IRQ_CLEAR, 0xffffffff);
	se_wr(g, SE_DMA_TX_IRQ_CLR, 0xffffffff);
	se_wr(g, SE_DMA_RX_IRQ_CLR, 0xffffffff);
	se_wr(g, SE_IRQ_EN, 0xffffffff);
	se_wr(g, GENI_CGC_CTRL, se_rd(g, GENI_CGC_CTRL) | 0x7f);
	se_wr(g, SE_DMA_GENERAL_CFG, se_rd(g, SE_DMA_GENERAL_CFG) | 0xf);
	se_wr(g, GENI_OUTPUT_CTRL, 0x7f);
	se_wr(g, GENI_FORCE_DEFAULT, 1);
	se_wr(g, SE_IRQ_EN, se_rd(g, SE_IRQ_EN) | 0xf);
	se_wr(g, GENI_DMA_MODE_EN, se_rd(g, GENI_DMA_MODE_EN) & ~1u);
	se_wr(g, SE_GSI_EVENT_EN, 0);
	se_wr(g, GENI_RX_WATERMARK, g->tx_wm);
	se_wr(g, GENI_RX_RFR_WATERMARK, depth);
	se_wr(g, GENI_M_IRQ_EN, se_rd(g, GENI_M_IRQ_EN) | M_COMMON_IRQS);
	se_wr(g, GENI_S_IRQ_EN, se_rd(g, GENI_S_IRQ_EN) | S_COMMON_IRQS);
	/* geni_se_config_packing(se, 8, 4, true, true, true) */
	se_wr(g, GENI_TX_PACKING_CFG0, 0x7f8fe);
	se_wr(g, GENI_TX_PACKING_CFG1, 0xffefe);
	se_wr(g, GENI_RX_PACKING_CFG0, 0x7f8fe);
	se_wr(g, GENI_RX_PACKING_CFG1, 0xffefe);
	se_wr(g, GENI_BYTE_GRAN, 0);
	/* geni_se_select_fifo_mode() */
	se_wr(g, GENI_M_IRQ_CLEAR, 0xffffffff);
	se_wr(g, GENI_M_IRQ_EN, se_rd(g, GENI_M_IRQ_EN) | M_CMD_DONE | M_TX_FIFO_WATERMARK | M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST);
	se_wr(g, GENI_DMA_MODE_EN, se_rd(g, GENI_DMA_MODE_EN) & ~1u);
	/* qcom_geni_i2c_conf() */
	se_wr(g, GENI_CLK_SEL, (uint32_t)level);
	se_wr(g, GENI_SER_M_CLK_CFG, geni_clk_map[rate].div << 4 | 1);
	se_wr(g, I2C_SCL_COUNTERS, geni_clk_map[rate].t_high << 20 | geni_clk_map[rate].t_low << 10 | geni_clk_map[rate].t_cycle);
	/* the pins to the engine (function 1: the QUP's), as Android's "default" state; the bus's pull as by hand */
	instantnoodle_gpio_function(b->sda, 1, b->pull, 2);
	instantnoodle_gpio_function(b->scl, 1, b->pull, 2);
	g->irq = gic_v3_spi_attach(g->intid, false, geni_interrupt, g) == KERN_SUCCESS;
	g->state = 1;
	printf("i2c: %s: the engine does the bus now, %u kHz, FIFO mode, %s\n", g->what, g->khz,
	    g->irq ? "interrupt-driven" : "polled (no interrupt)");
}

/* The bus's engine, if it does the bus (looked at once, at the first transfer); NULL: by hand. Under the bus's lock. */
static struct geni_bus *
geni_of(struct instantnoodle_i2c *b)
{
	for (unsigned i = 0; i < sizeof(geni_buses) / sizeof(geni_buses[0]); i++) {
		struct geni_bus *g = &geni_buses[i];

		if (g->sda != b->sda.pin) {
			continue;
		}
		if (g->state == 0) {
			PE_parse_boot_argn("i2c_geni", &geni_mode, sizeof(geni_mode));
			if (geni_mode == 0) {
				g->state = -1;
				return NULL;
			}
			geni_take(b, g);
		}
		return g->state == 1 ? g : NULL;
	}
	return NULL;
}

/* For what the engine cannot do (a message of no bytes; a stuck bus's nine clocks): the pins by hand for a moment */
static void
pins_by_hand(struct instantnoodle_i2c *b, bool by_hand)
{
	if (by_hand) {
		line(b, b->sda, true);
		line(b, b->scl, true);
	} else {
		instantnoodle_gpio_function(b->sda, 1, b->pull, 2);
		instantnoodle_gpio_function(b->scl, 1, b->pull, 2);
	}
}

void
instantnoodle_i2c_recover(struct instantnoodle_i2c *b)
{
	struct geni_bus *g;

	bus_lock(b);
	g = geni_of(b);
	if (g != NULL) {
		pins_by_hand(b, true);
	}
	line(b, b->sda, true);
	for (int i = 0; i < 9; i++) {
		line(b, b->scl, false);
		bb_delay();
		line(b, b->scl, true);
		bb_delay();
	}
	bb_stop(b);
	if (g != NULL) {
		pins_by_hand(b, false);
	}
	lck_mtx_unlock(b->lock);
}

int
instantnoodle_i2c_write(struct instantnoodle_i2c *b, uint8_t addr, const uint8_t *bytes, unsigned n)
{
	struct geni_bus *g;
	int err;

	bus_lock(b);
	g = geni_of(b);
	if (g != NULL && n != 0) {
		err = geni_message(g, addr, false, NULL, bytes, n, false);
		lck_mtx_unlock(b->lock);
		if (err) {
			b->errors++;
		}
		return err;
	}
	if (g != NULL) {
		pins_by_hand(b, true);          /* (no message of no bytes in the engine: Linux's I2C_AQ_NO_ZERO_LEN) */
	}
	bb_start(b);
	err = bb_tx(b, (uint8_t)(addr << 1));
	for (unsigned i = 0; i < n && err == 0; i++) {
		err = bb_tx(b, bytes[i]);
	}
	bb_stop(b);
	if (g != NULL) {
		pins_by_hand(b, false);
	}
	lck_mtx_unlock(b->lock);
	if (err) {
		b->errors++;
	}
	return err;
}

int
instantnoodle_i2c_read(struct instantnoodle_i2c *b, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n)
{
	struct geni_bus *g;
	int err;
	uint64_t began = mach_absolute_time(), locked, done;

	bus_lock(b);
	locked = mach_absolute_time();
	g = geni_of(b);
	if (g != NULL) {
		/* geni_i2c_fifo_xfer(): the register written with STOP_STRETCH, then read after a repeated start */
		err = geni_message(g, addr, false, NULL, &reg, 1, true);
		if (err == 0 && n != 0) {
			err = geni_message(g, addr, true, buf, NULL, n, false);
		}
	} else {
		bb_start(b);
		err = bb_tx(b, (uint8_t)(addr << 1));
		if (err == 0) {
			err = bb_tx(b, reg);
		}
		if (err == 0) {
			bb_start(b);                    /* repeated start */
			err = bb_tx(b, (uint8_t)(addr << 1 | 1));
		}
		for (unsigned i = 0; i < n && err == 0; i++) {
			buf[i] = bb_rx(b, i + 1 < n);
		}
		bb_stop(b);
	}
	done = mach_absolute_time();
	lck_mtx_unlock(b->lock);
	if (err) {
		b->errors++;
	}
	if (b->trace) {
		hk_tl("tl: b %s begin %llu locked %llu done %llu bytes %u error %d\n", b->name,
		    micros(began), micros(locked), micros(done), n, err);
	}
	return err;
}
