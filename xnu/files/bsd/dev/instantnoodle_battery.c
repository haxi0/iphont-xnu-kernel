/*
 * The battery of the OnePlus 8 (instantnoodle, SM8250), for the power source iOS reads
 * (hk_battery_report() in hk_battery.c, then iokit/Kernel/HKBattery.cpp).
 *
 * On redwood the numbers come from the audio DSP's battery manager. This phone has none: its
 * Android reads a TI fuel gauge of OnePlus's own choosing, "ti,bq27541-battery" at I2C address
 * 0x55 on serial engine 16 (i2c@888000, pins gpio48 data / gpio49 clock, SOUTH tile; Android's
 * live device tree), with OnePlus's driver drivers/oneplus/power/supply/qcom/bq27541_fuelgauger.c
 * (android_kernel_oneplus_sm8250). The register map is that driver's: the bq27541 one, or the
 * bq27411 one when the gauge says it is one (control subcommand DEVICE_TYPE, 0x0421).
 *
 * Whether a charger is on the USB port: the PM8150B's charger (SPMI slave 2), USBIN's real-time
 * status (0x1310, bit 4 "plugged in") and the charger's state (0x1006 [2:0]: Linux smb5-reg.h),
 * read through the buttons' SPMI code.
 *
 * Only reads, and one control subcommand at the start (asking the gauge its type, which changes
 * nothing). Nothing is told to the charger: it charges as the boot chain left it set up. The
 * OnePlus fast-charge MCU on the same bus (0x26, "Warp") is left alone; without it the phone
 * charges at the PM8150B's normal rate.
 *
 * On with board=instantnoodle; battery=0 turns it off. Every 5 s; a line in the log every minute.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/instantnoodle.h>
#include <sys/hk_input.h>
#include <sys/systm.h>


#define GAUGE_ADDR              0x55
#define DEVICE_TYPE_BQ27411     0x0421
#define SUBCMD_DEVICE_TYPE      0x0001
#define REG_CNTL                0x00
#define FLAG_FC                 (1u << 9)       /* bq27541 FLAGS: full charge */

#define PM8150B_CHGR            0x210           /* slave 2, peripheral 0x10 */
#define PM8150B_USBIN           0x213
#define CHGR_STATUS_1           0x06            /* [2:0] trickle, pre, full-on, taper, terminate, pause, disable */
#define USBIN_RT_STS            0x10
#define USBIN_PLUGIN            (1u << 4)

static struct instantnoodle_i2c bus = {
	.name = "se16",
	.sda = { INSTANTNOODLE_TLMM_SOUTH, 48 },
	.scl = { INSTANTNOODLE_TLMM_SOUTH, 49 },
	.pull = INSTANTNOODLE_PULL_UP,          /* as the boot chain leaves the pins (0x7): no answer without it */
};

/* the gauge's registers, by its type (OnePlus's gauge_set_cmd_addr()) */
static struct {
	const char *name;
	uint8_t temp, volt, flags, rm, fcc, ai, soc, cycles, design;
} map = { "bq27541", 0x06, 0x08, 0x0a, 0x10, 0x12, 0x14, 0x2c, 0x2a, 0x3c };

static int
word(uint8_t reg, int *value)
{
	uint8_t b[2];
	int err = instantnoodle_i2c_read(&bus, GAUGE_ADDR, reg, b, 2);

	if (err == 0) {
		*value = b[0] | b[1] << 8;
	}
	return err;
}

static int
device_type(void)
{
	uint8_t cmd[3] = { REG_CNTL, SUBCMD_DEVICE_TYPE & 0xff, SUBCMD_DEVICE_TYPE >> 8 };
	int type = -1;

	if (instantnoodle_i2c_write(&bus, GAUGE_ADDR, cmd, sizeof(cmd)) != 0) {
		return -1;
	}
	delay(66);                                      /* as OnePlus's driver waits */
	return word(REG_CNTL, &type) == 0 ? type : -1;
}

/* one reading; false when the gauge did not answer */
static bool
sample(struct hk_battery_state *b, int *flags_out, uint8_t *charger_state)
{
	int soc, mv, ma, temp_dk, flags = 0, rm = 0, fcc = 0, cycles = 0, design = 0;
	uint8_t usbin = 0, chgr = 0xff;

	if (word(map.soc, &soc) || word(map.volt, &mv) || word(map.ai, &ma) || word(map.temp, &temp_dk)) {
		return false;
	}
	word(map.flags, &flags);
	word(map.rm, &rm);
	word(map.fcc, &fcc);
	if (map.cycles) {
		word(map.cycles, &cycles);
	}
	if (map.design) {
		word(map.design, &design);
	}
	ma = (int16_t)ma;                               /* TI's sign: more than 0 while the battery takes charge */
	bzero(b, sizeof(*b));
	b->size = sizeof(*b);
	b->percent_hundredths = (soc > 100 ? 100 : soc) * 100;
	b->voltage_uv = mv * 1000;
	b->current_ua = ma * 1000;
	b->temperature_hundredths = temp_dk * 10 - 27315;       /* tenths of a kelvin */
	b->present = 1;
	b->charge_uah = rm * 1000;
	b->full_uah = fcc * 1000;
	b->design_uah = design > 1000 && design < 10000 ? design * 1000 : 0;
	b->cycles = cycles < 10000 ? cycles : 0;

	if (hk_spmi_read(PM8150B_USBIN, USBIN_RT_STS, &usbin, 1) == 0) {
		b->external = (usbin & USBIN_PLUGIN) != 0;
	} else {
		b->external = ma > 0;                   /* the charger cannot be asked: taking charge says it is there */
	}
	if (hk_spmi_read(PM8150B_CHGR, CHGR_STATUS_1, &chgr, 1) == 0) {
		chgr &= 7;
	}
	/* Linux POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	if (!b->external) {
		b->status = 2;
	} else if (soc >= 100 || (flags & FLAG_FC) || chgr == 4) {
		b->status = 4;
	} else if (ma > 0 || chgr <= 3) {
		b->status = 1;
	} else {
		b->status = 3;
	}
	*flags_out = flags;
	*charger_state = chgr;
	return true;
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct hk_battery_state b;
	unsigned failed = 0;
	int type, flags;
	uint8_t chgr;

	printf("battery: bq27541 at %#x on %s (gpio%u/%u): pins cfg %#x %#x\n", GAUGE_ADDR, bus.name, bus.sda.pin,
	    bus.scl.pin, instantnoodle_gpio_cfg(bus.sda), instantnoodle_gpio_cfg(bus.scl));
	instantnoodle_i2c_recover(&bus);
	printf("battery: lines let go: sda %d scl %d\n", instantnoodle_gpio_level(bus.sda), instantnoodle_gpio_level(bus.scl));
	type = device_type();
	if (type < 0) {
		char found[200] = "";

		/* who is on this bus at all: an address and nothing else, which writes nothing */
		for (uint8_t a = 0x08; a < 0x78; a++) {
			if (instantnoodle_i2c_write(&bus, a, NULL, 0) == 0) {
				snprintf(found + strlen(found), sizeof(found) - strlen(found), " %#x", a);
			}
		}
		printf("battery: no gauge; on %s answer:%s\n", bus.name, found[0] ? found : " nobody");
	}
	if (type == DEVICE_TYPE_BQ27411) {
		map.name = "bq27411";
		map.temp = 0x02; map.volt = 0x04; map.flags = 0x06; map.rm = 0x0c; map.fcc = 0x0e;
		map.ai = 0x10; map.soc = 0x1c; map.cycles = 0; map.design = 0;
	}
	printf("battery: the gauge says device type %#x: %s register map\n", type, map.name);

	for (unsigned tick = 0;; tick++) {
		if (sample(&b, &flags, &chgr)) {
			failed = 0;
			hk_battery_report(&b);
			if (tick % 12 == 0) {
				printf("battery: %d%%, %d mV, %d mA, %d.%02d C, %d/%d mAh (design %d), %d cycles, flags %#x; "
				    "USB %s, charger state %u, status %d (%u bus errors)\n", b.percent_hundredths / 100,
				    b.voltage_uv / 1000, b.current_ua / 1000, b.temperature_hundredths / 100,
				    (b.temperature_hundredths < 0 ? -b.temperature_hundredths : b.temperature_hundredths) % 100,
				    b.charge_uah / 1000, b.full_uah / 1000, b.design_uah / 1000, b.cycles, flags,
				    b.external ? "plugged" : "unplugged", chgr, b.status, bus.errors);
			}
		} else if (++failed == 1 || failed % 12 == 0) {
			printf("battery: the gauge at %#x does not answer (%u times in a row)\n", GAUGE_ADDR, failed);
			instantnoodle_i2c_recover(&bus);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
instantnoodle_battery_init(void)
{
	uint32_t enabled = 1;
	thread_t thread;

	if (!instantnoodle_board()) {
		return;
	}
	PE_parse_boot_argn("battery", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
