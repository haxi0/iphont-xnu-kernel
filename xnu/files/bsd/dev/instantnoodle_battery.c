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
 *
 * An empty battery shuts the system down in order (reboot_kernel(RB_HALT): every program told to end, the
 * filesystems synced and unmounted, the loader switches the phone off) before the PMIC cuts the power in the middle
 * of a write: no charger and the gauge at 0% or the cell under BATTERY_EMPTY_MV, three readings in a row (15 s),
 * as Android's healthd shuts down at the gauge's 0%. battery_shutdown=0: only said; battery_empty_mv=N another
 * threshold (for trying it: one above the cell's voltage, the cable out).
 *
 * Every ten minutes and when the cable goes in or out, what the charger says of itself (only reads, PM8150B, Linux smb5-reg.h):
 * the port's kind (APSD), the input current limit in force (DCDC ICL_STATUS) and the configured ones, the power path,
 * charging enabled, the battery's temperature state -- for telling why it charges or does not (a computer's port
 * gives 500 mA in "USB 500" mode, less than a busy phone takes).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/instantnoodle.h>
#include <sys/hk_input.h>
#include <sys/reboot.h>
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


#define GAUGE_ADDR              0x55
#define DEVICE_TYPE_BQ27411     0x0421
#define SUBCMD_DEVICE_TYPE      0x0001
#define REG_CNTL                0x00
#define FLAG_FC                 (1u << 9)       /* bq27541 FLAGS: full charge */

#define PM8150B_CHGR            0x210           /* slave 2, peripheral 0x10 */
#define PM8150B_USBIN           0x213
#define CHGR_STATUS_1           0x06            /* [2:0] (smb5-reg.h) inhibit, trickle, pre, full-on, taper, terminate, pause, disable */
#define CHG_TERMINATE           5
#define CHG_PAUSE               6
#define USBIN_RT_STS            0x10
#define USBIN_PLUGIN            (1u << 4)
#define PM8150B_MISC            0x216
#define PM8150B_DCDC            0x211
#define BATTERY_EMPTY_MV        3300
#define BATTERY_LOW_PERCENT     10

extern int reboot_kernel(int howto, char *message);
extern void instantnoodle_touch_charger(bool plugged);
static uint32_t empty_readings, shutdown_on = 1, empty_mv = BATTERY_EMPTY_MV;
static volatile int battery_last_percent = -1, battery_last_external = -1;

/*
 * The charger's input current. The firmware leaves the charger in "USB 500" mode (USBIN_ICL_OPTIONS 0x1366: USB51_MODE,
 * not USBIN_MODE_CHG): right for a computer's port, and a wall charger gets 500 mA from it as well. As OnePlus's own
 * smb5 does for the port's kind (APSD 0x1308; smb5-lib.c smblib_set_icl_current() with SW_OVERRIDE_HC_MODE and its
 * DEFAULT_CDP_MA / DEFAULT_DCP_MA): the limit into USBIN_CURRENT_LIMIT_CFG (0x1370, 50 mA a step, qpnp-smb5.c's
 * usb_icl) -- 1500 mA for a charging downstream port, 2000 mA for a dedicated charger --, high-current mode
 * (USBIN_MODE_CHG), no command override (CMD_ICL_OVERRIDE 0x1342 bit 0 clear) and the limit kept after the port's
 * detection (USBIN_LOAD_CFG 0x1365 ICL_OVERRIDE_AFTER_APSD). The charger's AICL lowers it by itself where the adapter
 * cannot give that much. A computer's port (SDP) is left as the firmware set it. The firmware's values come back when
 * the cable comes out, so that the next cable starts as the firmware would have it. charge_icl=0: never touched.
 */
#define USBIN_APSD_RESULT       0x08
#define USBIN_CMD_ICL_OVERRIDE  0x42
#define USBIN_LOAD_CFG          0x65
#define USBIN_ICL_OPTIONS       0x66
#define USBIN_ICL_CFG           0x70
#define DCDC_AICL_ICL_STATUS    0x08
#define APSD_CDP                (1u << 2)
#define APSD_DCP                (1u << 3)
#define ICL_OVERRIDE            (1u << 0)
#define ICL_OVERRIDE_AFTER_APSD (1u << 4)
#define USBIN_MODE_CHG          (1u << 0)
#define ICL_STEP_MA             50
#define ICL_CDP_MA              1500
#define ICL_DCP_MA              2000

static uint32_t icl_on = 1;
static struct {
	bool saved, applied;
	uint8_t options, load, cfg, override;
} icl;

static int
icl_write(uint8_t options, uint8_t load, uint8_t cfg, uint8_t override)
{
	return hk_spmi_write(PM8150B_USBIN, USBIN_ICL_CFG, cfg) | hk_spmi_write(PM8150B_USBIN, USBIN_LOAD_CFG, load) |
	       hk_spmi_write(PM8150B_USBIN, USBIN_CMD_ICL_OVERRIDE, override) |
	       hk_spmi_write(PM8150B_USBIN, USBIN_ICL_OPTIONS, options);
}

static void
icl_follow(bool plugged)
{
	uint8_t apsd = 0, now = 0;
	uint32_t ma;
	int err;

	if (!icl_on) {
		return;
	}
	if (!icl.saved) {
		icl.saved = hk_spmi_read(PM8150B_USBIN, USBIN_ICL_OPTIONS, &icl.options, 1) == 0 &&
		    hk_spmi_read(PM8150B_USBIN, USBIN_LOAD_CFG, &icl.load, 1) == 0 &&
		    hk_spmi_read(PM8150B_USBIN, USBIN_ICL_CFG, &icl.cfg, 1) == 0 &&
		    hk_spmi_read(PM8150B_USBIN, USBIN_CMD_ICL_OVERRIDE, &icl.override, 1) == 0;
		if (!icl.saved) {
			return;
		}
	}
	if (!plugged) {
		if (icl.applied) {
			err = icl_write(icl.options, icl.load, icl.cfg, icl.override);
			icl.applied = false;
			printf("battery: the cable is out: the charger's input limit as the firmware had it%s\n", err ? ": NOT ALL WRITTEN" : "");
		}
		return;
	}
	if (icl.applied || hk_spmi_read(PM8150B_USBIN, USBIN_APSD_RESULT, &apsd, 1) != 0 || !(apsd & (APSD_CDP | APSD_DCP))) {
		return;
	}
	ma = (apsd & APSD_DCP) ? ICL_DCP_MA : ICL_CDP_MA;
	err = icl_write(icl.options | USBIN_MODE_CHG, icl.load | ICL_OVERRIDE_AFTER_APSD, (uint8_t)(ma / ICL_STEP_MA),
	    icl.override & ~ICL_OVERRIDE);
	icl.applied = true;
	delay_for_interval(500, NSEC_PER_MSEC);
	hk_spmi_read(PM8150B_DCDC, DCDC_AICL_ICL_STATUS, &now, 1);
	printf("battery: a %s: the charger's input limit to %u mA (high-current mode)%s; %u mA after AICL\n",
	    (apsd & APSD_DCP) ? "charger (DCP)" : "charging port (CDP)", ma, err ? ": NOT ALL WRITTEN" : "", now * ICL_STEP_MA);
}

/* For the system's sleep (bsd/dev/hk_power.c): the battery low and no charger at the last reading. */
bool instantnoodle_battery_low(void);

bool
instantnoodle_battery_low(void)
{
	return battery_last_external == 0 && battery_last_percent >= 0 && battery_last_percent <= BATTERY_LOW_PERCENT;
}

/* The charger's own account (reads only): a line */
static void
charger_say(void)
{
	static const struct {
		uint16_t id;
		uint8_t reg;
		const char *name;
	} regs[] = {
		{ PM8150B_CHGR, 0x06, "status1" }, { PM8150B_CHGR, 0x07, "status2" }, { PM8150B_CHGR, 0x42, "chg_en_cmd" },
		{ PM8150B_CHGR, 0x61, "fcc_cfg" }, { PM8150B_CHGR, 0x70, "fv_cfg" }, { PM8150B_USBIN, 0x07, "apsd_status" },
		{ PM8150B_USBIN, 0x08, "apsd_result" }, { PM8150B_USBIN, 0x66, "icl_options" }, { PM8150B_USBIN, 0x70, "icl_cfg" },
		{ PM8150B_CHGR, 0x0d, "status7_temp" }, { PM8150B_USBIN, 0x42, "icl_override" }, { PM8150B_USBIN, 0x65, "load_cfg" },
		{ PM8150B_DCDC, 0x06, "icl_max" }, { PM8150B_DCDC, 0x07, "icl_status" }, { PM8150B_DCDC, 0x08, "aicl_icl" },
		{ PM8150B_DCDC, 0x0a, "aicl" },
		{ PM8150B_DCDC, 0x0b, "power_path" }, { PM8150B_MISC, 0x51, "wd_cfg" },
	};
	char line[400];
	size_t n = 0;

	n += snprintf(line + n, sizeof(line) - n, "battery: charger");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]) && n < sizeof(line); i++) {
		uint8_t v = 0;

		if (hk_spmi_read(regs[i].id, regs[i].reg, &v, 1) == 0) {
			n += snprintf(line + n, sizeof(line) - n, " %s %#x", regs[i].name, v);
		} else {
			n += snprintf(line + n, sizeof(line) - n, " %s -", regs[i].name);
		}
	}
	printf("%s\n", line);
}

/* An empty battery, three readings in a row with no charger: the system shut down in order. */
static void
empty_check(const struct hk_battery_state *b)
{
	int mv = b->voltage_uv / 1000, percent = b->percent_hundredths / 100;

	battery_last_percent = percent;
	battery_last_external = b->external;
	if (b->external || (percent > 0 && mv >= (int)empty_mv)) {
		empty_readings = 0;
		return;
	}
	if (++empty_readings == 3) {
		printf("battery: EMPTY (%d%%, %d mV, no charger, three readings in a row): %s\n", percent, mv,
		    shutdown_on ? "the system shuts down" : "not shut down (battery_shutdown=0)");
		if (shutdown_on) {
			reboot_kernel(RB_HALT, (char *)"battery empty");
		}
	}
}

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
	instantnoodle_touch_charger(b->external != 0);   /* (the touch chip's noise filter for a charger) */
	if (hk_spmi_read(PM8150B_CHGR, CHGR_STATUS_1, &chgr, 1) == 0) {
		chgr &= 7;
	}
	/* Linux POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	if (!b->external) {
		b->status = 2;
	} else if (soc >= 100 || (flags & FLAG_FC) || chgr == CHG_TERMINATE) {
		b->status = 4;
	} else if (ma > 0 || (chgr >= 1 && chgr < CHG_TERMINATE)) {
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
	int said_external = -1;
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
			empty_check(&b);
			icl_follow(b.external);
			if (tick % 120 == 0 || b.external != said_external) {
				said_external = b.external;     /* (every ten minutes, and when the cable goes in or out) */
				charger_say();
			}
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
	PE_parse_boot_argn("battery_shutdown", &shutdown_on, sizeof(shutdown_on));
	PE_parse_boot_argn("battery_empty_mv", &empty_mv, sizeof(empty_mv));  /* (a test: above the cell's voltage, unplugged) */
	PE_parse_boot_argn("charge_icl", &icl_on, sizeof(icl_on));
	if (!enabled) {
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
