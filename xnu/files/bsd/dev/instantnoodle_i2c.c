/*
 * The OnePlus 8 (instantnoodle, SM8250): its TLMM pins and I2C on two of them, by hand
 * (sys/instantnoodle.h). Taken out of instantnoodle_touch.c, which did it first (as
 * ios-instantnoodle and ios-guacamole did), so that the fuel gauge, the vibration motor's chip and
 * the amplifiers can be reached the same way.
 *
 * TLMM pin registers (Linux pinctrl-msm.c): CFG [1:0] pull, [5:2] function, [8:6] drive
 * strength (2 mA steps), [9] output enable; IN_OUT bit 0 the level in, bit 1 the level out.
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

void
instantnoodle_i2c_recover(struct instantnoodle_i2c *b)
{
	bus_lock(b);
	line(b, b->sda, true);
	for (int i = 0; i < 9; i++) {
		line(b, b->scl, false);
		bb_delay();
		line(b, b->scl, true);
		bb_delay();
	}
	bb_stop(b);
	lck_mtx_unlock(b->lock);
}

int
instantnoodle_i2c_write(struct instantnoodle_i2c *b, uint8_t addr, const uint8_t *bytes, unsigned n)
{
	int err;

	bus_lock(b);
	bb_start(b);
	err = bb_tx(b, (uint8_t)(addr << 1));
	for (unsigned i = 0; i < n && err == 0; i++) {
		err = bb_tx(b, bytes[i]);
	}
	bb_stop(b);
	lck_mtx_unlock(b->lock);
	if (err) {
		b->errors++;
	}
	return err;
}

int
instantnoodle_i2c_read(struct instantnoodle_i2c *b, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n)
{
	int err;
	uint64_t began = mach_absolute_time(), locked, done;

	bus_lock(b);
	locked = mach_absolute_time();
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
