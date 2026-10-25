/*
 * The OnePlus 8's alert slider (instantnoodle, SM8250): a magnet in the slider and two iSentek IST8801
 * hall sensors beside it, on the I2C bus of serial engine 5 (i2c@984000: GPIO 4 data, 5 clock), "up" at
 * 0x19 and "down" at 0x18, both powered by LDO10 of the PM8150A (RPMh "ldoc10", its init voltage 3.0 V) (the phone's device tree; OxygenOS's log: both answer with ID 0x81 -- the
 * MagnaChip MXM1120 at 0x0d/0x0c that the tree also has is another build's, and does not answer).
 *
 * As OxygenOS's driver (drivers/oneplus/tri_state_key: ist_hall_ic/hall_ist8801_*.c, oneplus_tri_key.c)
 * sets them up: a soft reset, the test pad's voltage off (0x76 = 4), oversampling 5 (0x6c), persistence 2,
 * measuring at 20 Hz; and its sensitivity, "40mT" (CNTL2 0x2c, the gain as the chip came). Not done: its
 * interrupts -- both sensors are read every 100 ms instead, which is two short transfers.
 *
 * The position is told by the difference of the two (the up sensor's less the down sensor's), as there:
 * where the factory's calibration -- /mnt/vendor/persist/engineermode/tri_state_hall_data, three pairs
 * "down,up" for the slider up, in the middle and down; boot-arg slider_cal=D,U,D,U,D,U -- puts each
 * position, the nearest of the three wins (within 22 of it, OxygenOS's tolerance); without it, OxygenOS's
 * rule for a boot (reboot_get_position): more than 30 is up, less than -30 down, the middle otherwise.
 * A position counts once two readings in a row agree.
 *
 * What is told: a record of type HK_SLIDER on /dev/hk-touch (sys/hk_input.h; backboardd's side, inject.c,
 * makes the ring/silent switch of it and SpringBoard's Do Not Disturb), and sysctl kern.hackios.slider
 * (0 not known, HK_SLIDER_*). boot-arg slider=0: not read.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hk_input.h>
#include <sys/instantnoodle.h>

extern kern_return_t thread_terminate(thread_t thread);
extern bool hk_display_on(void);
extern void loader_log_putc(char c);

/* to the loader's log as well: the start of a run's kernel log may not reach the host */
static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

static struct instantnoodle_i2c bus = {
	.name = "se5",
	.sda = { INSTANTNOODLE_TLMM_NORTH, 4 },
	.scl = { INSTANTNOODLE_TLMM_NORTH, 5 },
	.pull = INSTANTNOODLE_PULL_UP,
};

#define ADDR_UP                 0x19
#define ADDR_DOWN               0x18
#define REG_PERSINT             0x00
#define REG_INTSRS              0x01
#define REG_SRST                0x07
#define REG_OPF                 0x08
#define REG_DID                 0x09
#define REG_ST1                 0x10
#define REG_CNTL2               0x0d
#define REG_ACTION              0x20
#define REG_IFCNTL              0x40
#define REG_GAINCNTL            0x54
#define REG_OSRCNTL             0x6c
#define REG_TSTCNTL             0x76
#define DID_IST8801             0x81
#define OPF_20HZ                0x70
#define CNTL2_40MT              0x2c            /* (GAIN_4_TIME | ADC_RES_10_BIT) & 0x1e | GAIN_2_TIME */

SYSCTL_DECL(_kern_hackios);
static uint32_t position;               /* HK_SLIDER_*, 0 while not known */
SYSCTL_UINT(_kern_hackios, OID_AUTO, slider, CTLFLAG_RD | CTLFLAG_LOCKED, &position, 0, "the alert slider: 1 down, 2 middle, 3 up");

static int
reg_write(uint8_t addr, uint8_t reg, uint8_t value)
{
	uint8_t bytes[2] = { reg, value };

	return instantnoodle_i2c_write(&bus, addr, bytes, 2);
}

static int
reg_read(uint8_t addr, uint8_t reg, uint8_t *value, unsigned n)
{
	return instantnoodle_i2c_read(&bus, addr, reg, value, n);
}

/* ist8801_reset_device(), ist8801_set_operation_mode(MEASUREMENT) and ist8801_set_sensitivity("40mT") */
static bool
sensor_init(uint8_t addr, const char *name)
{
	uint8_t id = 0, gain = 0, ifcntl = 0;

	if (reg_write(addr, REG_SRST, 1) != 0) {
		instantnoodle_i2c_recover(&bus);
		if (reg_write(addr, REG_SRST, 1) != 0) {
			say("slider: the %s sensor (%#x) does not answer\n", name, addr);
			return false;
		}
	}
	delay_for_interval(20, NSEC_PER_MSEC);
	if (reg_read(addr, REG_DID, &id, 1) != 0 || id != DID_IST8801) {
		say("slider: the %s sensor (%#x) says it is %#x, not an IST8801 (%#x)\n", name, addr, id, DID_IST8801);
		return false;
	}
	if (reg_write(addr, REG_TSTCNTL, 0x04) != 0 || reg_write(addr, REG_OSRCNTL, 0x05) != 0 ||
	    reg_read(addr, REG_GAINCNTL, &gain, 1) != 0 || reg_write(addr, REG_PERSINT, 2 << 4) != 0 ||
	    reg_write(addr, REG_INTSRS, 0) != 0 ||                     /* polled: no interrupt */
	    reg_write(addr, REG_ACTION, 0) != 0) {
		return false;
	}
	delay_for_interval(5, NSEC_PER_MSEC);
	if (reg_write(addr, REG_OPF, 0) != 0 || reg_read(addr, REG_IFCNTL, &ifcntl, 1) != 0 ||
	    reg_write(addr, REG_IFCNTL, ifcntl | 0x04) != 0 ||          /* the state machine reset */
	    reg_write(addr, REG_CNTL2, CNTL2_40MT) != 0 || reg_write(addr, REG_GAINCNTL, gain) != 0 ||
	    reg_write(addr, REG_OPF, OPF_20HZ) != 0) {
		return false;
	}
	say("slider: the %s sensor (%#x): IST8801, gain %u, measuring at 20 Hz\n", name, addr, gain);
	return true;
}

/* ist8801_get_data(): the field, if a new one is ready; otherwise the last */
static bool
sensor_read(uint8_t addr, int16_t *value)
{
	uint8_t b[3];

	if (reg_read(addr, REG_ST1, b, 3) != 0) {
		return false;
	}
	if (b[0] & 0x05) {
		*value = (int16_t)(b[2] << 8 | b[1]);
	}
	return true;
}

static int16_t cal_diff[3];             /* up, middle, down: the up sensor's less the down sensor's */
static bool calibrated;

/* boot-arg slider_cal=D,U,D,U,D,U (the factory's file, its ';' as ',') */
static void
calibration(void)
{
	char text[64] = "";
	int v[6], n = 0;
	const char *p = text;

	if (!PE_parse_boot_arg_str("slider_cal", text, sizeof(text))) {
		return;
	}
	while (n < 6 && *p) {
		int sign = 1, x = 0;

		while (*p == ',' || *p == ';' || *p == ' ') {
			p++;
		}
		if (*p == '-') {
			sign = -1;
			p++;
		}
		if (*p < '0' || *p > '9') {
			break;
		}
		while (*p >= '0' && *p <= '9') {
			x = 10 * x + (*p++ - '0');
		}
		v[n++] = sign * x;
	}
	if (n != 6) {
		say("slider: slider_cal=%s is not six numbers: OxygenOS's rule without calibration is used\n", text);
		return;
	}
	for (int i = 0; i < 3; i++) {
		cal_diff[i] = (int16_t)(v[2 * i + 1] - v[2 * i]);
	}
	calibrated = true;
	say("slider: the factory's calibration: up %d, middle %d, down %d\n", cal_diff[0], cal_diff[1], cal_diff[2]);
}

static uint32_t
decide(int16_t down, int16_t up)
{
	int diff = up - down;

	if (calibrated) {
		static const uint32_t which[3] = { HK_SLIDER_UP, HK_SLIDER_MIDDLE, HK_SLIDER_DOWN };
		int best = -1, best_distance = 23;  /* OxygenOS's tolerances: 15 towards the middle, 22 outwards */

		for (int i = 0; i < 3; i++) {
			int distance = diff > cal_diff[i] ? diff - cal_diff[i] : cal_diff[i] - diff;

			if (distance < best_distance) {
				best = i;
				best_distance = distance;
			}
		}
		return best < 0 ? 0 : which[best];
	}
	return diff > 30 ? HK_SLIDER_UP : diff < -30 ? HK_SLIDER_DOWN : HK_SLIDER_MIDDLE;
}

static void
slider_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const names[] = { "not known", "down: ring", "in the middle: silent", "up: Do Not Disturb" };
	int16_t down = 0, up = 0;
	uint32_t candidate = 0, failures = 0;

	if (!instantnoodle_supply_on("ldoc10", 3000)) {
		say("slider: no supply for the sensors: the alert slider is not read\n");
		thread_terminate(current_thread());
	}
	delay_for_interval(20, NSEC_PER_MSEC);
	if (!sensor_init(ADDR_UP, "up") || !sensor_init(ADDR_DOWN, "down")) {
		say("slider: the alert slider is not read\n");
		thread_terminate(current_thread());
	}
	calibration();
	delay_for_interval(100, NSEC_PER_MSEC);
	for (;;) {
		uint32_t now;

		if (!sensor_read(ADDR_DOWN, &down) || !sensor_read(ADDR_UP, &up)) {
			if (++failures == 10) {
				say("slider: the sensors stopped answering: the alert slider is not read any more\n");
				thread_terminate(current_thread());
			}
			instantnoodle_i2c_recover(&bus);
			delay_for_interval(100, NSEC_PER_MSEC);
			continue;
		}
		failures = 0;
		now = decide(down, up);
		if (now != 0 && now == candidate && now != position) {
			position = now;
			say("slider: %s (down sensor %d, up sensor %d)\n", names[now], down, up);
			hk_touch_post(HK_SLIDER, now, 0, 0);
		}
		candidate = now;
		/* dark: a third as often (a move is still seen in under a second: two readings in a row decide it) */
		delay_for_interval(hk_display_on() ? 100 : 300, NSEC_PER_MSEC);
	}
}

void
instantnoodle_slider_init(void)
{
	uint32_t on = 1;
	thread_t thread;

	PE_parse_boot_argn("slider", &on, sizeof(on));
	if (!instantnoodle_board() || on == 0) {
		return;
	}
	if (kernel_thread_start(slider_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
