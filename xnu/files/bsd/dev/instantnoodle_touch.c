/*
 * The touch screen of the OnePlus 8 (instantnoodle, SM8250): a Samsung S6SY761 ("SY771") at
 * I2C address 0x48, as a source of touch records for /dev/hk-touch (sys/hk_input.h).
 * On with boot-arg board=instantnoodle (xnuloader passes it); touchscreen=0 turns it off.
 *
 * Its bus is serial engine 13 (i2c@a94000, pins gpio36/gpio37 with external pull-ups); this
 * driver does not use the engine but drives the two pins (instantnoodle_i2c.c; open drain: low = output 0,
 * high = input), as ios-instantnoodle and ios-guacamole (OnePlus 7 Pro, same chip) did: the
 * engine's FIFO path is disabled on these phones and would need GPI DMA. Reset is gpio38, the
 * report line gpio39 (active low, TLMM summary interrupt 208 + 32 at the GIC). Supplies: the
 * chip's 1.8 V I/O from LDO1 of the PM8150A ("ldoc1"), its 3 V analog from LDO13 of the PM8150
 * ("ldoa13"), both through RPMh. All four pins are in the SOUTH tile of the SM8250 TLMM
 * (0x0f500000; pins at 0x1000 * gpio, Linux pinctrl-sm8250.c).
 *
 * Protocol: Linux drivers/input/touchscreen/s6sy761.c and the vendor's sec_ts driver. On this
 * phone the configuration commands (TOUCH_FUNC, SENSE_ON) are not acknowledged, yet the chip
 * reports touches after its boot-complete event; they are sent anyway.
 *
 * Coordinates: the panel's own, 1080 x 2400 (device tree touchpanel,panel-coords), scaled to
 * the record's 0..65535.
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
#include <sys/hk_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
void instantnoodle_touch_init(void);

static vm_offset_t rsc, cmd_db;

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

/* ---- pins (TLMM, SOUTH tile) and the bus (sys/instantnoodle.h) -------------------------------- */

#define PIN(n)                  ((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_SOUTH, (n) })
#define PIN_RESET               PIN(38)
#define PIN_IRQ                 PIN(39)
#define TS_ADDR                 0x48
#define TLMM_IRQ                (208 + 32)
#define IRQ_ENABLE              (1u << 0)
#define IRQ_CONFIG              8
#define IRQ_STATUS              12

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern void hk_tl(const char *format, ...) __printflike(1, 2);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);              /* (osfmk/kern/thread.h, private to osfmk's headers) */

static vm_offset_t irq_pin;
static uint64_t irq_time;
/*
 * The screen off (bsd/dev/hk_power.c): iOS turns the digitizer off with it, and the chip is held in reset for as
 * long -- no scanning, no report line, no look every 100 ms --, then started again as at the beginning (reset let go,
 * 140 ms, its boot-complete event, the touch function and sense on), which takes some 200 ms of the screen's wake.
 */
static volatile bool touch_dark_wanted;
static bool touch_dark;
static uint32_t touch_darks;
/* touch_irq=0: the old 10 ms polling, for recovery (the interrupt path: verified on the phone 2026-10-23) */
static uint32_t trace_latency, use_irq = 1;

static uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / NSEC_PER_USEC;
}

/* The I2C transfer sleeps and takes a mutex; only mask and wake here. */
static void
touch_irq(void *context __unused, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	__atomic_store_n(&irq_time, mach_absolute_time(), __ATOMIC_RELEASE);
	thread_wakeup((event_t)&irq_time);
}

static uint64_t
wait_report(void)
{
	for (;;) {
		uint64_t at, deadline;

		if (touch_dark_wanted != touch_dark) {
			return 0;               /* (the screen went off or came on: the loop does the rest) */
		}
		clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
		assert_wait_deadline((event_t)&irq_time, THREAD_UNINT, touch_dark ? 0 : deadline);
		at = __atomic_exchange_n(&irq_time, 0, __ATOMIC_ACQUIRE);
		if (touch_dark_wanted != touch_dark ||
		    (!touch_dark && (at != 0 || !instantnoodle_gpio_level(PIN_IRQ)))) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			return at;
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* The screen off or on (bsd/dev/hk_power.c): the worker puts the chip into reset, or starts it again. */
void instantnoodle_touch_display(bool on);

void
instantnoodle_touch_display(bool on)
{
	touch_dark_wanted = !on;
	thread_wakeup((event_t)&irq_time);
}

static struct instantnoodle_i2c bus = { .name = "se13", .sda = PIN(36), .scl = PIN(37) };

static int
ts_write(const uint8_t *tx, unsigned n)
{
	return instantnoodle_i2c_write(&bus, TS_ADDR, tx, n);
}

static int
ts_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	return instantnoodle_i2c_read(&bus, TS_ADDR, reg, buf, n);
}

/* ---- the supplies (RPMh), as bsd/dev/hk_goodix.c --------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return false;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("touch: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* (also the alert slider's sensors' supply, instantnoodle_slider.c: one request at a time) */
static LCK_GRP_DECLARE(supply_lck_grp, "instantnoodle-supply");
static LCK_MTX_DECLARE(supply_lock, &supply_lck_grp);

bool
instantnoodle_supply_on(const char *name, uint32_t mv)
{
	uint32_t base = rpmh_address(name);
	bool done;
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_MODE, base + VRM_ENABLE };
	uint32_t data[3] = { mv, VRM_MODE_LDO_HPM, 1 };         /* enable last */

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("touch: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	lck_mtx_lock(&supply_lock);
	done = rpmh_write(address, data, 3);
	lck_mtx_unlock(&supply_lock);
	if (!done) {
		printf("touch: RPMh: %s %u mV: no answer\n", name, mv);
		return false;
	}
	printf("touch: %s at %#x on, %u mV\n", name, base, mv);
	return true;
}

/* ---- the S6SY761 ---------------------------------------------------------------------------------- */

#define CMD_SENSE_ON            0x10
#define CMD_FW_INTEGRITY        0x21
#define CMD_TOUCH_FUNC          0x30
#define CMD_DEVICE_ID           0x52
#define CMD_BOOT_STATUS         0x55
#define CMD_READ_EVENT          0x60
#define CMD_READ_ALL            0x61
#define EVENT_LEN               8
#define EVENT_INFO              0x02
#define EVENT_VENDOR            0x07
#define ACK_BOOT_COMPLETE       0x00
#define MASK_TOUCH              0x01
#define MAX_FINGERS             10
#define EVENT_COUNT             32
#define PANEL_W                 1080
#define PANEL_H                 2400

static struct {
	uint32_t x, y;
	bool down;
} finger[MAX_FINGERS];
static unsigned events;

static int
wait_boot_complete(void)
{
	uint8_t ev[EVENT_LEN];

	for (int i = 0; i < 50; i++) {
		if (ts_read(CMD_READ_EVENT, ev, sizeof(ev)) == 0) {
			unsigned status = ev[0] >> 2 & 0xf;

			if ((status == EVENT_INFO || status == EVENT_VENDOR) && ev[1] == ACK_BOOT_COMPLETE) {
				return 0;
			}
		}
		sleep_ms(20);
	}
	return -1;
}

static uint32_t
scale(uint32_t v, uint32_t size)
{
	if (v >= size) {
		v = size - 1;
	}
	return v * 65535u / (size - 1);
}

static void
handle_event(const uint8_t *ev)
{
	unsigned id, state;
	uint32_t x, y;

	if (ev[0] == 0 || (ev[0] & 3) != 0) {
		return;                                 /* status, information or vendor events */
	}
	id = (ev[0] >> 2 & 0xf);
	if (id == 0 || id > MAX_FINGERS) {
		return;
	}
	id--;
	state = ev[0] >> 6;
	x = (uint32_t)ev[1] << 4 | ev[3] >> 4;
	y = (uint32_t)ev[2] << 4 | (ev[3] & 0xf);
	switch (state) {
	case 1:                                         /* press */
	case 2:                                         /* move */
		finger[id].x = scale(x, PANEL_W);
		finger[id].y = scale(y, PANEL_H);
		hk_touch_post(finger[id].down ? HK_TOUCH_MOVE : HK_TOUCH_DOWN, id, finger[id].x, finger[id].y);
		if (!finger[id].down) {
			printf("touch: finger %u down at %u,%u\n", id, x, y);
		}
		finger[id].down = true;
		events++;
		break;
	case 3:                                         /* release */
		if (finger[id].down) {
			finger[id].down = false;
			hk_touch_post(HK_TOUCH_UP, id, finger[id].x, finger[id].y);
			events++;
		}
		break;
	}
}

static int
poll_events(void)
{
	uint8_t ev[EVENT_LEN], stack[(EVENT_COUNT - 1) * EVENT_LEN];
	unsigned left;
	int err = ts_read(CMD_READ_EVENT, ev, sizeof(ev));

	if (err) {
		return err;
	}
	if (ev[0] == 0) {
		return 0;
	}
	left = ev[7] & 0x3f;
	if (left >= EVENT_COUNT) {
		return EOVERFLOW;
	}
	if (left) {
		err = ts_read(CMD_READ_ALL, stack, left * EVENT_LEN);
		if (err) {
			return err;
		}
	}
	handle_event(ev);
	if (left) {
		for (unsigned i = 0; i < left; i++) {
			handle_event(&stack[i * EVENT_LEN]);
		}
	}
	return 0;
}

static void
release_all(void)
{
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (finger[i].down) {
			finger[i].down = false;
			hk_touch_post(HK_TOUCH_UP, i, finger[i].x, finger[i].y);
		}
	}
}

static void
touch_thread(void *parameter __unused, wait_result_t result __unused)
{
	uint8_t id[3] = { 0, 0, 0 }, boot = 0, fw = 0, func[3] = { CMD_TOUCH_FUNC, MASK_TOUCH, 0 }, sense = CMD_SENSE_ON;
	unsigned errors = 0, unowned = 0;
	uint64_t previous = 0, report_since = 0;

	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);    /* memory, not registers */
	printf("touch: S6SY761: pins sda %u scl %u reset %u irq %u: cfg %#x %#x %#x %#x\n", bus.sda.pin, bus.scl.pin,
	    PIN_RESET.pin, PIN_IRQ.pin, instantnoodle_gpio_cfg(bus.sda), instantnoodle_gpio_cfg(bus.scl),
	    instantnoodle_gpio_cfg(PIN_RESET), instantnoodle_gpio_cfg(PIN_IRQ));

	instantnoodle_i2c_recover(&bus);                /* both lines let go */
	instantnoodle_gpio_input(PIN_IRQ, INSTANTNOODLE_PULL_NONE);
	instantnoodle_gpio_output(PIN_RESET, false, INSTANTNOODLE_PULL_UP, 8);  /* in reset while the supplies come up */
	if (!instantnoodle_supply_on("ldoc1", 1800) || (sleep_ms(1), !instantnoodle_supply_on("ldoa13", 3008))) {
		printf("touch: no supplies: no touch screen\n");
		goto out;
	}
	sleep_ms(20);
	instantnoodle_gpio_output(PIN_RESET, true, INSTANTNOODLE_PULL_UP, 8);
	sleep_ms(140);                                  /* Linux s6sy761: power-on wait */
	instantnoodle_i2c_recover(&bus);

	printf("touch: boot-complete event: %s\n", wait_boot_complete() == 0 ? "seen" : "NOT SEEN (going on)");
	if (ts_read(CMD_DEVICE_ID, id, 3) != 0) {
		printf("touch: no answer at address %#x: no touch screen\n", TS_ADDR);
		goto out;
	}
	ts_read(CMD_BOOT_STATUS, &boot, 1);
	ts_read(CMD_FW_INTEGRITY, &fw, 1);
	printf("touch: device id %02x %02x %02x, boot status %#x (0x20: application), firmware integrity %#x\n", id[0], id[1],
	    id[2], boot, fw);
	/* not acknowledged on this phone, harmless (see the top) */
	ts_write(func, 3);
	ts_write(&sense, 1);
	if (use_irq) {
		irq_pin = ml_io_map(INSTANTNOODLE_TLMM_SOUTH + 0x1000UL * PIN_IRQ.pin, 0x1000);
		/* pinctrl-sm8250: target KPSS=3, raw status enabled, level low. */
		wr(irq_pin + IRQ_CONFIG, (3u << 5) | (1u << 4));
		wr(irq_pin + IRQ_STATUS, 0);         /* TLMM status is write-zero-to-clear */
		if (gic_v3_spi_attach(TLMM_IRQ, false, touch_irq, NULL) != KERN_SUCCESS) {
			printf("touch: cannot attach TLMM interrupt %u; polling every 10 ms instead\n", TLMM_IRQ);
			use_irq = 0;
		} else {
			wr(irq_pin + IRQ_CONFIG, rd(irq_pin + IRQ_CONFIG) | IRQ_ENABLE);
			printf("touch: S6SY761 interrupt-driven, gpio39 low, GIC %u (100 ms watchdog)\n", TLMM_IRQ);
		}
	} else {
		printf("touch: polling every 10 ms (touch_irq=0)\n");
	}
	report_since = mach_absolute_time();

	for (;;) {
		uint64_t detected = use_irq || touch_dark ? wait_report() : mach_absolute_time();
		uint64_t began = mach_absolute_time(), done, ns;
		unsigned before = events;
		int err;

		if (touch_dark_wanted && !touch_dark) {
			release_all();
			if (use_irq) {
				wr(irq_pin + IRQ_CONFIG, rd(irq_pin + IRQ_CONFIG) & ~IRQ_ENABLE);
				gic_v3_spi_mask(TLMM_IRQ);
			}
			instantnoodle_gpio_output(PIN_RESET, false, INSTANTNOODLE_PULL_UP, 8);
			touch_dark = true;
			touch_darks++;
			continue;
		}
		if (!touch_dark_wanted && touch_dark) {
			int booted;

			instantnoodle_gpio_output(PIN_RESET, true, INSTANTNOODLE_PULL_UP, 8);
			sleep_ms(140);
			instantnoodle_i2c_recover(&bus);
			booted = wait_boot_complete();
			ts_write(func, 3);
			ts_write(&sense, 1);
			if (use_irq) {
				wr(irq_pin + IRQ_STATUS, 0);
				wr(irq_pin + IRQ_CONFIG, rd(irq_pin + IRQ_CONFIG) | IRQ_ENABLE);
				gic_v3_spi_unmask(TLMM_IRQ);
			}
			touch_dark = false;
			absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
			printf("touch: the screen is on: the chip started again in %llu ms (%s; %u times off)\n", ns / NSEC_PER_MSEC,
			    booted == 0 ? "boot-complete seen" : "no boot-complete event", touch_darks);
			continue;
		}
		if (touch_dark) {
			continue;
		}

		if (use_irq && detected != 0 && !(rd(irq_pin + IRQ_STATUS) & 1) && instantnoodle_gpio_level(PIN_IRQ)) {
			unowned++;
		} else {
			unowned = 0;
		}
		if (unowned == 16) {
			/* Allow a stale GIC pending bit after draining; stop an unowned interrupt storm. */
			printf("touch: unowned TLMM summary interrupt; leaving it masked, polling every 10 ms instead\n");
			wr(irq_pin + IRQ_CONFIG, rd(irq_pin + IRQ_CONFIG) & ~IRQ_ENABLE);
			gic_v3_spi_detach(TLMM_IRQ);
			use_irq = 0;
		}
		err = poll_events();
		done = mach_absolute_time();
		if (trace_latency && (events != before || err)) {
			hk_tl("tl: t irq %llu worker %llu done %llu events %u error %d\n",
			    use_irq ? micros(detected) : 0, micros(began), micros(done), events - before, err);
			if (!use_irq) {
				hk_tl("tl: c before %llu seen %llu read %llu\n", micros(previous), micros(began), micros(done));
			}
		}
		previous = began;
		if (err != 0) {
			if (++errors == 10) {
				release_all();
				instantnoodle_i2c_recover(&bus);
			}
		} else {
			errors = 0;
		}
		absolutetime_to_nanoseconds(done - report_since, &ns);
		if (ns >= 60 * NSEC_PER_SEC) {
			printf("touch: %u events, %u bus errors so far\n", events, bus.errors);
			report_since = done;
		}
		if (use_irq) {
			/* Drain new reports without a sleep. Back off on bus failure before rearming. */
			if (err) {
				sleep_ms(10);
			}
			wr(irq_pin + IRQ_STATUS, 0);
			gic_v3_spi_unmask(TLMM_IRQ);
		} else {
			sleep_ms(10);
		}
	}
out:
	thread_terminate(current_thread());
}

void
instantnoodle_touch_init(void)
{
	uint32_t enabled = 1;
	thread_t thread;

	if (!instantnoodle_board()) {
		return;
	}
	PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled));
	PE_parse_boot_argn("touch_irq", &use_irq, sizeof(use_irq));
	PE_parse_boot_argn("touch_lat", &trace_latency, sizeof(trace_latency));
	if (!enabled) {
		return;
	}
	/*
	 * Above the kernel's default (81): at 90 Hz the framebuffer's three copying threads hold processors
	 * most of the time, and at their priority a touch waited up to 7 ms to be read (BASEPRI_PREEMPT, 92).
	 */
	if (kernel_thread_start_priority(touch_thread, NULL, 92, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
