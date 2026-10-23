/*
 * The thermal governor (xnu/files/bsd/sys/hk_thermal.h) on the computer: scenarios with
 * temperatures made up tick by tick, and what must come of each. Run: tools/thermal-test/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include "../../xnu/files/bsd/sys/hk_thermal.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static const int TOP[THERMAL_DOMAINS] = { 15, 20, 22 };        /* rows, like the phone's tables */

/* every zone at t (tenths), but the zones of one group at hot */
static void
fill(int16_t *z, int t, int group, int hot)
{
	for (int i = 0; i < THERMAL_ZONES; i++) {
		z[i] = (int16_t)(hk_thermal_zones[i].domain == group ? hot : t);
	}
}

static void
cool_phone_stays_fast(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 450, -9, 0);
	for (int i = 0; i < 100; i++) {
		CHECK(thermal_gov_step(&g, z, 300) == 0, "a cool phone changed something at tick %d", i);
	}
	CHECK(g.cap[0] == 15 && g.cap[1] == 20 && g.cap[2] == 22 && g.level == THERMAL_NOMINAL, "caps or level moved");
}

static void
prime_core_hot_slows_only_it(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 600, 2, 960);                   /* the prime core at 96 C, the rest at 60 */
	unsigned f = thermal_gov_step(&g, z, 300);
	CHECK(f & THERMAL_CAPS_CHANGED, "nothing was slowed");
	CHECK(g.cap[2] == 21 && g.cap[1] == 20 && g.cap[0] == 15, "caps %d %d %d", g.cap[0], g.cap[1], g.cap[2]);
	CHECK(g.level == THERMAL_HEAVY, "level %d", g.level);
	CHECK(hk_thermal_zones[g.hottest].domain == 2, "hottest is %s", hk_thermal_zones[g.hottest].name);
}

static void
much_too_hot_slows_fast(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 600, 1, 1010);                  /* the big cores 6 C past the limit */
	thermal_gov_step(&g, z, 300);
	CHECK(g.cap[1] == 17, "big cores at row %d, want 17 (three rows down)", g.cap[1]);
}

static void
cools_down_slowly_without_seesaw(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 600, 1, 1000);
	for (int i = 0; i < 5; i++) thermal_gov_step(&g, z, 300);       /* down to 20 - 15 = 5 */
	CHECK(g.cap[1] == 5, "after five hot ticks: row %d", g.cap[1]);
	fill(z, 920, -9, 0);                    /* 92 C: under the limit but within the margin: hold */
	for (int i = 0; i < 20; i++) thermal_gov_step(&g, z, 300);
	CHECK(g.cap[1] == 5, "it sped up inside the hysteresis: row %d", g.cap[1]);
	fill(z, 700, -9, 0);                    /* 70 C: back up, one row a second */
	for (int i = 0; i < 4 * 3; i++) thermal_gov_step(&g, z, 300);
	CHECK(g.cap[1] == 8, "three seconds of cool: row %d, want 8", g.cap[1]);
	for (int i = 0; i < 4 * 30; i++) thermal_gov_step(&g, z, 300);
	CHECK(g.cap[1] == 20 && g.level == THERMAL_NOMINAL, "never got back to the top: row %d, level %d", g.cap[1], g.level);
}

static void
gpu_heat_slows_every_group(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 600, THERMAL_NO_CPU, 970);      /* the GPU, DSPs, modem area at 97 C */
	thermal_gov_step(&g, z, 300);
	CHECK(g.cap[0] == 14 && g.cap[1] == 19 && g.cap[2] == 21, "package heat: caps %d %d %d", g.cap[0], g.cap[1], g.cap[2]);
}

static void
critical_all_lowest_then_emergency(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	unsigned f = 0;
	thermal_gov_init(&g, TOP);
	fill(z, 700, 0, 1160);
	for (int i = 0; i < THERMAL_EMERGENCY_TICKS - 1; i++) {
		f = thermal_gov_step(&g, z, 300);
		CHECK(!(f & THERMAL_EMERGENCY), "emergency too early, tick %d", i);
	}
	CHECK(g.cap[0] == 0 && g.cap[1] == 0 && g.cap[2] == 0 && g.level == THERMAL_SLEEPING, "not all at the lowest");
	f = thermal_gov_step(&g, z, 300);
	CHECK(f & THERMAL_EMERGENCY, "no emergency after two seconds past the stop");
}

static void
critical_blip_is_no_emergency(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	for (int i = 0; i < 40; i++) {
		fill(z, 700, 0, i % 4 == 0 ? 1160 : 800);       /* a spike every second */
		CHECK(!(thermal_gov_step(&g, z, 300) & THERMAL_EMERGENCY), "a one-tick spike ended the run");
	}
}

static void
near_critical_is_trapping(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 600, 2, 1110);
	thermal_gov_step(&g, z, 300);
	CHECK(g.level == THERMAL_TRAPPING, "111 C: level %s", thermal_level_name(g.level));
}

static void
blind_sensors_hold_half_speed(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	unsigned f = 0;
	thermal_gov_init(&g, TOP);
	for (int i = 0; i < THERMAL_ZONES; i++) z[i] = THERMAL_INVALID;
	for (int i = 0; i < THERMAL_BLIND_TICKS; i++) f = thermal_gov_step(&g, z, -2730);
	CHECK(f & THERMAL_BLIND, "not blind after %d ticks", THERMAL_BLIND_TICKS);
	CHECK(g.cap[0] == 7 && g.cap[1] == 10 && g.cap[2] == 11, "blind caps %d %d %d", g.cap[0], g.cap[1], g.cap[2]);
	fill(z, 500, -9, 0);
	thermal_gov_step(&g, z, 300);
	CHECK(!(thermal_gov_step(&g, z, 300) & THERMAL_BLIND), "still blind with readings back");
}

static void
hot_battery(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	thermal_gov_init(&g, TOP);
	fill(z, 500, -9, 0);
	thermal_gov_step(&g, z, 460);
	CHECK(g.level == THERMAL_MODERATE && g.cap[2] == 22, "46 C battery: level %d cap %d", g.level, g.cap[2]);
	thermal_gov_step(&g, z, 520);
	CHECK(g.level == THERMAL_HEAVY && g.cap[2] == 11 && g.cap[0] == 7, "52 C battery: level %d caps %d %d", g.level, g.cap[0], g.cap[2]);
	thermal_gov_step(&g, z, 600);
	CHECK(g.level == THERMAL_TRAPPING && g.cap[2] == 0, "60 C battery: level %d cap %d", g.level, g.cap[2]);
}

static void
group_without_our_cores_is_left_alone(void)
{
	struct thermal_gov g; int16_t z[THERMAL_ZONES];
	const int top[THERMAL_DOMAINS] = { 15, -1, -1 };        /* cpus=1: only a small core */
	thermal_gov_init(&g, top);
	fill(z, 600, -9, 0);
	z[13] = 1000;                           /* the prime core's sensor hot: not ours, but heat is heat */
	thermal_gov_step(&g, z, 300);
	CHECK(g.cap[1] == -1 && g.cap[2] == -1, "a group that is not ours got a cap");
	CHECK(g.cap[0] == 15, "the small cores were slowed for another group's sensor: row %d", g.cap[0]);
}

int
main(void)
{
	cool_phone_stays_fast();
	prime_core_hot_slows_only_it();
	much_too_hot_slows_fast();
	cools_down_slowly_without_seesaw();
	gpu_heat_slows_every_group();
	critical_all_lowest_then_emergency();
	critical_blip_is_no_emergency();
	near_critical_is_trapping();
	blind_sensors_hold_half_speed();
	hot_battery();
	group_without_our_cores_is_left_alone();
	printf("thermal-test: %s (%d zones, 11 scenarios)\n", failures ? "FAILED" : "all good", THERMAL_ZONES);
	return failures != 0;
}
