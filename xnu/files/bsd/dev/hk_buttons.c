/*
 * The buttons of the POCO X5 Pro (redwood) -- power, volume up, volume down -- as a source of
 * button records for /dev/hk-touch (sys/hk_input.h). Only with boot-arg buttons=1:
 * every address below belongs to this phone.
 *
 * None of the three is a pin of the SoC. They are inputs of two of the power management chips:
 *   power        KPDPWR_N of the power-on block ("PON") of the PMK8350
 *   volume down  RESIN_N of the same block
 *   volume up    GPIO6 of the PM7325, pulled up; the key pulls it low
 * Both chips sit on the SPMI bus (slaves 0 and 1), and the way to that bus is the SoC's "PMIC
 * arbiter": it owns the bus and gives each of the systems on the chip (this one, the modem, the
 * always-on processor, TrustZone...) a set of registers per PMIC peripheral -- a "channel" -- to
 * ask for a transfer with. Reads go through the "observer" channels, which every system has for
 * every peripheral, whoever owns it.
 *
 * THIS DRIVER ONLY READS. Nothing is written to either chip, neither a register of a PMIC nor
 * the configuration of the pin: the PMICs make the phone's supplies, and a wrong write there
 * can switch one off. The firmware reads the same keys itself (volume down and volume up choose
 * between fastboot and recovery) and has left both inputs as they are needed; the driver checks
 * that before it believes them, and where it finds something else it says so and leaves that
 * key alone. The only thing written is the command word that asks the arbiter for a read.
 *
 * What a press is: the PON's "real time status" has a bit per key, set while the key is held
 * (after the block's own debouncing); the GPIO's has the level of the pin. A thread reads the
 * two registers 60 times a second, and a key has changed when two looks in a row say so. A key
 * that reads as held before it has ever been seen released is not believed (a pin that is not
 * what it was taken for must not become a volume key held down for the whole session).
 * No interrupt is used; what one would take is said at the end of this file.
 *
 * The mapping: the phone has what the iPhone X has, whose user space runs here -- a side button
 * and two volume keys, no home button. Power is the side button (HK_BUTTON_LOCK).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg
 * buttons_steps=N stops after the first N:
 *   1  map the arbiter and read its version             (the first access to that block)
 *   2  find the channels of the two peripherals         (reads of the arbiter's tables)
 *   3  ask each peripheral what it is                   (the first transfers on the bus)
 *   4  read how the keys' inputs are set up, and what they say now
 *   5  watch them                                       (the default)
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (arbiter version 5),
 * drivers/input/misc/pm8941-pwrkey.c (the "pmk8350" kind), drivers/pinctrl/qcom/pinctrl-spmi-gpio.c,
 * and the phone's own device tree (qcom,spmi@c440000, pon_hlos@1300, gpio_keys).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hk_input.h>
#include <sys/hk_board.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Every line goes to the loader's log directly as well: without -v the kernel's own messages
 * reach that log only at the end of a run (with the kernel's message buffer), and "xl log"
 * during a run is to show a press when it happens.
 */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
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

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter ---------------------------------------------------------------------------- */

#define ARB_CORE_PHYS           hk_board_addr("spmi_core", 0x0c440000UL)    /* "core": the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           hk_board_addr("spmi_cnfg", 0x0c40a000UL)    /* "cnfg": which system owns each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       hk_board_addr("spmi_obsrvr", 0x0e600000UL)    /* "obsrvr": 0x10000 per system, 0x80 per channel in it */
#define ARB_OBSERVER_SIZE       0x10000UL       /* ours is the first */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what follows is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral, [24] interrupts go to the owner */
#define ARB_CHANNEL_INTERRUPTS  (1u << 24)
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */
#define READ_TIMEOUT_US         2000            /* a read takes some tens of microseconds */

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/*
 * One transfer at a time through the arbiter for this system, whoever asks (the flashlight's
 * driver, bsd/dev/hk_torch.c, as well): with two at once the arbiter drops one ("the transfer
 * was dropped"), as Linux never lets happen (one lock for the bus).
 */
static LCK_GRP_DECLARE(spmi_lck_grp, "hk-spmi");
static LCK_SPIN_DECLARE(spmi_lock, &spmi_lck_grp);

void
hk_spmi_lock(void)
{
	lck_spin_lock(&spmi_lock);
}

void
hk_spmi_unlock(void)
{
	lck_spin_unlock(&spmi_lock);
}

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral: the high byte of its register addresses */
	uint16_t        channel;
	bool            found;
};
static struct peripheral pon = { .name = "the PMK8350's power-on block", .id = 0x013 };          /* registers 0x1300 of slave 0 */
static struct peripheral pin = { .name = "GPIO6 of the PM7325", .id = 0x18d };                   /* registers 0x8d00 of slave 1 */
static struct peripheral pin_down = { .name = "(none)", .id = 0 };     /* volume down as a pin, where it is one */

/*
 * The keys of each phone (boot-arg board=, which xnuloader passes): redwood above, by default.
 * OnePlus 8 (instantnoodle, SM8250): power is KPDPWR of the PM8150's power-on block (registers
 * 0x0800 of slave 0, status bit 0, the older layout: no HLOS half), volume up and volume down
 * are GPIO6 and GPIO7 of the PM8150 (0xc500, 0xc600), pulled up, pulled low by the key (its
 * device tree's gpio_keys; read the same way by ios-instantnoodle's sweet_keys.c).
 */
static struct {
	uint8_t pon_subtype_from, pon_subtype_to;       /* what the power-on block must say it is */
	uint8_t power_bit, volume_down_bit;             /* in its status; volume_down_bit 0: a pin instead */
} keys = { 0x09, 0x09, 0x80, 0x40 };

static void
keys_for_board(void)
{
	char board[32] = "";

	if (!PE_parse_boot_argn("board", board, sizeof(board)) || strcmp(board, "instantnoodle") != 0) {
		return;
	}
	pon.name = "the PM8150's power-on block";
	pon.id = 0x008;
	pin.name = "GPIO6 of the PM8150";
	pin.id = 0x0c5;
	pin_down.name = "GPIO7 of the PM8150";
	pin_down.id = 0x0c6;
	keys.pon_subtype_from = 0x04;          /* what the phone's says: type 1, subtype 4 */
	keys.pon_subtype_to = 0x04;
	keys.power_bit = 0x01;
	keys.volume_down_bit = 0;
}

/* How the reads have gone: said once a minute. */
static struct {
	uint64_t reads, failed, total_ns, longest_ns, last_ns;
} bus;

/*
 * Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what
 * went wrong.
 */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	uint64_t start = mach_absolute_time(), now, interval, taken;
	uint32_t status, data[2];
	const char *problem = NULL;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hk_spmi_lock();
	wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(channel + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	hk_spmi_unlock();

	if (!(status & STATUS_DONE)) {
		problem = "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		problem = "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		problem = "the transfer failed (nobody at that address?)";
	} else if (status & STATUS_DROPPED) {
		problem = "the transfer was dropped";
	} else {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	absolutetime_to_nanoseconds(now - start, &taken);
	bus.reads++;
	bus.total_ns += taken;
	bus.last_ns = taken;
	if (taken > bus.longest_ns) {
		bus.longest_ns = taken;
	}
	if (problem != NULL) {
		bus.failed++;
	}
	return problem;
}

/*
 * Which channel is that peripheral's. Several can be: one per system that may write to it. As
 * Linux chooses: the first, unless a later one is ours to write through.
 */
static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		printf("buttons: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    (map & ARB_CHANNEL_INTERRUPTS) ? ", which also gets its interrupts" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
		}
	}
	if (!p->found) {
		printf("buttons: no channel of the arbiter is %s (slave %u, peripheral %#x)\n", p->name, p->id >> 8, p->id & 0xff);
	}
}

/*
 * A read for another driver (sys/hk_input.h): any peripheral of any slave, `id` as above,
 * 1 to 8 registers. Its channel is looked up once (the first one in the table: reads go through
 * our system's observer, whoever may write). The arbiter is mapped here too, so that it does not
 * matter whether the buttons' thread has run. 0, or ENXIO / EIO.
 * shortcut: the lookup table is not locked, one calling thread only (the battery's); lock it for a second caller.
 */
int
hk_spmi_read(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	static struct peripheral known[8];
	static vm_offset_t core, observer;
	struct peripheral *p = NULL;

	if (count == 0 || count > 8) {
		return EINVAL;
	}
	if (observer == 0) {
		core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		if (rd(core + ARB_VERSION) < ARB_VERSION_5 || rd(core + ARB_VERSION) >= ARB_VERSION_7) {
			return ENXIO;
		}
		observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	}
	for (unsigned i = 0; i < 8 && p == NULL; i++) {
		if (known[i].found && known[i].id == id) {
			p = &known[i];
		} else if (!known[i].found) {
			for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
				if (((rd(core + ARB_CHANNEL(n)) >> 8) & 0xfff) == id) {
					known[i].id = id;
					known[i].channel = (uint16_t)n;
					known[i].found = true;
					p = &known[i];
					break;
				}
			}
			if (p == NULL) {
				return ENXIO;
			}
		}
	}
	if (p == NULL) {
		return ENXIO;
	}

	vm_offset_t channel = observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status, data[2];

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hk_spmi_lock();
	wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(channel + OBSERVER_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	hk_spmi_unlock();
	if (status != STATUS_DONE) {
		return EIO;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return 0;
}

/* ---- the two peripherals ------------------------------------------------------------------------- */

/* every peripheral of these PMICs */
#define REG_TYPE                0x04
#define REG_SUBTYPE             0x05
#define REG_RT_STATUS           0x10            /* the inputs as they are now */
#define REG_INT_SET_TYPE        0x11            /* interrupts: a bit per input in each of these */
#define REG_INT_POLARITY_HIGH   0x12
#define REG_INT_POLARITY_LOW    0x13
#define REG_INT_ENABLED         0x15
#define REG_INT_LATCHED         0x18
#define REG_INT_PENDING         0x19

/* the power-on block, of the kind that is split in two ("generation 3"): this is its half for the operating system */
#define PON_TYPE                0x01
#define PON_SUBTYPE_HLOS        0x09
#define PON_KPDPWR              0x80            /* in the status and in the interrupt registers */
#define PON_RESIN               0x40

/* a GPIO of the low or medium voltage kind */
#define GPIO_TYPE               0x10
#define GPIO_SUBTYPE_LV         0x10
#define GPIO_SUBTYPE_MV_VIN3    0x13            /* 0x10 to 0x13: one layout */
#define GPIO_LEVEL              0x01            /* in the status */
#define GPIO_REG_MODE           0x40            /* [1:0]: 0 input, 1 output, 2 both, 3 analog */
#define GPIO_REG_SUPPLY         0x41
#define GPIO_REG_PULL           0x42            /* [2:0]: 0 to 3 up (30, 1.5, 31.5 uA, 1.5 with a boost), 4 down, 5 none */
#define GPIO_REG_ENABLE         0x46            /* [7] */

#define POLL_MS                 16
#define DARK_POLL_MS            250             /* the screen dark and the keys' interrupts armed: a look this often */
#define AFTER_IRQ_MS            500             /* ... and every POLL_MS for this long after one of them */

static struct button {
	const char *    name;
	uint32_t        record;         /* HK_BUTTON_* */
	bool            usable;         /* its input is what it was taken for */
	bool            live;           /* it has been seen released */
	bool            last;           /* the look before this one */
	bool            down;           /* as reported */
	uint64_t        since;          /* when it went down */
	uint32_t        presses;
} power = { .name = "power", .record = HK_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = HK_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = HK_BUTTON_VOLUME_DOWN };

/* "12.345 s (08:13:05.918 UTC)": since boot as in the log's own stamps, and as the daemons' lines have it */
static void
now_text(char *text, size_t size, uint64_t now)
{
	clock_sec_t seconds;
	clock_usec_t micro;
	uint64_t up;

	absolutetime_to_nanoseconds(now, &up);
	clock_get_calendar_microtime(&seconds, &micro);
	snprintf(text, size, "%llu.%03llu s (%02u:%02u:%02u.%03u UTC)", up / NSEC_PER_SEC, up / NSEC_PER_MSEC % 1000,
	    (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60), (unsigned)(micro / 1000));
}

static void
button_look(struct button *b, bool pressed)
{
	uint64_t now = mach_absolute_time(), held;
	bool steady = pressed == b->last;
	char when[64];

	b->last = pressed;
	if (!b->usable || !steady) {
		return;
	}
	if (!b->live) {
		if (!pressed) {
			b->live = true;
			printf("buttons: %s is released now: believed from here on\n", b->name);
		}
		return;
	}
	if (pressed == b->down) {
		return;
	}
	b->down = pressed;
	now_text(when, sizeof(when), now);
	if (pressed) {
		b->since = now;
		b->presses++;
		printf("buttons: %s PRESSED at %s\n", b->name, when);
	} else {
		absolutetime_to_nanoseconds(now - b->since, &held);
		printf("buttons: %s released at %s, after %llu ms\n", b->name, when, held / NSEC_PER_MSEC);
	}
	hk_touch_post(pressed ? HK_BUTTON_DOWN : HK_BUTTON_UP, b->record, 0, 0);
}

/* One look at both peripherals. */
static void
buttons_poll(void)
{
	static unsigned said;
	const char *problem;
	uint8_t status;

	if (power.usable || (volume_down.usable && keys.volume_down_bit)) {
		problem = spmi_read(&pon, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&power, status & keys.power_bit);
			if (keys.volume_down_bit) {
				button_look(&volume_down, status & keys.volume_down_bit);
			}
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pon.name, problem);
		}
	}
	if (volume_up.usable) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&volume_up, !(status & GPIO_LEVEL));
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pin.name, problem);
		}
	}
	if (volume_down.usable && !keys.volume_down_bit) {
		problem = spmi_read(&pin_down, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&volume_down, !(status & GPIO_LEVEL));
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pin_down.name, problem);
		}
	}
}

/* Step 3: is each peripheral what the registers above are those of? */
static bool
identify(const struct peripheral *p, uint8_t type, uint8_t subtype_from, uint8_t subtype_to)
{
	const char *problem;
	uint8_t is[2];

	if (!p->found) {
		return false;
	}
	printf("buttons: next: read the type of %s through channel %u (2 registers, 0x%02x04)\n", p->name, p->channel, p->id & 0xff);
	problem = spmi_read(p, REG_TYPE, is, 2);
	if (problem != NULL) {
		printf("buttons: %s: %s\n", p->name, problem);
		return false;
	}
	printf("buttons: %s says type %#x, subtype %#x (the read took %llu us)\n", p->name, is[0], is[1], bus.last_ns / 1000);
	if (is[0] != type || is[1] < subtype_from || is[1] > subtype_to) {
		printf("buttons: ... which is not what this driver knows (type %#x, subtype %#x to %#x): left alone\n", type,
		    subtype_from, subtype_to);
		return false;
	}
	return true;
}

/* Step 4 for the power-on block: nothing to check but what it says now; its interrupt setup goes to the log. */
static void
pon_inputs(void)
{
	const char *problem;
	uint8_t r[8], more[2];

	printf("buttons: next: read the status and the interrupt setup of %s (registers 0x%02x10 to 0x%02x19)\n", pon.name,
	    pon.id & 0xff, pon.id & 0xff);
	problem = spmi_read(&pon, REG_RT_STATUS, r, 8);
	if (problem == NULL) {
		problem = spmi_read(&pon, REG_INT_LATCHED, more, 2);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pon.name, problem);
		return;
	}
	printf("buttons: power-on block: status %#x (power %s, volume down %s); interrupts: edge %#x, high %#x, low %#x, "
	    "enabled %#x, latched %#x, pending %#x\n", r[0], (r[0] & keys.power_bit) ? "HELD" : "released",
	    !keys.volume_down_bit ? "(a pin)" : (r[0] & keys.volume_down_bit) ? "HELD" : "released",
	    r[REG_INT_SET_TYPE - REG_RT_STATUS], r[REG_INT_POLARITY_HIGH - REG_RT_STATUS],
	    r[REG_INT_POLARITY_LOW - REG_RT_STATUS], r[REG_INT_ENABLED - REG_RT_STATUS], more[0], more[1]);
	power.usable = true;
	power.last = r[0] & keys.power_bit;
	if (keys.volume_down_bit) {
		volume_down.usable = true;
		volume_down.last = r[0] & keys.volume_down_bit;
	}
}

/* Step 4 for a pin: it is the volume key only if it is an input with a pull-up, and switched on. */
static void
pin_input(const struct peripheral *pin, struct button *key)
{
	const char *problem;
	uint8_t c[8], status = 0;
	unsigned mode, pull;

	printf("buttons: next: read the setup and the level of %s (registers 0x%02x40 to 0x%02x47, 0x%02x10)\n", pin->name,
	    pin->id & 0xff, pin->id & 0xff, pin->id & 0xff);
	problem = spmi_read(pin, GPIO_REG_MODE, c, 8);
	if (problem == NULL) {
		problem = spmi_read(pin, REG_RT_STATUS, &status, 1);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pin->name, problem);
		return;
	}
	mode = c[0] & 3;
	pull = c[GPIO_REG_PULL - GPIO_REG_MODE] & 7;
	printf("buttons: %s's pin: mode %u (%s), supply %u, pull %u (%s), %s; registers 0x40.. = %02x %02x %02x %02x %02x %02x "
	    "%02x %02x; level %s\n", key->name, mode, mode == 0 ? "input" : mode == 1 ? "output" : mode == 2 ? "input and output" : "analog",
	    c[GPIO_REG_SUPPLY - GPIO_REG_MODE] & 7, pull, pull < 4 ? "up" : pull == 4 ? "down" : "none",
	    (c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80) ? "enabled" : "DISABLED", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
	    (status & GPIO_LEVEL) ? "high (released)" : "LOW (held)");
	if (mode != 0 || pull >= 4 || !(c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80)) {
		printf("buttons: %s's pin is not an enabled input with a pull-up. Making it one is a write to the PMIC, "
		    "which this driver does not do: %s is left alone\n", key->name, key->name);
		return;
	}
	key->usable = true;
	key->last = !(status & GPIO_LEVEL);
}

/* ---- the power key as the system's wake (bsd/dev/hk_power.c) ------------------------------------- */

/*
 * The system sleeps with every processor but one switched off and that one in WFI; nothing polls the keys then.
 * What wakes it is the power key's interrupt, as on Android (qpnp-power-on's "kpdpwr", both edges):
 *   - the power-on block raises it: interrupt type edge, polarity high and low, enabled, for KPDPWR (bit 0 of
 *     0x0811, 0x0812, 0x0813, 0x0815); after one its latched status is cleared (0x0814). Writes to the PMIC,
 *     the only ones this driver makes, through the channel apps owns (13 on the OnePlus 8: "written by system
 *     0, which also gets its interrupts"), each read back;
 *   - the arbiter passes it on: the channel's ACC_ENABLE (chnls, 0x100 of its 0x10000), its status and clear
 *     (0x104, 0x108), and apps' summary of the channels that have one (intr, 4 bytes per 32 channels);
 *   - the power domain controller (PDC, 0x0b220000) forwards the arbiter's one line ("periph_irq", its pin 1) to
 *     the interrupt controller as SPI 481: pin 1 enabled (IRQ_ENABLE_BANK 0x10), level high (IRQ_i_CFG 0x110 +
 *     4 n, 0b100), as Linux's qcom-pdc.c sets it -- unless the firmware has already;
 *   - SPI 481 (interrupt 513), level, taken by this driver: awake, it acknowledges and wakes the thread, which looks
 *     at the keys (every POLL_MS while the screen is lit; dark, DARK_POLL_MS unless an interrupt came); asleep,
 *     hk_power.c sees it pending and asks hk_buttons_wake_check().
 * The volume keys' pins (GPIO6/GPIO7 on the OnePlus 8) the same way, both edges of their input (bit 0 of their
 * 0x11..0x15), through their own channels -- if those are apps' with interrupts, as Android's gpio_keys has them.
 * Only so does the thread wait in the dark; a key seen to change with no interrupt for it ends that (polled again).
 * Only on the OnePlus 8 (its PON at 0x0800, no HLOS half), and only if every step reads back as written:
 * otherwise hk_power.c looks at the key itself while asleep.
 */
#define ARB_CHNLS_PHYS          hk_board_addr("spmi_chnls", 0x0c600000UL)
#define ARB_INTR_PHYS           hk_board_addr("spmi_intr", 0x0e700000UL)
#define ARB_ACC_ENABLE          0x100
#define ARB_IRQ_STATUS          0x104
#define ARB_IRQ_CLEAR           0x108
#define CHNL_CMD                0x00
#define CHNL_STATUS             0x08
#define CHNL_WDATA0             0x10
#define PDC_PHYS                hk_board_addr("pdc", 0x0b220000UL)
#define PDC_ENABLE_BANK(n)      (0x10 + 4 * (n))
#define PDC_CFG(pin)            (0x110 + 4 * (pin))
#define PDC_LEVEL_HIGH          0x4
#define PDC_ARB_PIN             1
#define ARB_INTID               (481 + 32)
#define REG_INT_LATCHED_CLR     0x14
#define REG_INT_EN_SET          0x15

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern bool hk_display_on(void);
int hk_buttons_wake_check(void);
bool hk_buttons_wake_armed(void);
bool hk_buttons_power_held(void);
void hk_buttons_woken(void);

static vm_offset_t wake_chnl, wake_intr, pin_chnl, pin_down_chnl;
static bool wake_armed, pins_armed;
static volatile uint32_t keys_irq;      /* interrupts of the keys so far: the thread's wait (buttons_thread) */
static volatile bool wake_pressed;      /* woken by the power key: the press is told to iOS once the thread runs again */
static uint64_t wake_irqs, wake_others;

/*
 * One register written through the power key's channel, and the arbiter's answer waited for: a command given while
 * the one before is still going is dropped (the enable after the latched status's clear was, on the phone). The
 * status word as the arbiter left it; STATUS_DONE alone: written. Any context.
 */
static uint32_t
chnl_write(vm_offset_t chnl, uint8_t reg, uint8_t value)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hk_spmi_lock();
	wr(chnl + CHNL_WDATA0, value);
	wr(chnl + CHNL_CMD, (uint32_t)reg << 4);       /* register write, 1 byte */
	do {
		status = rd(chnl + CHNL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	hk_spmi_unlock();
	return status;
}

static const char *
spmi_write(const struct peripheral *p, vm_offset_t chnl, uint8_t reg, uint8_t value)
{
	static char why[48];
	uint32_t status = chnl_write(chnl, reg, value);
	uint8_t back = 0;
	const char *problem;

	if (status != STATUS_DONE) {
		snprintf(why, sizeof(why), "the write was not done (status %#x)", status);
		return why;
	}
	problem = spmi_read(p, reg, &back, 1);
	return problem != NULL ? problem : back != value ? "it reads back otherwise" : NULL;
}

/*
 * Awake: the power key's interrupt only acknowledged (its press is seen by the next look). One that keeps coming with
 * nothing of the power key's in it (a register of the arbiter not as read here) is given up after 16 in a row: the
 * line is level-triggered, and it would never stop.
 */
static uint32_t wake_empty;

static void
wake_interrupt(void *context __unused, uint32_t intid)
{
	wake_irqs++;
	if (hk_buttons_wake_check()) {
		wake_empty = 0;
		thread_wakeup((event_t)&keys_irq);
	} else if (++wake_empty == 16 && wake_armed) {
		const char *line = "buttons: the arbiter's interrupt keeps coming with nothing of the power key's: given up\n";

		gic_v3_spi_mask(intid);
		wake_armed = false;
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
}

/* a volume key's pin, if its channel has an interrupt: acknowledged (arbiter and PMIC). true: it had one */
static bool
pin_ack(const struct peripheral *p, vm_offset_t chnl, uint32_t word, uint32_t *pending)
{
	uint32_t bit = 1u << (p->channel % 32);

	if (chnl == 0 || word != p->channel / 32 || !(*pending & bit)) {
		return false;
	}
	wr(chnl + ARB_IRQ_CLEAR, rd(chnl + ARB_IRQ_STATUS));
	(void)chnl_write(chnl, REG_INT_LATCHED_CLR, 1);
	*pending &= ~bit;
	return true;
}

/*
 * After an interrupt of the arbiter, with the interrupt masked or taken: the keys' acknowledged. 1: it was the power
 * key's; 2: only a volume key's (no reason to end a sleep: an iPhone's volume keys do not wake it); 0: none. Another channel with one (enabled by somebody else; nobody here would quiet it, and the line is
 * level-triggered) has the interrupt given up: the sleep looks at the key itself from then on. Any context.
 */
int
hk_buttons_wake_check(void)
{
	int power_key = 0;
	bool volume = false;

	if (!wake_armed) {
		return 0;
	}
	for (uint32_t word = 0; word < ARB_MAX_CHANNELS / 32; word++) {
		uint32_t pending = rd(wake_intr + 4 * word);

		volume |= pin_ack(&pin, pin_chnl, word, &pending);
		volume |= pin_ack(&pin_down, pin_down_chnl, word, &pending);

		if (word == pon.channel / 32 && (pending & (1u << (pon.channel % 32)))) {
			uint32_t status = rd(wake_chnl + ARB_IRQ_STATUS);

			wr(wake_chnl + ARB_IRQ_CLEAR, status);
			power_key |= (status & keys.power_bit) != 0;
			pending &= ~(1u << (pon.channel % 32));
		}
		if (pending != 0 && wake_others++ == 0) {
			char line[128];

			gic_v3_spi_mask(ARB_INTID);
			wake_armed = false;
			/* (interrupts may be off here: the loader's log only, no console) */
			snprintf(line, sizeof(line), "buttons: the arbiter has interrupts of other channels (%#x in word %u): "
			    "its interrupt given up\n", pending, word);
			for (const char *c = line; *c != 0; c++) {
				loader_log_putc(*c);
			}
		}
	}
	if (power_key) {
		(void)chnl_write(wake_chnl, REG_INT_LATCHED_CLR, keys.power_bit);
	}
	if (power_key || volume) {
		keys_irq++;
	}
	return power_key ? 1 : volume ? 2 : 0;
}

/*
 * A volume key's pin as an interrupt (wake_arm, before the arbiter lets any through): both edges of its input, its
 * latched status cleared, enabled -- each read back. The channel's ACC_ENABLE is the caller's. false: polled.
 */
static bool
pin_arm(const struct peripheral *p, vm_offset_t *chnl)
{
	static const uint8_t regs[] = { REG_INT_SET_TYPE, REG_INT_POLARITY_HIGH, REG_INT_POLARITY_LOW };
	const char *problem = NULL;
	uint8_t r = 0;

	if (!p->found || (rd(arb_core + ARB_CHANNEL(p->channel)) & ARB_CHANNEL_INTERRUPTS) == 0 ||
	    (rd(arb_cnfg + ARB_OWNER(p->channel)) & 7) != ARB_SYSTEM) {
		printf("buttons: %s's interrupt is not this system's to take: the keys are polled\n", p->name);
		return false;
	}
	*chnl = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * p->channel, 0x1000);
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]) && problem == NULL; i++) {
		problem = spmi_read(p, regs[i], &r, 1);
		if (problem == NULL && !(r & 1)) {
			problem = spmi_write(p, *chnl, regs[i], r | 1);
		}
	}
	if (problem == NULL) {
		wr(*chnl + ARB_IRQ_CLEAR, 0xff);
		(void)chnl_write(*chnl, REG_INT_LATCHED_CLR, 1);
		if (spmi_write(p, *chnl, REG_INT_EN_SET, 1) != NULL &&
		    (spmi_read(p, REG_INT_ENABLED, &r, 1) != NULL || !(r & 1))) {
			problem = "not enabled";
		}
	}
	if (problem != NULL) {
		printf("buttons: %s's interrupt: %s: the keys are polled\n", p->name, problem);
		*chnl = 0;
		return false;
	}
	return true;
}

/* Set the power key's interrupt up, once, after step 4 (the thread). true: it works all the way to SPI 481. */
static bool
wake_arm(void)
{
	static const struct {
		uint8_t reg;
		const char *what;
	} steps[] = {
		{ REG_INT_SET_TYPE, "type edge" }, { REG_INT_POLARITY_HIGH, "polarity high" },
		{ REG_INT_POLARITY_LOW, "polarity low" },
	};
	uint8_t r = 0;
	const char *problem = NULL;
	vm_offset_t pdc;
	uint32_t bank, cfg;
	char board[32] = "";

	if (!PE_parse_boot_argn("board", board, sizeof(board)) || strcmp(board, "instantnoodle") != 0 || !power.usable ||
	    (rd(arb_core + ARB_CHANNEL(pon.channel)) & ARB_CHANNEL_INTERRUPTS) == 0 ||
	    (rd(arb_cnfg + ARB_OWNER(pon.channel)) & 7) != ARB_SYSTEM) {
		printf("buttons: the power key's interrupt is not this system's to take: the sleep looks at the key itself\n");
		return false;
	}
	wake_chnl = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * pon.channel, 0x1000);
	wake_intr = ml_io_map(ARB_INTR_PHYS + 0x10000UL * ARB_SYSTEM, 0x1000);
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]) && problem == NULL; i++) {
		problem = spmi_read(&pon, steps[i].reg, &r, 1);
		if (problem == NULL && !(r & keys.power_bit)) {
			problem = spmi_write(&pon, wake_chnl, steps[i].reg, r | keys.power_bit);
		}
		if (problem != NULL) {
			printf("buttons: the power key's interrupt, %s: %s\n", steps[i].what, problem);
		}
	}
	if (problem != NULL) {
		return false;
	}
	wr(wake_chnl + ARB_IRQ_CLEAR, 0xff);
	(void)chnl_write(wake_chnl, REG_INT_LATCHED_CLR, keys.power_bit);
	if ((problem = spmi_write(&pon, wake_chnl, REG_INT_EN_SET, keys.power_bit)) != NULL &&
	    (spmi_read(&pon, REG_INT_ENABLED, &r, 1) != NULL || !(r & keys.power_bit))) {
		printf("buttons: the power key's interrupt, enable: %s\n", problem);
		return false;
	}
	/*
	 * Channels of ours with an interrupt let through already (the firmware's: nobody here would take it, and the line
	 * would stay raised): theirs taken away -- ACC_ENABLE cleared, the status acknowledged --, each said once.
	 */
	for (uint32_t word = 0; word < ARB_MAX_CHANNELS / 32; word++) {
		uint32_t pending = rd(wake_intr + 4 * word) & ~(word == pon.channel / 32 ? 1u << (pon.channel % 32) : 0);

		while (pending != 0) {
			uint32_t n = word * 32 + (uint32_t)__builtin_ctz(pending);
			vm_offset_t chnl;

			pending &= pending - 1;
			if ((rd(arb_cnfg + ARB_OWNER(n)) & 7) != ARB_SYSTEM) {
				printf("buttons: channel %u (%#x) has an interrupt and is not ours: the sleep looks at the key itself\n",
				    n, rd(arb_core + ARB_CHANNEL(n)));
				return false;
			}
			chnl = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * n, 0x1000);
			wr(chnl + ARB_ACC_ENABLE, 0);
			wr(chnl + ARB_IRQ_CLEAR, rd(chnl + ARB_IRQ_STATUS));
			printf("buttons: channel %u (peripheral %#x) had its interrupt let through, by the firmware: taken away\n", n,
			    (rd(arb_core + ARB_CHANNEL(n)) >> 8) & 0xfff);
		}
		if ((rd(wake_intr + 4 * word) & ~(word == pon.channel / 32 ? 1u << (pon.channel % 32) : 0)) != 0) {
			printf("buttons: the arbiter's interrupts of other channels stay (word %u): the sleep looks at the key itself\n",
			    word);
			return false;
		}
	}
	pins_armed = (!volume_up.usable || pin_arm(&pin, &pin_chnl)) &&
	    (!volume_down.usable || keys.volume_down_bit || pin_arm(&pin_down, &pin_down_chnl));
	if (pins_armed) {
		if (pin_chnl != 0) {
			wr(pin_chnl + ARB_ACC_ENABLE, 1);
		}
		if (pin_down_chnl != 0) {
			wr(pin_down_chnl + ARB_ACC_ENABLE, 1);
		}
	} else {
		pin_chnl = pin_down_chnl = 0;
	}
	wr(wake_chnl + ARB_ACC_ENABLE, 1);
	pdc = ml_io_map(PDC_PHYS, 0x1000);
	bank = rd(pdc + PDC_ENABLE_BANK(PDC_ARB_PIN / 32));
	cfg = rd(pdc + PDC_CFG(PDC_ARB_PIN));
	if (!(bank & (1u << (PDC_ARB_PIN % 32))) || (cfg & 7) != PDC_LEVEL_HIGH) {
		wr(pdc + PDC_CFG(PDC_ARB_PIN), PDC_LEVEL_HIGH);
		wr(pdc + PDC_ENABLE_BANK(PDC_ARB_PIN / 32), bank | 1u << (PDC_ARB_PIN % 32));
	}
	printf("buttons: the power key's interrupt: PON %s, arbiter channel %u, PDC pin %u (was enabled %u, type %#x; now %u, %#x), "
	    "SPI %u; the volume keys' %s\n", pon.name, pon.channel, PDC_ARB_PIN, (bank >> (PDC_ARB_PIN % 32)) & 1, cfg & 7,
	    (rd(pdc + PDC_ENABLE_BANK(PDC_ARB_PIN / 32)) >> (PDC_ARB_PIN % 32)) & 1, rd(pdc + PDC_CFG(PDC_ARB_PIN)) & 7, ARB_INTID - 32,
	    pins_armed ? "too (channels of the pins)" : "not: polled");
	wake_armed = true;
	if (gic_v3_spi_attach(ARB_INTID, false, wake_interrupt, NULL) != KERN_SUCCESS) {
		printf("buttons: interrupt %u is taken: the sleep looks at the key itself\n", ARB_INTID);
		wake_armed = pins_armed = false;
	}
	return wake_armed;
}

/* For hk_power.c: whether the sleep can wait for the interrupt (else it looks at the key itself) */
bool
hk_buttons_wake_armed(void)
{
	return wake_armed;
}

/* ... the key itself, for that: held now. Any context. */
bool
hk_buttons_power_held(void)
{
	uint8_t status = 0;

	return power.usable && spmi_read(&pon, REG_RT_STATUS, &status, 1) == NULL && (status & keys.power_bit);
}

/* Woken by the power key: if its press is over by the time the thread looks again, the press is told all the same. */
void
hk_buttons_woken(void)
{
	wake_pressed = true;
}

/* After a wake, every processor back (hk_power_did_wake): the thread looks at once, not after its dark wait. */
void hk_buttons_kick(void);

void
hk_buttons_kick(void)
{
	keys_irq++;
	thread_wakeup((event_t)&keys_irq);
}

/*
 * A write for another driver (sys/hk_input.h): one register of any peripheral, through the channel this system owns
 * for it (none: ENXIO -- a peripheral owned by another system is not ours to write), waited for and read back.
 * 0, or ENXIO / EIO. Thread context (the channel's page is mapped the first time). Up to four peripherals.
 */
int
hk_spmi_write(uint16_t id, uint8_t reg, uint8_t value)
{
	static struct {
		uint16_t id;
		uint16_t channel;
		vm_offset_t chnl;
	} known[4];
	static vm_offset_t core, cnfg;
	vm_offset_t chnl = 0;
	uint64_t start, interval;
	uint32_t status;
	uint8_t back = 0;

	if (core == 0) {
		core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
		if (rd(core + ARB_VERSION) < ARB_VERSION_5 || rd(core + ARB_VERSION) >= ARB_VERSION_7) {
			return ENXIO;
		}
	}
	for (unsigned i = 0; i < 4 && chnl == 0; i++) {
		if (known[i].chnl != 0 && known[i].id == id) {
			chnl = known[i].chnl;
		} else if (known[i].chnl == 0) {
			for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
				if (((rd(core + ARB_CHANNEL(n)) >> 8) & 0xfff) == id && (rd(cnfg + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
					known[i].id = id;
					known[i].channel = (uint16_t)n;
					known[i].chnl = chnl = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * n, 0x1000);
					break;
				}
			}
			if (chnl == 0) {
				return ENXIO;
			}
		}
	}
	if (chnl == 0) {
		return ENXIO;
	}
	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	start = mach_absolute_time();
	hk_spmi_lock();
	wr(chnl + CHNL_WDATA0, value);
	wr(chnl + CHNL_CMD, (uint32_t)reg << 4);       /* register write, 1 byte */
	do {
		status = rd(chnl + CHNL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	hk_spmi_unlock();
	if (status != STATUS_DONE || hk_spmi_read(id, reg, &back, 1) != 0 || back != value) {
		return EIO;
	}
	return 0;
}

static void
say_summary(void)
{
	printf("buttons: so far %u presses of power, %u of volume up, %u of volume down; now%s%s%s%s; %llu reads of the bus, "
	    "%llu us each (the longest %llu), %llu failed\n", power.presses, volume_up.presses, volume_down.presses,
	    power.down ? " power" : "", volume_up.down ? " volume-up" : "", volume_down.down ? " volume-down" : "",
	    (power.down || volume_up.down || volume_down.down) ? " held" : " none held", bus.reads,
	    bus.reads ? bus.total_ns / bus.reads / 1000 : 0, bus.longest_ns / 1000, bus.failed);
}

static void
buttons_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, version, channels;
	bool pon_known, pin_known, pin_down_known = false;

	printf("buttons: step 1: map the PMIC arbiter (%#lx, %#lx, %#lx) and read its version\n", ARB_CORE_PHYS, ARB_CNFG_PHYS,
	    ARB_OBSERVER_PHYS);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	printf("buttons: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("buttons: this driver knows version 5 of the arbiter only: no buttons\n");
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	if (steps < 2) {
		goto out;
	}

	printf("buttons: step 2: look through the arbiter's table for the channels of the two peripherals\n");
	find_channel(&pon, channels);
	find_channel(&pin, channels);
	if (pin_down.id != 0) {
		find_channel(&pin_down, channels);
	}
	if (steps < 3) {
		goto out;
	}

	printf("buttons: step 3: the first transfers on the SPMI bus\n");
	pon_known = identify(&pon, PON_TYPE, keys.pon_subtype_from, keys.pon_subtype_to);
	pin_known = identify(&pin, GPIO_TYPE, GPIO_SUBTYPE_LV, GPIO_SUBTYPE_MV_VIN3);
	if (pin_down.id != 0) {
		pin_down_known = identify(&pin_down, GPIO_TYPE, GPIO_SUBTYPE_LV, GPIO_SUBTYPE_MV_VIN3);
	}
	if (steps < 4) {
		goto out;
	}

	printf("buttons: step 4: how the keys' inputs are set up\n");
	if (pon_known) {
		pon_inputs();
	}
	if (pin_known) {
		pin_input(&pin, &volume_up);
	}
	if (pin_down_known) {
		pin_input(&pin_down, &volume_down);
	}
	if (!power.usable && !volume_up.usable) {
		printf("buttons: nothing that can be read: no buttons\n");
		goto out;
	}
	if (power.last || volume_down.last || volume_up.last) {
		printf("buttons: a key reads as held before anything was watched:%s%s%s. It is not reported until it has been "
		    "seen released\n", power.last ? " power" : "", volume_up.last ? " volume-up" : "",
		    volume_down.last ? " volume-down" : "");
	}
	/* (a key that is released now is believed at once) */
	power.live = power.usable && !power.last;
	volume_down.live = volume_down.usable && !volume_down.last;
	volume_up.live = volume_up.usable && !volume_up.last;
	if (steps < 5) {
		goto out;
	}

	{
		uint32_t wake = 1;

		/* buttons_wake=0: the system's sleep looks at the key itself (hk_power.c) */
		PE_parse_boot_argn("buttons_wake", &wake, sizeof(wake));
		if (wake) {
			wake_arm();
		}
	}
	printf("buttons: step 5: watching%s%s%s, every %u ms%s: power is the side button\n", power.usable ? " power" : "",
	    volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", POLL_MS,
	    wake_armed && pins_armed ? " (screen dark: on their interrupts)" : "");
	uint32_t seen = keys_irq;
	for (uint64_t start = mach_absolute_time(), said = start, busy_until = 0, now, ns;;) {
		bool before[3] = { power.last, volume_up.last, volume_down.last }, waited = false;

		if (wake_armed && pins_armed && !hk_display_on() && !power.last && !volume_up.last && !volume_down.last &&
		    mach_absolute_time() >= busy_until) {
			/* dark and nothing held: until a key's interrupt (DARK_POLL_MS at most) */
			assert_wait_timeout((event_t)&keys_irq, THREAD_UNINT, DARK_POLL_MS, NSEC_PER_MSEC);
			if (keys_irq != seen) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			waited = keys_irq == seen;
		} else {
			delay_for_interval(POLL_MS, NSEC_PER_MSEC);
		}
		if (keys_irq != seen) {
			nanoseconds_to_absolutetime(AFTER_IRQ_MS * NSEC_PER_MSEC, &ns);
			busy_until = mach_absolute_time() + ns;
		}
		seen = keys_irq;                        /* (one after this is seen by the next wait) */
		if (wake_pressed) {
			/* the press that woke the system, over before this thread ran again: told as it was */
			wake_pressed = false;
			if (power.live && !power.down && !hk_buttons_power_held()) {
				power.presses++;
				printf("buttons: power pressed and released while the system woke: told now\n");
				hk_touch_post(HK_BUTTON_DOWN, power.record, 0, 0);
				delay_for_interval(80, NSEC_PER_MSEC);  /* (a press as a hand makes one: 80 ms in the logs) */
				hk_touch_post(HK_BUTTON_UP, power.record, 0, 0);
			}
		}
		buttons_poll();
		if (waited && keys_irq == seen &&
		    (power.last != before[0] || volume_up.last != before[1] || volume_down.last != before[2])) {
			pins_armed = false;
			printf("buttons: a key changed with no interrupt for it: polled every %u ms from here on\n", POLL_MS);
		}
		/* once a minute, and once early on as well: a short run is to say what the reads cost too */
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - said, &ns);
		if (ns >= 60 * NSEC_PER_SEC || (said == start && ns >= 10 * NSEC_PER_SEC)) {
			said = now;
			say_summary();
		}
		if (bus.failed >= 1000 && bus.failed * 2 > bus.reads) {
			printf("buttons: more than half of %llu reads have failed: giving up\n", bus.reads);
			break;
		}
	}
out:
	if (steps < 5) {
		printf("buttons: stopping here (buttons_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hk_buttons_init(void)
{
	uint32_t enabled = 0, steps = 5;
	char board[32] = "";
	thread_t thread;

	keys_for_board();

	/*
	 * On with the touch screen (the phone, somebody at it) unless buttons=0 says otherwise: a hand
	 * has pressed all three keys and each press and release was read as it was made.
	 */
	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		uint32_t touch_screen = 0;

		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
		/* the OnePlus 8: on by default (only reads; buttons=0 turns it off) */
		enabled |= PE_parse_boot_argn("board", board, sizeof(board)) && strcmp(board, "instantnoodle") == 0;
	}
	if (!enabled) {
		return;
	}
	PE_parse_boot_argn("buttons_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(buttons_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * What interrupts would take, instead of looking 60 times a second:
 *
 *   - The arbiter has one interrupt line for all the PMICs' peripherals ("periph_irq": through the
 *     power domain controller's pin 1 to the interrupt controller's SPI 481). This kernel has no
 *     driver that takes a hardware interrupt other than the timer's so far; that is the first
 *     thing to add (a handler registered for one more interrupt of the GICv3 the kernel already
 *     drives, and that line unmasked and set to level, high).
 *   - Per channel, the arbiter has an enable bit ("ACC_ENABLE"), a status and a clear register
 *     (in the "chnls" region: 0x100, 0x104, 0x108 of 0x10000 per channel), and per system a
 *     summary of which channels have something. Those are writes to the arbiter, not to a PMIC.
 *   - But the peripheral must be told to raise them as well: for each key a bit in "interrupt
 *     type" (edge), "polarity high" and "polarity low" (both edges), and "enable set". Those are
 *     WRITES TO THE PMIC -- to the power-on block for power and volume down, to the GPIO for
 *     volume up -- unless the firmware has left them set; step 4 prints what it finds there.
 *     After an interrupt the peripheral's latched status is cleared by another write to it.
 *   - Only the system that owns a peripheral's channel gets its interrupts (step 2 prints the
 *     owners).
 * Looking costs two reads of some tens of microseconds each, 60 times a second (the minute's
 * summary line says what they really take), so the reason to want interrupts would not be
 * processor time; it would be a system that sleeps and is to be woken by a key, and this one
 * does not sleep.
 */
