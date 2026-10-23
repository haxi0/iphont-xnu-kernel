/*
 * The thermal service of the POCO X5 Pro (redwood): every quarter of a second the SoC's 27 temperature
 * sensors are read, the governor of sys/hk_thermal.h decides how fast each group of processor
 * cores may run and how hot iOS should be told the phone is, and that is carried out.
 *
 * Why there has to be one: with cpufreq=1 this kernel puts every group of cores it uses at the top of
 * its frequency table (bsd/dev/hk_cpufreq.c) and nothing ever lowers it again -- Android's thermal
 * engine, which would, is not here. A long build in a terminal or a game would then run the cores
 * into the hardware's last-resort protection. This service is the missing governor:
 *
 *   - the cores are slowed a row of the frequency table at a time (three when far too hot) as soon as
 *     a sensor near them passes its limit (95 C for the cores, 100 C for the rest), and sped up again
 *     a row a second once everything is 5 C under it -- no see-saw;
 *   - heat from the GPU, the DSPs, the memory or the modem area slows every group, it is one package;
 *   - a hot battery (from bsd/dev/hk_battery.c) holds the cores at half speed from 50 C, at the
 *     lowest from 58 C;
 *   - past 115 C everything goes to the lowest row at once, and after two seconds there the run ends
 *     with a panic (which returns the phone to the loader and saves the log): the last line of defence
 *     before the hardware's own;
 *   - if the sensors stop answering for five seconds the cores are held at half speed: a sensor that
 *     cannot be read is no reason to run hot.
 *
 * What it tells:
 *   kern.hackios.thermal              a table: every sensor's reading and limit, each group's speed
 *   kern.hackios.thermal_pressure     iOS's pressure level 0..4 (nominal .. sleeping), for thermald in
 *                                     the userland, which posts it as com.apple.system.thermalpressurelevel
 *   kern.hackios.thermal_hottest      the hottest reading, in tenths of a degree
 *   kern.hackios.thermal_throttles    how many times a group was slowed over the run
 *
 * On by default whenever cpufreq=1 has raised a group (that is when it is needed); thermal=0 turns it
 * off, thermal=2 turns it on to watch and report without cpufreq (no caps to set then). Only reads the
 * sensors -- TSENS is never written, its own thresholds stay as the firmware set them.
 *
 * The sensors' register layout: Linux drivers/thermal/qcom/tsens-v2.c (as bsd/dev/hk_adreno.c
 * already reads the GPU's two): a status word per sensor at 0xa0 + 4 n, the last reading in its low 12
 * bits, signed, in tenths of a degree, bit 21 set when the reading is valid.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hk_input.h>
#include <sys/hk_thermal.h>
#include <sys/sysctl.h>
#include <sys/hk_board.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

#define TSENS0_TM               hk_board_addr("tsens0", 0x0c263000UL)    /* dt: tsens@c222000, "tsens_tm_physical" */
#define TSENS1_TM               hk_board_addr("tsens1", 0x0c265000UL)    /* dt: tsens@c223000 */
#define TSENS_TM_SIZE           0x1000UL
#define TSENS_STATUS(sensor)    (0xa0 + 4 * (sensor))
#define TSENS_VALID             (1u << 21)
#define TSENS_TRIES             4               /* a sensor caught between two conversions is asked again */

static vm_offset_t tsens[2];
static struct thermal_gov gov;
static int16_t readings[THERMAL_ZONES];
/*
 * Where each zone's sensor is: the table's, unless board_thermal_<zone>=0xBSS says otherwise (B the TSENS
 * block, SS the sensor; 0xffff: none), with '_' for the '-' of the zone's name. A phone whose SoC numbers its
 * sensors otherwise passes them so (the OnePlus 8, SM8250: gpuss-0 is sensor 15 of block 0, gpuss-1 sensor 8
 * of block 1 -- its device tree's thermal-zones -- where the POCO X5 Pro has them at 1 and 2 of block 1).
 */
static uint16_t zone_where[THERMAL_ZONES];
static bool thermal_caps_on;                    /* cpufreq raised a group: there are caps to set */

/* what the sysctls show, kept by the service's thread and read by anyone */
static LCK_GRP_DECLARE(thermal_lck_grp, "hk-thermal");
static LCK_MTX_DECLARE(thermal_mtx, &thermal_lck_grp);
static char thermal_text[3072];
static int thermal_pressure, thermal_hottest = THERMAL_INVALID, thermal_throttles, thermal_running;

SYSCTL_DECL(_kern_hackios);
SYSCTL_INT(_kern_hackios, OID_AUTO, thermal_pressure, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_pressure, 0,
    "iOS thermal pressure level: 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping");
SYSCTL_INT(_kern_hackios, OID_AUTO, thermal_hottest, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_hottest, 0,
    "the hottest sensor's reading, tenths of a degree Celsius");
SYSCTL_INT(_kern_hackios, OID_AUTO, thermal_throttles, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_throttles, 0,
    "how many times a group of cores was slowed down");
SYSCTL_INT(_kern_hackios, OID_AUTO, thermal_running, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_running, 0,
    "1 while the thermal service runs");

static int
thermal_sysctl_text(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static char copy[sizeof(thermal_text)];

	lck_mtx_lock(&thermal_mtx);
	strlcpy(copy, thermal_running ? thermal_text : "thermal: not running (thermal=0, or no cpufreq=1)\n", sizeof(copy));
	lck_mtx_unlock(&thermal_mtx);
	return SYSCTL_OUT(req, copy, strlen(copy) + 1);
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, thermal, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    thermal_sysctl_text, "A", "every temperature sensor and every group of cores, as a table");

/* the kernel's log and the loader's, like the other drivers of the phone's bring-up */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

/* "95.3" from 953; "-" for no reading */
static const char *
tenths_text(int t, char *buf, size_t size)
{
	if (t == THERMAL_INVALID) {
		strlcpy(buf, "-", size);
	} else {
		snprintf(buf, size, "%s%d.%d", t < 0 ? "-" : "", (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
	}
	return buf;
}

/* one sensor's reading, THERMAL_INVALID if it has none after a few tries */
static int16_t
read_sensor(unsigned block, unsigned sensor)
{
	for (int i = 0; i < TSENS_TRIES; i++) {
		uint32_t status = *(volatile uint32_t *)(tsens[block] + TSENS_STATUS(sensor));

		if (status & TSENS_VALID) {
			return (int16_t)((int32_t)(status << 20) >> 20);
		}
	}
	return THERMAL_INVALID;
}

static int16_t
read_zone(int z)
{
	return zone_where[z] == 0xffff ? THERMAL_INVALID : read_sensor(zone_where[z] >> 8 & 1, zone_where[z] & 0xff);
}

/* The GPU's sensors at the last tick (the hotter of the gpuss zones), tenths of a degree; THERMAL_INVALID: none. */
int
hk_thermal_gpu_tenths(void)
{
	int hottest = THERMAL_INVALID;

	for (int z = 0; thermal_running && z < THERMAL_ZONES; z++) {
		if (strncmp(hk_thermal_zones[z].name, "gpuss-", 6) == 0 && readings[z] != THERMAL_INVALID && readings[z] > hottest) {
			hottest = readings[z];
		}
	}
	return hottest;
}

/* the table of kern.hackios.thermal, from this tick's readings and decisions */
static void
write_text(int battery)
{
	static char text[sizeof(thermal_text)];
	size_t n = 0;
	char a[16], b[16], c[16];

	n += snprintf(text + n, sizeof(text) - n, "pressure %s (%d), hottest %s %s C, battery %s C, slowed %d times\n",
	    thermal_level_name(gov.level), gov.level, gov.hottest >= 0 ? hk_thermal_zones[gov.hottest].name : "-",
	    tenths_text(gov.hottest_temp, a, sizeof(a)), tenths_text(battery < -2000 ? THERMAL_INVALID : battery, b, sizeof(b)),
	    (int)gov.throttles);
	for (int d = 0; d < THERMAL_DOMAINS && n < sizeof(text); d++) {
		static const char *const names[THERMAL_DOMAINS] = { "small cores", "big cores", "prime core" };

		if (gov.top[d] < 0) {
			n += snprintf(text + n, sizeof(text) - n, "%-12s not ours\n", names[d]);
		} else {
			n += snprintf(text + n, sizeof(text) - n, "%-12s row %d of %d, %u MHz of %u\n", names[d], gov.cap[d],
			    gov.top[d], hk_cpufreq_khz(d, gov.cap[d]) / 1000, hk_cpufreq_khz(d, gov.top[d]) / 1000);
		}
	}
	for (int z = 0; z < THERMAL_ZONES && n < sizeof(text); z++) {
		const struct thermal_zone *zone = &hk_thermal_zones[z];

		n += snprintf(text + n, sizeof(text) - n, "%-9s tsens%u/%-2u %6s C   limit %s, stop %s\n", zone->name, zone->block,
		    zone->sensor, tenths_text(readings[z], a, sizeof(a)), tenths_text(zone->passive, b, sizeof(b)),
		    tenths_text(zone->critical, c, sizeof(c)));
	}
	lck_mtx_lock(&thermal_mtx);
	memcpy(thermal_text, text, sizeof(thermal_text));
	thermal_text[sizeof(thermal_text) - 1] = 0;
	thermal_pressure = gov.level;
	thermal_hottest = gov.hottest_temp;
	thermal_throttles = (int)gov.throttles;
	lck_mtx_unlock(&thermal_mtx);
}

static void
thermal_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t ticks = 0;
	bool was_blind = false;

	for (;;) {
		int battery = hk_battery_temperature();
		unsigned flags;
		char a[16];

		battery = battery <= -273 ? -2730 : battery * 10;
		for (int z = 0; z < THERMAL_ZONES; z++) {
			readings[z] = read_zone(z);
		}
		flags = thermal_gov_step(&gov, readings, battery);

		if ((flags & THERMAL_CAPS_CHANGED) && thermal_caps_on) {
			for (int d = 0; d < THERMAL_DOMAINS; d++) {
				hk_cpufreq_cap(d, gov.cap[d]);
			}
		}
		if (flags & (THERMAL_CAPS_CHANGED | THERMAL_LEVEL_CHANGED)) {
			tell("thermal: %s, hottest %s at %s C; cores at rows %d/%d %d/%d %d/%d\n", thermal_level_name(gov.level),
			    gov.hottest >= 0 ? hk_thermal_zones[gov.hottest].name : "-", tenths_text(gov.hottest_temp, a, sizeof(a)),
			    gov.cap[0], gov.top[0], gov.cap[1], gov.top[1], gov.cap[2], gov.top[2]);
		}
		if ((flags & THERMAL_BLIND) && !was_blind) {
			tell("thermal: no CPU sensor has answered for %d ms: the cores are held at half speed\n",
			    THERMAL_BLIND_TICKS * THERMAL_TICK_MS);
		}
		was_blind = (flags & THERMAL_BLIND) != 0;
		write_text(battery);
		if (flags & THERMAL_EMERGENCY) {
			panic("thermal: %s at %s C for %d ms with every core at its lowest speed: the run ends here to protect the phone",
			    hk_thermal_zones[gov.hottest].name, tenths_text(gov.hottest_temp, a, sizeof(a)),
			    THERMAL_EMERGENCY_TICKS * THERMAL_TICK_MS);
		}
		if (++ticks % (60000 / THERMAL_TICK_MS) == 0) {
			tell("thermal: a minute: %s, hottest %s at %s C, slowed %u times so far\n", thermal_level_name(gov.level),
			    gov.hottest >= 0 ? hk_thermal_zones[gov.hottest].name : "-", tenths_text(gov.hottest_temp, a, sizeof(a)),
			    gov.throttles);
		}
		delay_for_interval(THERMAL_TICK_MS, NSEC_PER_MSEC);
	}
}

void
hk_thermal_init(void)
{
	uint32_t mode = 1;
	int top[THERMAL_DOMAINS], raised = 0;
	thread_t thread;
	char a[16];

	PE_parse_boot_argn("thermal", &mode, sizeof(mode));
	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		top[d] = hk_cpufreq_top(d);
		raised |= top[d] >= 0;
	}
	/* on the phone only: QEMU has no TSENS, and without cpufreq=1 the firmware's speeds are modest */
	if (mode == 0 || (mode == 1 && !raised)) {
		return;
	}
	thermal_caps_on = raised;
	tsens[0] = ml_io_map(TSENS0_TM, TSENS_TM_SIZE);
	tsens[1] = ml_io_map(TSENS1_TM, TSENS_TM_SIZE);
	thermal_gov_init(&gov, top);
	for (int z = 0; z < THERMAL_ZONES; z++) {
		char name[40] = "thermal_";
		uint16_t table = (uint16_t)(hk_thermal_zones[z].block << 8 | hk_thermal_zones[z].sensor);

		strlcat(name, hk_thermal_zones[z].name, sizeof(name));
		for (char *c = name; *c != 0; c++) {
			*c = *c == '-' ? '_' : *c;
		}
		zone_where[z] = (uint16_t)hk_board_number(name, table);
		if (zone_where[z] != table) {
			tell("thermal: %s is sensor %u of block %u here (board_%s)\n", hk_thermal_zones[z].name, zone_where[z] & 0xff,
			    zone_where[z] >> 8, name);
		}
		readings[z] = read_zone(z);
	}
	/* one reading of the prime core's sensor to the log: whether TSENS answers at all is the first thing to know */
	tell("thermal: %d sensors; the prime core reads %s C; caps %s; every %d ms\n", THERMAL_ZONES,
	    tenths_text(readings[13], a, sizeof(a)), thermal_caps_on ? "on" : "off (thermal=2: watching only)", THERMAL_TICK_MS);
	if (kernel_thread_start(thermal_thread, NULL, &thread) != KERN_SUCCESS) {
		tell("thermal: no thread: the cores are NOT watched\n");
		return;
	}
	thread_deallocate(thread);
	thermal_running = 1;
}
