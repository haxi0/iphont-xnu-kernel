/*
 * The thermal governor of the POCO X5 Pro (redwood): which of the SoC's temperature sensors there
 * are, where each sits, how warm it may get, and the decision made every tick -- how fast each group
 * of processor cores may run, and how hot iOS should be told the phone is.
 *
 * Only the decision is here, as inline functions with no kernel in them: bsd/dev/hk_thermal.c
 * reads the sensors and carries the decision out, and tools/thermal-test compiles this same header
 * on the computer and drives it through heat-ups, cool-downs, failing sensors and a hot battery.
 *
 * The sensors: the SoC has two TSENS blocks (dt: tsens@c222000 with its readings at 0xc263000, 15
 * sensors; tsens@c223000 at 0xc265000, 12 sensors). Which sensor is where, and the limits, come from
 * the phone's own device tree (its thermal-zones: the "-step" zones are where Xiaomi's kernel starts
 * to slow the cores, the "-usr" zones where it shuts the phone down):
 *
 *   tsens0  0 aoss-0      1..4 cpu-0-0..3 (the small cores)     5, 6 cpuss-0, cpuss-1 (shared)
 *           7..12 cpu-1-0..5 (the big cores, two sensors each)  13, 14 cpu-1-6, cpu-1-7 (the prime core)
 *   tsens1  0 aoss-1      1, 2 gpuss-0, gpuss-1     3, 4 nspss    5 video    6 ddr    7..10 mdmss
 *           11 camera-0
 *
 * Xiaomi's limits are 108..112 C to start slowing the cores and 115 C to shut down. Ours are lower on
 * purpose: this kernel runs every core it uses at the top of its frequency table all the time
 * (cpufreq=1), with no other governor -- so slowing starts at 95 C, well before the hardware's own
 * protection would have to act, and 115 C is the emergency stop as in Xiaomi's.
 *
 * Temperatures are kept in tenths of a degree Celsius, as TSENS reports them.
 */
#ifndef _SYS_HK_THERMAL_H_
#define _SYS_HK_THERMAL_H_

#include <stdbool.h>
#include <stdint.h>

#define THERMAL_DOMAINS         3               /* the cores' frequency groups: small, big, prime (hk_cpufreq.c) */
#define THERMAL_ALL_CPUS        (-2)            /* a zone shared by every group of cores */
#define THERMAL_NO_CPU          (-1)            /* a zone of something else: the GPU, the DSPs, the memory */
#define THERMAL_INVALID         (-32768)        /* no reading from that sensor this tick */

/* iOS's own levels (OSThermalPressureLevel, <libkern/OSThermalNotification.h>): what thermald posts */
#define THERMAL_NOMINAL         0               /* nothing to worry about */
#define THERMAL_MODERATE        1               /* within 10 C of slowing down */
#define THERMAL_HEAVY           2               /* the cores are being slowed */
#define THERMAL_TRAPPING        3               /* within 5 C of the emergency stop */
#define THERMAL_SLEEPING        4               /* past it: everything at the lowest speed */

#define THERMAL_TICK_MS         250             /* how often the sensors are read */
#define THERMAL_HOT_STEP        50              /* 5 C past the limit: slow down three rows a tick, not one */
#define THERMAL_COOL_MARGIN     50              /* 5 C under the limit before speeding up again */
#define THERMAL_COOL_TICKS      4               /* ... and for this many ticks in a row (one second) */
#define THERMAL_EMERGENCY_TICKS 8               /* two seconds past the stop: the run ends (a panic) */
#define THERMAL_BLIND_TICKS     20              /* five seconds with no CPU reading: slow to half and say so */

#define THERMAL_BATTERY_WARM    450             /* the battery's temperature: 45 C, at least MODERATE */
#define THERMAL_BATTERY_HOT     500             /* 50 C: the cores held at most at half speed, HEAVY */
#define THERMAL_BATTERY_DANGER  580             /* 58 C: TRAPPING, the cores at the lowest speed */

struct thermal_zone {
	const char *name;                       /* as the device tree calls it */
	uint8_t block, sensor;                  /* TSENS block 0 or 1, sensor number in it */
	int8_t domain;                          /* 0..2, THERMAL_ALL_CPUS or THERMAL_NO_CPU */
	int16_t passive;                        /* start slowing at this, tenths of C */
	int16_t critical;                       /* the emergency stop */
};

static const struct thermal_zone hk_thermal_zones[] = {
	{ "aoss-0", 0, 0, THERMAL_NO_CPU, 1000, 1150 },
	{ "cpu-0-0", 0, 1, 0, 950, 1150 },
	{ "cpu-0-1", 0, 2, 0, 950, 1150 },
	{ "cpu-0-2", 0, 3, 0, 950, 1150 },
	{ "cpu-0-3", 0, 4, 0, 950, 1150 },
	{ "cpuss-0", 0, 5, THERMAL_ALL_CPUS, 950, 1150 },
	{ "cpuss-1", 0, 6, THERMAL_ALL_CPUS, 950, 1150 },
	{ "cpu-1-0", 0, 7, 1, 950, 1150 },
	{ "cpu-1-1", 0, 8, 1, 950, 1150 },
	{ "cpu-1-2", 0, 9, 1, 950, 1150 },
	{ "cpu-1-3", 0, 10, 1, 950, 1150 },
	{ "cpu-1-4", 0, 11, 1, 950, 1150 },
	{ "cpu-1-5", 0, 12, 1, 950, 1150 },
	{ "cpu-1-6", 0, 13, 2, 950, 1150 },
	{ "cpu-1-7", 0, 14, 2, 950, 1150 },
	{ "aoss-1", 1, 0, THERMAL_NO_CPU, 1000, 1150 },
	{ "gpuss-0", 1, 1, THERMAL_NO_CPU, 950, 1150 },
	{ "gpuss-1", 1, 2, THERMAL_NO_CPU, 950, 1150 },
	{ "nspss-0", 1, 3, THERMAL_NO_CPU, 1000, 1150 },
	{ "nspss-1", 1, 4, THERMAL_NO_CPU, 1000, 1150 },
	{ "video", 1, 5, THERMAL_NO_CPU, 1000, 1150 },
	{ "ddr", 1, 6, THERMAL_NO_CPU, 1000, 1150 },
	{ "mdmss-0", 1, 7, THERMAL_NO_CPU, 950, 1150 },
	{ "mdmss-1", 1, 8, THERMAL_NO_CPU, 950, 1150 },
	{ "mdmss-2", 1, 9, THERMAL_NO_CPU, 950, 1150 },
	{ "mdmss-3", 1, 10, THERMAL_NO_CPU, 950, 1150 },
	{ "camera-0", 1, 11, THERMAL_NO_CPU, 1000, 1150 },
};
#define THERMAL_ZONES   ((int)(sizeof(hk_thermal_zones) / sizeof(hk_thermal_zones[0])))

/* What the governor keeps from one tick to the next. */
struct thermal_gov {
	int top[THERMAL_DOMAINS];               /* the highest row of each group's frequency table; -1: not ours */
	int cap[THERMAL_DOMAINS];               /* the row each group may run at now */
	int cool[THERMAL_DOMAINS];              /* ticks in a row it has been cool enough to speed up */
	int level;                              /* THERMAL_NOMINAL..THERMAL_SLEEPING */
	int hottest;                            /* the zone furthest past (or nearest to) its limit; -1 none */
	int16_t hottest_temp;
	int hot_ticks;                          /* ticks in a row past a critical limit */
	int blind_ticks;                        /* ticks in a row with no reading of any CPU zone */
	uint32_t throttles;                     /* times a group was slowed, over the run */
};

/* What one tick decided, besides the caps. */
#define THERMAL_CAPS_CHANGED    (1u << 0)       /* write the caps to the hardware */
#define THERMAL_LEVEL_CHANGED   (1u << 1)       /* tell iOS */
#define THERMAL_EMERGENCY       (1u << 2)       /* past the stop for too long: end the run now */
#define THERMAL_BLIND           (1u << 3)       /* no CPU sensor reads: held at half speed */

static inline void
thermal_gov_init(struct thermal_gov *g, const int top[THERMAL_DOMAINS])
{
	*g = (struct thermal_gov){ .level = THERMAL_NOMINAL, .hottest = -1 };
	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		g->top[d] = top[d];
		g->cap[d] = top[d];
	}
}

static inline int
thermal_clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * One tick: `tenths` holds a reading per zone of hk_thermal_zones (THERMAL_INVALID where there was
 * none), `battery` the battery's temperature in tenths (below -2000: not known). Changes the caps and
 * the level in `g`; returns THERMAL_* flags.
 */
static inline unsigned
thermal_gov_step(struct thermal_gov *g, const int16_t *tenths, int battery)
{
	int excess[THERMAL_DOMAINS], package = -10000, worst = -10000, worst_zone = -1, any_cpu = 0, critical = 0;
	int old_cap[THERMAL_DOMAINS], old_level = g->level, level;
	unsigned flags = 0;

	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		excess[d] = -10000;
		old_cap[d] = g->cap[d];
	}
	/* how far past its limit each zone is (negative: under it), and the worst of them */
	for (int z = 0; z < THERMAL_ZONES; z++) {
		const struct thermal_zone *zone = &hk_thermal_zones[z];
		int t = tenths[z], over;

		if (t == THERMAL_INVALID) {
			continue;
		}
		over = t - zone->passive;
		if (zone->domain >= 0) {
			excess[zone->domain] = over > excess[zone->domain] ? over : excess[zone->domain];
			any_cpu = 1;
		} else {
			/* the shared CPU zones and everything else heat the whole package: they count for every group */
			package = over > package ? over : package;
			any_cpu |= zone->domain == THERMAL_ALL_CPUS;
		}
		if (t >= zone->critical) {
			critical = 1;
		}
		if (over > worst) {
			worst = over;
			worst_zone = z;
		}
	}
	g->hottest = worst_zone;
	g->hottest_temp = worst_zone >= 0 ? tenths[worst_zone] : THERMAL_INVALID;

	/* no reading of any CPU zone for a while: a sensor block that has stopped is no reason to run hot */
	g->blind_ticks = any_cpu ? 0 : g->blind_ticks + 1;

	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		int e = excess[d] > package ? excess[d] : package;

		if (g->top[d] < 0) {
			continue;                       /* a group with none of our cores in it */
		}
		if (e >= THERMAL_HOT_STEP) {
			g->cap[d] -= 3;
			g->cool[d] = 0;
		} else if (e >= 0) {
			g->cap[d] -= 1;
			g->cool[d] = 0;
		} else if (e < -THERMAL_COOL_MARGIN) {
			if (++g->cool[d] >= THERMAL_COOL_TICKS && g->cap[d] < g->top[d]) {
				g->cap[d] += 1;         /* one row a second back up: no see-saw */
				g->cool[d] = 0;
			}
		} else {
			g->cool[d] = 0;                 /* between the two: hold where it is */
		}
		if (battery >= THERMAL_BATTERY_DANGER) {
			g->cap[d] = 0;
		} else if (battery >= THERMAL_BATTERY_HOT && g->cap[d] > g->top[d] / 2) {
			g->cap[d] = g->top[d] / 2;
		}
		if (g->blind_ticks >= THERMAL_BLIND_TICKS && g->cap[d] > g->top[d] / 2) {
			g->cap[d] = g->top[d] / 2;
		}
		if (critical) {
			g->cap[d] = 0;
		}
		g->cap[d] = thermal_clamp(g->cap[d], 0, g->top[d]);
		if (g->cap[d] < old_cap[d]) {
			g->throttles++;
		}
		if (g->cap[d] != old_cap[d]) {
			flags |= THERMAL_CAPS_CHANGED;
		}
	}

	/* the level iOS is told */
	if (critical) {
		level = THERMAL_SLEEPING;
	} else if (worst_zone >= 0 && g->hottest_temp >= hk_thermal_zones[worst_zone].critical - 50) {
		level = THERMAL_TRAPPING;
	} else {
		int slowed = 0;
		for (int d = 0; d < THERMAL_DOMAINS; d++) {
			slowed |= g->top[d] >= 0 && g->cap[d] < g->top[d];
		}
		level = slowed ? THERMAL_HEAVY : worst >= -100 ? THERMAL_MODERATE : THERMAL_NOMINAL;
	}
	if (battery >= THERMAL_BATTERY_DANGER && level < THERMAL_TRAPPING) {
		level = THERMAL_TRAPPING;
	} else if (battery >= THERMAL_BATTERY_HOT && level < THERMAL_HEAVY) {
		level = THERMAL_HEAVY;
	} else if (battery >= THERMAL_BATTERY_WARM && level < THERMAL_MODERATE) {
		level = THERMAL_MODERATE;
	}
	g->level = level;
	if (level != old_level) {
		flags |= THERMAL_LEVEL_CHANGED;
	}

	g->hot_ticks = critical ? g->hot_ticks + 1 : 0;
	if (g->hot_ticks >= THERMAL_EMERGENCY_TICKS) {
		flags |= THERMAL_EMERGENCY;
	}
	if (g->blind_ticks >= THERMAL_BLIND_TICKS) {
		flags |= THERMAL_BLIND;
	}
	return flags;
}

static inline const char *
thermal_level_name(int level)
{
	static const char *const names[] = { "nominal", "moderate", "heavy", "trapping", "sleeping" };
	return level >= 0 && level <= THERMAL_SLEEPING ? names[level] : "?";
}

#endif /* _SYS_HK_THERMAL_H_ */
