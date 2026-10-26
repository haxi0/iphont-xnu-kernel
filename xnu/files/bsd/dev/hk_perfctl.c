/*
 * The performance controller of the Edge scheduler on the phones: what Apple's CLPC (closed, part of every AMP
 * iPhone's kernelcache) does for XNU, rebuilt on the interface XNU gives it (osfmk/arm/machine_routines.h,
 * sched_perfcontrol_*), for the Snapdragons' clusters (osfmk/arm64/machine_routines.c: a cluster is an EPSS
 * frequency domain; the Cortex-A55s are E, the A77/A78s P).
 *
 * The Edge scheduler (osfmk/kern/sched_clutch.c, doc/sched_clutch_edge.md) places a thread on the preferred cluster
 * of its thread group's QoS bucket, spills it along the edges of a migration graph when that cluster's expected
 * latency is too high, steals and rebalances between clusters when one runs dry. It decides nothing about which
 * cluster a group should prefer, nor about the graph's weights, nor about clock speeds: that is the controller's.
 * Without one every group prefers the boot cluster (here the single prime core) and the E cores are an overflow.
 *
 * What this controller does, every PC_TICK_MS (PC_TICK_DARK_MS with the screen off):
 *
 *  1. Accounting, from the scheduler's callouts (interrupts off, a few atomics each): every context switch and
 *     quantum expiry charges the time a thread ran to its thread group, by class (FG: above-UI, UI, real-time
 *     and above-UI kernel threads; DF: default/legacy; UT; BG), weighted by the capacity of the core it ran on at
 *     the speed it ran at -- so a group's demand is in one unit wherever it ran: PC_REF is the fastest core at its
 *     top speed. Also the cores it used (its width), its moves between clusters (with the thread's own last
 *     cluster kept in its perfcontrol state), scheduling latencies by class and cluster (on-core callout), frames
 *     and missed deadlines of its work intervals (CoreAnimation's), and its threads waiting on the render server.
 *  2. Per thread group, a recommendation, held with hysteresis (a level: E, P, PRIME):
 *     - interactive work (screen lit; UI-class demand, CA frames, the render server while something waits on
 *       it) is put on P at once, and kept there PC_INTERACTIVE_HOLD after its last sign;
 *     - other work goes by demand: to P only when it would saturate the E cluster for PC_SUSTAIN ticks
 *       (one thread busier than PC_E_UP of an E core, or the group more than PC_E_UP of the E cluster),
 *       back to E when it fits again (PC_E_DOWN) for as long;
 *     - utility and background work prefers E (as Apple's sched_edge_restrict_ut/bg), whatever its group does;
 *     - the busiest single-threaded interactive group(s) (one per prime core) get the prime cluster, the one
 *       whose frames miss first; the choice changes at most every PC_PRIME_DWELL and only for a clearly busier one;
 *     - THREAD_GROUP_FLAGS_EFFICIENT groups stay on E.
 *     Moving up migrates the group's running and runnable threads at once; moving down lets them drain.
 *  3. The migration graph: P<->P free (weight 0, one core type); P->E allowed with PC_W_PE (less with the screen
 *     off, more when threads ping-pong between the core types); E->P opened only while the E cluster is
 *     saturated (its expected latency at default QoS over PC_E_SAT_US for two ticks; or the scheduler reports
 *     starved runnable threads), closed again after PC_SUSTAIN calm ticks -- Apple's "open clusters rapidly when
 *     the work can use them, migrate down otherwise".
 *  4. Clock speeds (bsd/dev/hk_cpufreq.c's governor keeps following each group's busiest core): a floor per cluster
 *     for its interactive groups' per-thread demand with 25% headroom, the top speed for 100 ms after a missed
 *     frame (raised at once), and a touch raises only the clusters interactive work runs on.
 *
 * The cores' capacities are measured, not assumed: once every core is up, a kernel thread bound to each cluster
 * reads the core's MIDR_EL1 and L2 size and runs a short integer workload at the top speed. Until then they are
 * estimated from the top speeds.
 *
 * Boot-args: perfctl=0 (the Edge scheduler with its static defaults and no controller), perfctl_log=1 (each
 * recommendation change in the log). kern.hackios.amp: the clusters, the graph, the scheduler's and the
 * controller's counts, latency histograms and the busiest groups (tools/sched-test/amp.py reads it).
 */
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <kern/thread_group.h>
#include <mach/thread_policy.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hk_input.h>
#include <sys/hk_board.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/work_interval.h>

#if CONFIG_SCHED_EDGE && HK_SCHED_SELECTABLE

extern uint32_t ml_cpu_frequency_domain(int cpu_id);
extern bool sched_edge_active(void);
extern bool ml_cpu_is_small_core(int cpu_id);    /* (osfmk/kern/sched_prim.c: the scheduler chosen at boot is Edge) */
#define PC_BUCKET_DF            3       /* TH_BUCKET_SHARE_DF (osfmk/kern/sched.h): default QoS */
#define PC_URGENCY_NORMAL       2       /* THREAD_URGENCY_NORMAL (osfmk/kern/sched_urgency.h, not exported to BSD) */
#define PC_URGENCY_REAL_TIME    3       /* THREAD_URGENCY_REAL_TIME */
extern bool ml_cpu_online(int cpu_id);
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern uint32_t processor_avail_count;
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void loader_log_line(const char *line);

#define PC_REF                  1024    /* capacity of the fastest core at its top speed */
#define PC_TICK_MS              20
#define PC_TICK_DARK_MS         250
#define PC_SUSTAIN              25      /* ticks (0.5 s lit) a fall in demand must last to move non-interactive work down to E */
#define PC_SUSTAIN_UP           5       /* ... and a rise to move it up (0.1 s: a CPU-bound program is not kept waiting) */
#define PC_INTERACTIVE_HOLD_MS  2000    /* interactive work stays on P this long after its last sign */
#define PC_RECENT_MS            30000   /* ... and is put back at once by a touch for this long */
#define PC_PRIME_DWELL_MS       100
#define PC_E_UP                 90      /* % of an E core (one thread) or of the E cluster (the group): too much for E */
#define PC_E_DOWN               60      /* % under which it fits on E again */
#define PC_PRIME_UP             50      /* % of a P core one thread of an interactive group keeps busy: worth the prime core */
#define PC_FG_SIGN              8       /* /PC_REF of UI-class demand: a sign of interactive work */
#define PC_DEADLINE_E           300     /* per mille of its period a frame may take at E's top speed and stay on E */
#define PC_DEADLINE_PRIME       600     /* ... over this at the P cluster's top speed: the prime cluster */
#define PC_DEADLINE_TARGET      700     /* the clock floor makes a frame take this much of its period */
#define PC_E_SAT_US             2000    /* expected latency at default QoS on E (us) that opens E->P */
#define PC_W_PE                 64      /* P->E migration weight (us of latency difference), Apple's default */
#define PC_W_PE_DARK            16
#define PC_W_PE_PINGPONG        192
#define PC_W_EP                 500     /* E->P weight while open */
#define PC_PINGPONG_PER_S       3000    /* E<->P moves a second that count as ping-pong */
#define PC_MAX_CHANGES          64      /* recommendation changes applied per tick (the rest the next) */

enum { PC_FG, PC_DF, PC_UT, PC_BG, PC_CLASSES };
enum { LVL_E, LVL_P, LVL_PRIME, LVL_COUNT };
enum { LAT_RT, LAT_FG, LAT_DF, LAT_BG, LAT_CLASSES };
#define LAT_BINS 8
static const uint32_t lat_bin_us[LAT_BINS - 1] = { 20, 50, 100, 250, 1000, 4000, 16000 };
static const char *const class_name[PC_CLASSES] = { "fg", "df", "ut", "bg" };
static const char *const lat_name[LAT_CLASSES] = { "rt", "fg", "df", "bg" };
static const char *const level_name[LVL_COUNT] = { "E", "P", "prime" };

struct pc_cluster {
	uint32_t        id, type, ncpus, domain, first_cpu;
	uint64_t        cpus;
	uint32_t        max_khz;
	uint32_t        cap;                    /* one core at max_khz, PC_REF for the fastest */
	_Atomic uint32_t cur_cap;               /* one core at the speed it was asked for now */
	uint32_t        midr, l2_kb, bench;     /* what calibration found */
	uint32_t        dpc;                    /* dynamic power coefficient, uW/MHz/V^2 (the board's, as Linux's device tree has it) */
	uint32_t        cur_mw;                 /* one busy core at the speed asked for now, by the energy model */
	_Atomic uint32_t cur_njpk;              /* ... in nJ per 1024 ticks of mach time */
	uint32_t        util;                   /* busy, per mille of its cores, last tick */
	uint64_t        busy_total;             /* abs, all its cores */
	struct sched_edge_cluster_info info;
};

struct pc_cpu {
	uint64_t        last;                   /* abs time accounted up to */
	_Atomic uint64_t busy;                  /* abs busy since the last tick */
	uint64_t        busy_total;
	uint64_t        energy;                 /* nJ, by the model, this core's busy time since boot (written by the core) */
	uint32_t        cluster;
	bool            going_idle;             /* the switch in progress is to the idle thread (csw before on-core) */
} __attribute__((aligned(64)));

/* in each thread's perfcontrol state (64 bytes, osfmk/arm/thread.h) */
struct pc_thread_state {
	uint32_t        last_cluster;           /* +1; 0: never ran */
};

/* in each thread group's machine data (osfmk/kern/thread_group.c gives 256 bytes) */
struct pc_tg {
	_Atomic uint64_t work[PC_CLASSES];      /* abs x cap/PC_REF since the last tick */
	_Atomic uint64_t run;                   /* abs since the last tick */
	_Atomic uint32_t cpus;                  /* cores used since the last tick */
	_Atomic uint32_t migrations;            /* moves between clusters since the last tick */
	_Atomic uint32_t frames, missed;        /* work-interval finishes since the last tick */
	_Atomic uint32_t rs_waiting;            /* its threads blocked on the render server, now */
	_Atomic uint32_t wi_types;              /* work-interval types it has used (bit per type) */
	_Atomic uint32_t wi_need;               /* the heaviest frame since the last tick: its work at the fastest core's
	                                         * top speed, per mille of its period (start to deadline) */
	_Atomic uint64_t max_lat;               /* worst FG scheduling latency since the last tick, abs */
	_Atomic uint64_t energy;                /* nJ by the energy model, since it was made */
	uint32_t        flags;
	/* the controller thread's own */
	uint32_t        demand[PC_CLASSES];     /* EWMA, PC_REF = the fastest core all the time */
	uint32_t        width16;                /* EWMA of the cores used, x16 */
	uint32_t        pt;                     /* per-thread demand of FG+DF, PC_REF units */
	uint32_t        busy;                   /* EWMA: how busy its threads keep their cores, per mille */
	uint8_t         level[PC_CLASSES];
	uint8_t         applied;
	uint8_t         prime;
	uint16_t        up_ticks, down_ticks;
	uint64_t        interactive_until, interactive_last;
	uint64_t        miss_until;
	uint64_t        deadline_until;         /* deadline work seen until (1 s after its last frame) */
	uint32_t        need;                   /* EWMA of wi_need (rises at once, falls over ~8 ticks) */
	uint64_t        total_work, total_run;  /* abs (work: x cap/PC_REF) */
	uint32_t        total_migrations, total_frames, total_missed, lat_max_us;
	uint32_t        changes;
};
_Static_assert(sizeof(struct pc_tg) <= 256, "struct pc_tg must fit the thread group's machine data");

static struct pc_cluster pc_clusters[SCHED_PERFCONTROL_MAX_CLUSTERS];
static struct pc_cpu pc_cpus[MAX_CPUS];
static uint32_t pc_nclusters, pc_ncpus;
static uint32_t pc_level_cluster[LVL_COUNT];
static uint32_t pc_e_cluster = UINT32_MAX;      /* the E cluster, if any */
static uint32_t pc_prime_slots;                 /* groups the prime cluster takes: its cores */
static bool pc_on, pc_log, pc_calibrated, pc_observe;
static _Atomic bool pc_dark, pc_touched;
static int pc_event;
static uint64_t pc_ticks;

/* counts (cumulative) */
static _Atomic uint64_t pc_mig[SCHED_PERFCONTROL_MAX_CLUSTERS][SCHED_PERFCONTROL_MAX_CLUSTERS];
static _Atomic uint64_t pc_lat[LAT_CLASSES][SCHED_PERFCONTROL_MAX_CLUSTERS][LAT_BINS];
static _Atomic uint64_t pc_frames, pc_missed, pc_rs_blocks;
static _Atomic uint64_t pc_starved;             /* max_runnable_latency reports over 20 ms */
static uint64_t pc_rec_up, pc_rec_down, pc_prime_changes, pc_ep_opened, pc_floor_boosts;
static uint64_t pc_ep_open_until;               /* abs: E->P kept open (starvation) until */
static uint32_t pc_ep_sat_ticks, pc_ep_calm_ticks;
static bool pc_ep_open;
static uint32_t pc_w_pe = PC_W_PE;
static uint64_t pc_mig_ep_last;                 /* E<->P moves at the last ping-pong check */
static uint64_t pc_mig_check_at;
static uint64_t pc_prime_ids[4], pc_prime_since;

static void pc_choose_levels(void);

static uint64_t
abs_from_ms(uint32_t ms)
{
	uint64_t a;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &a);
	return a;
}

static uint32_t
us_from_abs(uint64_t a)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(a, &ns);
	return (uint32_t)MIN(ns / NSEC_PER_USEC, UINT32_MAX);
}

/* ---- the scheduler's callouts: interrupts off, no locks, only this core's slot and atomics --------------------- */

static inline uint32_t
class_of(perfcontrol_class_t c)
{
	switch (c) {
	case PERFCONTROL_CLASS_ABOVEUI:
	case PERFCONTROL_CLASS_UI:
	case PERFCONTROL_CLASS_REALTIME:
	case PERFCONTROL_CLASS_KERNEL:
		return PC_FG;
	case PERFCONTROL_CLASS_UTILITY:
		return PC_UT;
	case PERFCONTROL_CLASS_BACKGROUND:
		return PC_BG;
	default:
		return PC_DF;
	}
}

/* The time since the core's last accounting point, charged to the thread that ran it */
static inline void
pc_account(uint32_t cpu, uint64_t now, struct perfcontrol_thread_data *t)
{
	struct pc_cpu *c = &pc_cpus[cpu];
	uint64_t dt = now - c->last;

	if (c->last == 0 || now < c->last) {
		c->last = now;
		return;
	}
	c->last = now;
	if (t->perfctl_class == PERFCONTROL_CLASS_IDLE) {
		return;
	}
	os_atomic_add(&c->busy, dt, relaxed);
	/* the energy model's estimate, given back to XNU (CONFIG_PERVASIVE_ENERGY: the thread's and task's recount) */
	uint64_t nj = dt * os_atomic_load(&pc_clusters[c->cluster].cur_njpk, relaxed) >> 10;
	c->energy += nj;
	t->energy_estimate_nj += (uint32_t)MIN(nj, (uint64_t)UINT32_MAX - t->energy_estimate_nj);
	struct pc_tg *g = t->thread_group_data;
	if (g == NULL) {
		return;
	}
	os_atomic_add(&g->energy, nj, relaxed);
	uint32_t cap = os_atomic_load(&pc_clusters[c->cluster].cur_cap, relaxed);
	os_atomic_add(&g->work[class_of(t->perfctl_class)], dt * cap / PC_REF, relaxed);
	os_atomic_add(&g->run, dt, relaxed);
	os_atomic_or(&g->cpus, 1u << cpu, relaxed);
}

static void
pc_csw(perfcontrol_event event, uint32_t cpu_id, uint64_t timestamp, __unused uint32_t flags,
    struct perfcontrol_thread_data *offcore, struct perfcontrol_thread_data *oncore,
    __unused struct perfcontrol_cpu_counters *counters, __unused void *unused)
{
	if (cpu_id >= pc_ncpus) {
		return;
	}
	pc_account(cpu_id, timestamp, offcore);
	pc_cpus[cpu_id].going_idle = (event == IDLE);
	if (oncore->perfctl_class == PERFCONTROL_CLASS_IDLE || oncore->perfctl_state == NULL) {
		return;
	}
	/* a move between clusters: the thread's own last one, kept in its state */
	struct pc_thread_state *st = (struct pc_thread_state *)oncore->perfctl_state;
	uint32_t here = pc_cpus[cpu_id].cluster;

	if (st->last_cluster != 0 && st->last_cluster != here + 1) {
		os_atomic_inc(&pc_mig[st->last_cluster - 1][here], relaxed);
		if (oncore->thread_group_data != NULL) {
			os_atomic_inc(&((struct pc_tg *)oncore->thread_group_data)->migrations, relaxed);
		}
	}
	st->last_cluster = here + 1;
}

/* A long-running thread is charged at its quantum's end too, not only when it leaves the core */
static void
pc_state_update(__unused perfcontrol_event event, uint32_t cpu_id, uint64_t timestamp, __unused uint32_t flags,
    struct perfcontrol_thread_data *thr_data, __unused void *unused)
{
	if (cpu_id < pc_ncpus) {
		pc_account(cpu_id, timestamp, thr_data);
	}
}

static void
pc_oncore(__unused perfcontrol_state_t state, going_on_core_t on)
{
	uint32_t cpu = (uint32_t)cpu_number();

	if (cpu >= pc_ncpus || pc_cpus[cpu].going_idle) {
		return;
	}
	uint32_t us = us_from_abs(on->scheduling_latency);
	if (on->scheduling_latency == 0 || us > 1000000) {
		return;                 /* (not woken from a wait, or not measured) */
	}
	uint32_t lc = on->urgency == PC_URGENCY_REAL_TIME ? LAT_RT :
	    on->qos_class >= THREAD_QOS_USER_INITIATED ? LAT_FG :
	    on->qos_class >= THREAD_QOS_LEGACY || on->qos_class == THREAD_QOS_UNSPECIFIED ? LAT_DF : LAT_BG;
	uint32_t bin = 0;

	while (bin < LAT_BINS - 1 && us >= lat_bin_us[bin]) {
		bin++;
	}
	os_atomic_inc(&pc_lat[lc][pc_cpus[cpu].cluster][bin], relaxed);
	struct pc_tg *g = on->thread_group_data;
	if (g != NULL && (lc == LAT_FG || lc == LAT_RT)) {
		os_atomic_max(&g->max_lat, on->scheduling_latency, relaxed);
	}
}

static void
pc_offcore(__unused perfcontrol_state_t state, __unused going_off_core_t off, __unused boolean_t terminating)
{
}

static void
pc_tg_init(thread_group_data_t data)
{
	struct pc_tg *g = data->thread_group_data;

	g->flags = data->thread_group_flags;
}

static void
pc_tg_deinit(__unused thread_group_data_t data)
{
}

static void
pc_tg_flags(thread_group_data_t data)
{
	((struct pc_tg *)data->thread_group_data)->flags = data->thread_group_flags;
}

static void
pc_work_interval(__unused perfcontrol_state_t state, perfcontrol_work_interval_t wi)
{
	struct pc_tg *g = wi->thread_group_data;

	if (g == NULL) {
		return;
	}
	os_atomic_or(&g->wi_types, 1u << ((wi->create_flags & WORK_INTERVAL_TYPE_MASK) >> 28), relaxed);
	if (wi->finish != 0 && wi->deadline != 0) {
		os_atomic_inc(&g->frames, relaxed);
		os_atomic_inc(&pc_frames, relaxed);
		if (wi->deadline > wi->start && wi->finish > wi->start) {
			/* (the frame's time, from its start, as if run at the fastest core's top speed: the core it ended on, now) */
			uint32_t cpu = (uint32_t)cpu_number();
			uint32_t cap = cpu < pc_ncpus ? os_atomic_load(&pc_clusters[pc_cpus[cpu].cluster].cur_cap, relaxed) : PC_REF;
			uint64_t need = (wi->finish - wi->start) * 1000 / (wi->deadline - wi->start) * cap / PC_REF;
			os_atomic_max(&g->wi_need, (uint32_t)MIN(need, 4000ULL), relaxed);
		}
		if (wi->finish > wi->deadline) {
			os_atomic_inc(&g->missed, relaxed);
			os_atomic_inc(&pc_missed, relaxed);
			thread_wakeup((event_t)&pc_event);      /* (a late frame: the controller acts now, not at its next tick) */
		}
	}
}

/*
 * Something waits on the render server (blocking_tg NULL: the render server, a well-known group): its frames are
 * someone else's latency. The render server's group is the one that runs CoreAnimation's work intervals.
 */
static _Atomic uint32_t pc_rs_waiting;

static void
pc_tg_blocked(__unused thread_group_data_t blocked, thread_group_data_t blocking, uint32_t flags, __unused perfcontrol_state_t st)
{
	if (blocking == NULL && (flags & PERFCONTROL_CALLOUT_BLOCKING_TG_RENDER_SERVER)) {
		os_atomic_inc(&pc_rs_waiting, relaxed);
		os_atomic_inc(&pc_rs_blocks, relaxed);
	}
}

static void
pc_tg_unblocked(__unused thread_group_data_t unblocked, thread_group_data_t unblocking, uint32_t flags, __unused perfcontrol_state_t st)
{
	if (unblocking == NULL && (flags & PERFCONTROL_CALLOUT_BLOCKING_TG_RENDER_SERVER)) {
		uint32_t was = os_atomic_load(&pc_rs_waiting, relaxed);

		while (was > 0 && !os_atomic_cmpxchgv(&pc_rs_waiting, was, was - 1, &was, relaxed)) {
			;
		}
	}
}

static void
pc_max_runnable_latency(perfcontrol_max_runnable_latency_t l)
{
	if (us_from_abs(l->max_scheduling_latencies[PC_URGENCY_NORMAL]) > 20000) {
		os_atomic_inc(&pc_starved, relaxed);
	}
}

static struct sched_perfcontrol_callbacks pc_callbacks = {
	.version = SCHED_PERFCONTROL_CALLBACKS_VERSION_8,
	.offcore = pc_offcore,
	.oncore = pc_oncore,
	.max_runnable_latency = pc_max_runnable_latency,
	.work_interval_notify = pc_work_interval,
	.thread_group_init = pc_tg_init,
	.thread_group_deinit = pc_tg_deinit,
	.csw = pc_csw,
	.state_update = pc_state_update,
	.thread_group_flags_update = pc_tg_flags,
	.thread_group_blocked = pc_tg_blocked,
	.thread_group_unblocked = pc_tg_unblocked,
};

/* ---- the clusters: what the cores are, measured ------------------------------------------------------------------ */

/* A mixed integer workload (xorshift, table updates, data-dependent branches): ops a millisecond on this core */
static uint32_t
pc_bench(void)
{
	uint32_t table[256];
	uint64_t x = 0x9e3779b97f4a7c15ULL, acc = 0, start, ns;
	const uint32_t n = 400000;

	for (int i = 0; i < 256; i++) {
		table[i] = (uint32_t)(i * 2654435761u);
	}
	start = mach_absolute_time();
	for (uint32_t i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		uint32_t idx = (uint32_t)x & 255;
		table[idx] += (uint32_t)(x >> 32);
		if (table[(idx + 1) & 255] & 1) {
			acc += x >> 3;
		} else {
			acc -= idx;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	__asm__ volatile ("" : : "r"(acc), "r"(table) : "memory");
	return ns ? (uint32_t)((uint64_t)n * NSEC_PER_MSEC / ns) : 0;
}

static const char *
pc_part_name(uint32_t midr)
{
	if (((midr >> 24) & 0xff) == 0x51) {    /* Qualcomm's names for Arm's cores */
		switch ((midr >> 4) & 0xfff) {
		case 0x804: return "Kryo Gold (Cortex-A76)";
		case 0x805: return "Kryo Silver (Cortex-A55)";
		default: return "Qualcomm core";
		}
	}
	if (((midr >> 24) & 0xff) != 0x41) {
		return "non-Arm core";
	}
	switch ((midr >> 4) & 0xfff) {
	case 0xd05: return "Cortex-A55";
	case 0xd0b: return "Cortex-A76";
	case 0xd0d: return "Cortex-A77";
	case 0xd41: return "Cortex-A78";
	case 0xd44: return "Cortex-X1";
	case 0xd46: return "Cortex-A510";
	case 0xd47: return "Cortex-A710";
	default: return "Arm core";
	}
}

static void
pc_tell(const char *format, ...) __printflike(1, 2);

static void
pc_tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	loader_log_line(line);  /* (the kernel's own log of a run keeps only its end) */
}

/* The capacities from the top speeds, until they are measured: an E core about 0.42 of a P core per clock */
static void
pc_estimate_capacities(void)
{
	uint32_t best = 1;

	for (uint32_t c = 0; c < pc_nclusters; c++) {
		uint32_t khz = pc_clusters[c].max_khz ? pc_clusters[c].max_khz : 1000000;
		pc_clusters[c].cap = pc_clusters[c].type == CLUSTER_TYPE_E ? khz * 42 / 100 : khz;
		best = MAX(best, pc_clusters[c].cap);
	}
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		pc_clusters[c].cap = MAX(1u, (uint32_t)((uint64_t)pc_clusters[c].cap * PC_REF / best));
	}
}

static void
pc_update_cur_caps(void)
{
	bool learned = false;

	for (uint32_t c = 0; c < pc_nclusters; c++) {
		/* (the controller starts before the governor, bsd/dev/hk_cpufreq.c, has read the groups' tables) */
		if (pc_clusters[c].max_khz == 0 && hk_cpufreq_max_khz(pc_clusters[c].domain) != 0) {
			pc_clusters[c].max_khz = hk_cpufreq_max_khz(pc_clusters[c].domain);
			learned = true;
		}
	}
	if (learned && !pc_calibrated) {
		pc_estimate_capacities();
	}
	static uint64_t ns_per_1024;
	if (ns_per_1024 == 0) {
		absolutetime_to_nanoseconds(1024, &ns_per_1024);
	}
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];
		uint32_t khz = hk_cpufreq_cur_khz(k->domain);
		uint64_t mv = hk_cpufreq_mv(k->domain, hk_cpufreq_row(k->domain));

		/* P = C * f * V^2 (Linux's energy model for these cores): uW/MHz/V^2 * MHz * mV^2 / 10^6 = uW */
		k->cur_mw = (uint32_t)((uint64_t)k->dpc * (khz / 1000) * mv * mv / 1000000 / 1000);
		os_atomic_store(&k->cur_njpk, (uint32_t)((uint64_t)k->cur_mw * ns_per_1024 / 1000), relaxed);

		uint32_t cap = (k->max_khz && khz) ? (uint32_t)((uint64_t)k->cap * khz / k->max_khz) : k->cap;
		os_atomic_store(&k->cur_cap, MAX(1u, cap), relaxed);
	}
}

/*
 * Once every core is up: each cluster's cores read their MIDR_EL1 and L2 (CCSIDR_EL1 for level 2), and the first one
 * runs pc_bench() at the cluster's top speed. The capacities are then the measured speeds, the fastest PC_REF.
 */
static void
pc_calibrate(void)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	uint32_t best = 1;

	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];
		bool first = true;

		for (unsigned i = 0; i < t->num_cpus; i++) {
			if (!(k->cpus & (1ULL << i)) || !ml_cpu_online((int)i)) {
				continue;
			}
			processor_t was = thread_bind(cpu_to_processor((int)i));
			thread_block(THREAD_CONTINUE_NULL);
			uint32_t midr = (uint32_t)__builtin_arm_rsr64("MIDR_EL1");
			__builtin_arm_wsr64("CSSELR_EL1", 2);   /* level 2, data */
			__builtin_arm_isb(0xf);           /* ISB SY */
			uint64_t ccsidr = __builtin_arm_rsr64("CCSIDR_EL1");
			uint32_t l2_kb = (uint32_t)(((ccsidr >> 13) & 0x7fff) + 1) * (uint32_t)(((ccsidr >> 3) & 0x3ff) + 1) *
			    (16u << (ccsidr & 7)) / 1024;
			if (first) {
				k->midr = midr;
				k->l2_kb = l2_kb;
				if (k->max_khz) {
					hk_cpufreq_floor(k->domain, k->max_khz, true);
				}
				delay_for_interval(5, NSEC_PER_MSEC);
				k->bench = MAX(pc_bench(), MAX(pc_bench(), pc_bench()));
				if (k->max_khz) {
					hk_cpufreq_floor(k->domain, 0, false);
				}
				first = false;
			} else if (((midr >> 4) & 0xfff) != ((k->midr >> 4) & 0xfff) || l2_kb != k->l2_kb) {
				pc_tell("perfctl: cpu %u is a %s with %u KB of L2, unlike the rest of cluster %u (%s, %u KB): "
				    "the board's description of its domains (board_cpu_domains) is wrong?\n", i, pc_part_name(midr), l2_kb, c,
				    pc_part_name(k->midr), k->l2_kb);
			}
			thread_bind(was);
			thread_block(THREAD_CONTINUE_NULL);
		}
		best = MAX(best, k->bench);
	}
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];

		if (k->bench) {
			k->cap = MAX(1u, (uint32_t)((uint64_t)k->bench * PC_REF / best));
		}
		pc_tell("perfctl: cluster %u (%s, cpus 0x%llx): %s r%up%u, L2 %u KB, %u MHz, %u ops/ms -> capacity %u of %u\n",
		    c, k->type == CLUSTER_TYPE_E ? "E" : "P", k->cpus, pc_part_name(k->midr), (k->midr >> 20) & 0xf, k->midr & 0xf,
		    k->l2_kb, k->max_khz / 1000, k->bench, k->cap, PC_REF);
	}
	pc_update_cur_caps();
	if (!pc_observe) {
		pc_choose_levels();
	}
	pc_calibrated = true;
	pc_tell("perfctl: E work to cluster %u, P to %u, prime to %u (%u group(s) at a time)\n", pc_level_cluster[LVL_E],
	    pc_level_cluster[LVL_P], pc_level_cluster[LVL_PRIME], pc_prime_slots);
}

/* ---- the policy -------------------------------------------------------------------------------------------------- */

struct pc_change {
	struct thread_group *tg;
	void            *data;
	uint32_t        clusters[PC_CLASSES];
	bool            up;
};

struct pc_pass {
	uint64_t        now, window;
	bool            dark, touched, rs_waiting, missed_any;
	uint32_t        nchanges;
	struct pc_change changes[PC_MAX_CHANGES];
	uint32_t        floor_khz[SCHED_PERFCONTROL_MAX_CLUSTERS];
	bool            floor_now[SCHED_PERFCONTROL_MAX_CLUSTERS];
	/* the prime candidates of this pass */
	uint64_t        cand_id[4];
	uint32_t        cand_score[4];
};

static inline uint32_t
ewma(uint32_t old, uint32_t now)
{
	return now > old ? (old + 3 * now) / 4 : (7 * old + now) / 8;      /* up within a tick or two, down over ~8 */
}

static bool
pc_is_prime(uint64_t id)
{
	for (uint32_t i = 0; i < pc_prime_slots; i++) {
		if (pc_prime_ids[i] == id) {
			return true;
		}
	}
	return false;
}

static void
pc_candidate(struct pc_pass *p, uint64_t id, uint32_t score)
{
	uint32_t worst = 0;

	for (uint32_t i = 1; i < pc_prime_slots; i++) {
		if (p->cand_score[i] < p->cand_score[worst]) {
			worst = i;
		}
	}
	if (score > p->cand_score[worst]) {
		p->cand_score[worst] = score;
		p->cand_id[worst] = id;
	}
}

/* One group, with tg_lock held (no blocking): its window read, its demand and level; true to keep the reference */
static bool
pc_visit(struct thread_group *tg, void *data, void *arg)
{
	struct pc_pass *p = arg;
	struct pc_tg *g = data;
	uint64_t id = thread_group_get_id(tg);
	uint32_t window_demand[PC_CLASSES], width;
	const struct pc_cluster *e = pc_e_cluster != UINT32_MAX ? &pc_clusters[pc_e_cluster] : NULL;
	const struct pc_cluster *gold = &pc_clusters[pc_level_cluster[LVL_P]];

	for (int c = 0; c < PC_CLASSES; c++) {
		uint64_t w = os_atomic_xchg(&g->work[c], 0, relaxed);

		g->total_work += w;
		window_demand[c] = (uint32_t)MIN(w * PC_REF / p->window, 16 * PC_REF);
		g->demand[c] = ewma(g->demand[c], window_demand[c]);
	}
	uint64_t run = os_atomic_xchg(&g->run, 0, relaxed);
	g->total_run += run;
	width = (uint32_t)__builtin_popcount(os_atomic_xchg(&g->cpus, 0, relaxed));
	if (width) {
		g->width16 = ewma(g->width16, width * 16);
	}
	/* how busy its threads kept the cores they ran on (per mille): 1000 is a thread that never waits */
	g->busy = ewma(g->busy, width ? (uint32_t)MIN(run * 1000 / (p->window * width), 1000) : 0);
	uint32_t moves = os_atomic_xchg(&g->migrations, 0, relaxed);
	uint32_t frames = os_atomic_xchg(&g->frames, 0, relaxed), missed = os_atomic_xchg(&g->missed, 0, relaxed);
	uint64_t lat = os_atomic_xchg(&g->max_lat, 0, relaxed);
	g->total_migrations += moves;
	g->total_frames += frames;
	g->total_missed += missed;
	if (lat) {
		g->lat_max_us = MAX(g->lat_max_us * 15 / 16, us_from_abs(lat));
	}

	uint32_t need = os_atomic_xchg(&g->wi_need, 0, relaxed);
	if (frames) {
		g->deadline_until = p->now + abs_from_ms(1000);
		g->need = MAX(need, (7 * g->need + need) / 8);
	} else if (p->now >= g->deadline_until) {
		g->need = 0;
	}
	bool deadline_work = p->now < g->deadline_until;

	uint32_t main_demand = g->demand[PC_FG] + g->demand[PC_DF];
	g->pt = main_demand * 16 / MAX(16u, g->width16);
	bool frame_producer = (os_atomic_load(&g->wi_types, relaxed) & (1u << (WORK_INTERVAL_TYPE_COREANIMATION >> 28))) != 0;

	/* interactive: lit, and work the user is waiting for */
	bool sign = !p->dark && (window_demand[PC_FG] >= PC_FG_SIGN || frames != 0 || (frame_producer && p->rs_waiting) ||
	    (g->flags & THREAD_GROUP_FLAGS_UI_APP && main_demand >= PC_FG_SIGN));
	if (sign) {
		g->interactive_until = p->now + abs_from_ms(PC_INTERACTIVE_HOLD_MS);
		g->interactive_last = p->now;
	} else if (p->touched && !p->dark && g->interactive_last != 0 && p->now - g->interactive_last < abs_from_ms(PC_RECENT_MS)) {
		g->interactive_until = p->now + abs_from_ms(PC_INTERACTIVE_HOLD_MS);
	}
	if (missed) {
		g->miss_until = p->now + abs_from_ms(100);
		p->missed_any = true;
	}
	bool interactive = !p->dark && p->now < g->interactive_until;

	/*
	 * Work with deadlines (work intervals: CoreAnimation's frames, CoreAudio's cycles, the screen on or off): placed
	 * by how much of its period a frame needs. It stays on E only if a frame would take under PC_DEADLINE_E of its
	 * period there at E's top speed, and goes to the prime cluster when on P it would take over PC_DEADLINE_PRIME, or
	 * when frames are missed on P. A missed frame puts it on P at once.
	 */
	uint8_t deadline_level = LVL_E;
	if (deadline_work) {
		uint32_t on_e = e ? g->need * PC_REF / e->cap : UINT32_MAX;       /* per mille of the period, on E */
		uint32_t on_p = g->need * PC_REF / gold->cap;
		bool missing = p->now < g->miss_until;

		deadline_level = (missing || on_e > PC_DEADLINE_E) ? LVL_P : LVL_E;
		if (deadline_level == LVL_P && (on_p > PC_DEADLINE_PRIME || (missing && g->level[PC_FG] >= LVL_P))) {
			deadline_level = LVL_PRIME;
		}
	}

	/*
	 * The main level (FG and DF work). A group the controller has not placed yet (a program just launched, a new
	 * work interval) starts on P with the screen lit -- a launch is the user waiting -- or with UI-class work in its first
	 * tick, and on E otherwise; from
	 * there it moves by its demand like any other (down after PC_SUSTAIN quiet ticks).
	 */
	uint8_t main_level = g->applied ? g->level[PC_DF] : (p->dark && window_demand[PC_FG] == 0 ? LVL_E : LVL_P);
	if (e == NULL || (g->flags & THREAD_GROUP_FLAGS_EFFICIENT)) {
		main_level = e == NULL ? LVL_P : LVL_E;
	} else if (deadline_work && !interactive) {
		main_level = MIN(deadline_level, LVL_P);
		g->up_ticks = g->down_ticks = 0;
	} else if (interactive) {
		main_level = LVL_P;
		g->up_ticks = g->down_ticks = 0;
	} else {
		/*
		 * Too much for E: more than PC_E_UP of an E core at its top speed for one thread, or of the E cluster for
		 * the group -- or threads that never wait, on an E cluster already at its top speed (the demand measured
		 * there cannot show more than the core gives). Up after PC_SUSTAIN_UP ticks, down after PC_SUSTAIN.
		 */
		const struct pc_cluster *ek = &pc_clusters[pc_e_cluster];
		bool e_flat_out = ek->max_khz == 0 || hk_cpufreq_cur_khz(ek->domain) * 10 >= ek->max_khz * 9;
		bool too_much = g->pt * 100 > e->cap * PC_E_UP || main_demand * 100 > e->cap * e->ncpus * PC_E_UP ||
		    (g->busy >= 850 && g->level[PC_DF] == LVL_E && e_flat_out && main_demand > 0);
		bool fits = g->pt * 100 < e->cap * PC_E_DOWN && main_demand * 100 < e->cap * e->ncpus * PC_E_DOWN && g->busy < 700;

		g->up_ticks = too_much ? MIN(g->up_ticks + 1, 1000) : 0;
		g->down_ticks = fits ? MIN(g->down_ticks + 1, 1000) : 0;
		uint32_t sustain = p->dark ? MAX(2, PC_SUSTAIN * PC_TICK_MS / PC_TICK_DARK_MS) : PC_SUSTAIN;
		uint32_t sustain_up = p->dark ? 2 : PC_SUSTAIN_UP;
		if (main_level == LVL_E && g->up_ticks >= sustain_up) {
			main_level = LVL_P;
		} else if (main_level != LVL_E && g->down_ticks >= sustain) {
			main_level = LVL_E;
		} else if (main_level == LVL_PRIME) {
			main_level = LVL_P;
		}
	}

	/* the prime cluster: chosen among this pass's candidates at its end (pc_choose_prime()) */
	uint8_t fg_level = main_level;
	if (main_level == LVL_P && (interactive || deadline_level == LVL_PRIME) && pc_prime_slots &&
	    pc_level_cluster[LVL_PRIME] != pc_level_cluster[LVL_P]) {
		bool missing = p->now < g->miss_until;
		if (missing || deadline_level == LVL_PRIME || g->pt * 100 >= gold->cap * PC_PRIME_UP) {
			pc_candidate(p, id, g->pt + (missing ? PC_REF : 0) + (deadline_level == LVL_PRIME ? PC_REF / 2 : 0));
		}
		if (pc_is_prime(id)) {
			fg_level = LVL_PRIME;
		}
	}
	g->prime = fg_level == LVL_PRIME;

	uint8_t want[PC_CLASSES] = { fg_level, fg_level == LVL_PRIME ? LVL_PRIME : main_level, LVL_E, LVL_E };
	if (e == NULL) {
		want[PC_UT] = want[PC_BG] = LVL_P;
	}

	/*
	 * A clock floor for interactive work (its busiest thread's demand on its cluster, with headroom) and for deadline
	 * work (a frame in PC_DEADLINE_TARGET of its period), screen on or off.
	 */
	if (interactive || deadline_work) {
		uint32_t c = pc_level_cluster[fg_level];
		const struct pc_cluster *k = &pc_clusters[c];

		if (k->max_khz) {
			uint32_t khz = interactive ? (uint32_t)MIN((uint64_t)g->pt * k->max_khz * 5 / 4 / k->cap, k->max_khz) : 0;
			if (deadline_work) {
				uint64_t on_k = (uint64_t)g->need * PC_REF / k->cap;     /* per mille of the period at this cluster's top */
				khz = MAX(khz, (uint32_t)MIN(on_k * k->max_khz / PC_DEADLINE_TARGET, k->max_khz));
			}
			if (p->now < g->miss_until) {
				khz = k->max_khz;
				p->floor_now[c] = true;
			}
			p->floor_khz[c] = MAX(p->floor_khz[c], khz);
		}
	}

	if (pc_observe) {
		return false;   /* (only the measurements: sched=clutch, or one cluster) */
	}
	bool changed = !g->applied;
	bool up = false;
	for (int c = 0; c < PC_CLASSES; c++) {
		if (want[c] != g->level[c]) {
			changed = true;
			up |= want[c] > g->level[c];
		}
	}
	if (!changed || p->nchanges >= PC_MAX_CHANGES) {
		return false;
	}
	struct pc_change *ch = &p->changes[p->nchanges++];
	ch->tg = tg;
	ch->data = data;
	ch->up = up;
	for (int c = 0; c < PC_CLASSES; c++) {
		ch->clusters[c] = pc_level_cluster[want[c]];
		if (pc_log && g->applied && want[c] != g->level[c]) {
			printf("perfctl: group %llu %s: %s %s -> %s (demand fg %u df %u ut %u bg %u, width %u.%u, per thread %u%s%s)\n",
			    id, thread_group_get_name(tg), class_name[c], level_name[g->level[c]], level_name[want[c]], g->demand[PC_FG],
			    g->demand[PC_DF], g->demand[PC_UT], g->demand[PC_BG], g->width16 / 16, (g->width16 % 16) * 10 / 16, g->pt,
			    interactive ? ", interactive" : "", missed ? ", missed frames" : "");
		}
		g->level[c] = want[c];
	}
	if (g->applied) {
		g->changes++;
		up ? pc_rec_up++ : pc_rec_down++;
	}
	g->applied = 1;
	return true;
}

static void
pc_apply(struct pc_change *ch)
{
	uint32_t overrides[PERFCONTROL_CLASS_MAX];

	for (int i = 0; i < PERFCONTROL_CLASS_MAX; i++) {
		overrides[i] = SCHED_PERFCONTROL_PREFERRED_CLUSTER_OVERRIDE_NONE;
	}
	overrides[PERFCONTROL_CLASS_ABOVEUI] = ch->clusters[PC_FG];
	overrides[PERFCONTROL_CLASS_UI] = ch->clusters[PC_FG];
	overrides[PERFCONTROL_CLASS_NONUI] = ch->clusters[PC_DF];
	overrides[PERFCONTROL_CLASS_UTILITY] = ch->clusters[PC_UT];
	overrides[PERFCONTROL_CLASS_BACKGROUND] = ch->clusters[PC_BG];
	sched_perfcontrol_thread_group_preferred_clusters_set(ch->data, ch->clusters[PC_DF], overrides,
	    ch->up ? (SCHED_PERFCONTROL_PREFERRED_CLUSTER_MIGRATE_RUNNING | SCHED_PERFCONTROL_PREFERRED_CLUSTER_MIGRATE_RUNNABLE) : 0);
}

/* The prime cluster's groups: kept unless a candidate is clearly busier, or they stopped being candidates */
static void
pc_choose_prime(struct pc_pass *p)
{
	if (pc_prime_slots == 0 || p->now - pc_prime_since < abs_from_ms(PC_PRIME_DWELL_MS)) {
		return;
	}
	uint64_t next[4] = { 0 };
	bool differ = false;

	for (uint32_t i = 0; i < pc_prime_slots; i++) {
		next[i] = p->cand_score[i] ? p->cand_id[i] : 0;
	}
	for (uint32_t i = 0; i < pc_prime_slots; i++) {
		if (!pc_is_prime(next[i]) || (next[i] == 0 && pc_prime_ids[i] != 0)) {
			differ = true;
		}
	}
	if (differ) {
		memcpy(pc_prime_ids, next, sizeof(next));
		pc_prime_since = p->now;
		pc_prime_changes++;
	}
}

static void
pc_edges(struct pc_pass *p)
{
	sched_clutch_edge matrix[SCHED_PERFCONTROL_MAX_CLUSTERS * SCHED_PERFCONTROL_MAX_CLUSTERS];
	bool changes[SCHED_PERFCONTROL_MAX_CLUSTERS * SCHED_PERFCONTROL_MAX_CLUSTERS];
	sched_clutch_edge now[SCHED_PERFCONTROL_MAX_CLUSTERS * SCHED_PERFCONTROL_MAX_CLUSTERS];
	bool request[SCHED_PERFCONTROL_MAX_CLUSTERS * SCHED_PERFCONTROL_MAX_CLUSTERS];
	uint32_t n = pc_nclusters;
	bool any = false;

	if (pc_e_cluster != UINT32_MAX) {
		const struct sched_edge_cluster_info *ei = &pc_clusters[pc_e_cluster].info;
		uint32_t limit = p->dark ? 4 * PC_E_SAT_US : PC_E_SAT_US;
		bool saturated = ei->available_cpus != 0 && ei->load[PC_BUCKET_DF] != UINT32_MAX &&
		    (ei->load[PC_BUCKET_DF] >= limit || ei->runnable > 2 * ei->available_cpus);
		uint64_t starved = os_atomic_xchg(&pc_starved, 0, relaxed);

		if (starved) {
			pc_ep_open_until = p->now + abs_from_ms(250);
		}
		pc_ep_sat_ticks = saturated ? pc_ep_sat_ticks + 1 : 0;
		pc_ep_calm_ticks = saturated ? 0 : pc_ep_calm_ticks + 1;
		bool open = pc_ep_open;
		if (!open && (pc_ep_sat_ticks >= 2 || p->now < pc_ep_open_until)) {
			open = true;
			pc_ep_opened++;
		} else if (open && pc_ep_calm_ticks >= PC_SUSTAIN && p->now >= pc_ep_open_until) {
			open = false;
		}
		pc_ep_open = open;

		/* threads ping-ponging between the core types: the spill down made dearer for a while */
		if (p->now - pc_mig_check_at >= abs_from_ms(1000)) {
			uint64_t ep = 0;
			for (uint32_t a = 0; a < n; a++) {
				for (uint32_t b = 0; b < n; b++) {
					if (pc_clusters[a].type != pc_clusters[b].type) {
						ep += os_atomic_load(&pc_mig[a][b], relaxed);
					}
				}
			}
			uint64_t rate = (ep - pc_mig_ep_last) * NSEC_PER_SEC / MAX(1ULL, (uint64_t)us_from_abs(p->now - pc_mig_check_at) * 1000);
			pc_w_pe = p->dark ? PC_W_PE_DARK : rate > PC_PINGPONG_PER_S ? PC_W_PE_PINGPONG : PC_W_PE;
			pc_mig_ep_last = ep;
			pc_mig_check_at = p->now;
		}
	}
	for (uint32_t s = 0; s < n; s++) {
		for (uint32_t d = 0; d < n; d++) {
			sched_clutch_edge edge = { .sce_migration_weight = 0, .sce_migration_allowed = 0, .sce_steal_allowed = 0 };

			if (s != d) {
				if (pc_clusters[s].type == pc_clusters[d].type) {
					edge = (sched_clutch_edge){ .sce_migration_weight = 0, .sce_migration_allowed = 1, .sce_steal_allowed = 1 };
				} else if (pc_clusters[s].type == CLUSTER_TYPE_P) {
					edge = (sched_clutch_edge){ .sce_migration_weight = pc_w_pe, .sce_migration_allowed = 1, .sce_steal_allowed = 1 };
				} else if (pc_ep_open) {
					edge = (sched_clutch_edge){ .sce_migration_weight = PC_W_EP, .sce_migration_allowed = 1, .sce_steal_allowed = 1 };
				}
			}
			matrix[s * n + d] = edge;
			request[s * n + d] = true;
		}
	}
	sched_perfcontrol_edge_matrix_get(now, request, 0, n);
	for (uint32_t i = 0; i < n * n; i++) {
		changes[i] = (i / n != i % n) && now[i].sce_edge_packed != matrix[i].sce_edge_packed;
		any |= changes[i];
	}
	if (any) {
		sched_perfcontrol_edge_matrix_set(matrix, changes, 0, n);
	}
}

static void
pc_tick(void)
{
	static struct pc_pass pass;
	static uint64_t last;
	struct pc_pass *p = &pass;
	uint64_t now = mach_absolute_time();

	if (last == 0 || now <= last) {
		last = now;
		return;
	}
	bzero(p, sizeof(*p));
	p->now = now;
	p->window = now - last;
	last = now;
	p->dark = os_atomic_load(&pc_dark, relaxed);
	p->touched = os_atomic_xchg(&pc_touched, false, relaxed);
	p->rs_waiting = os_atomic_load(&pc_rs_waiting, relaxed) != 0;
	pc_ticks++;

	pc_update_cur_caps();
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];
		uint64_t busy = 0;

		for (uint32_t i = 0; i < pc_ncpus; i++) {
			if (pc_cpus[i].cluster == c) {
				uint64_t b = os_atomic_xchg(&pc_cpus[i].busy, 0, relaxed);
				pc_cpus[i].busy_total += b;
				busy += b;
			}
		}
		k->busy_total += busy;
		k->util = (uint32_t)MIN(busy * 1000 / (p->window * k->ncpus), 1000);
		if (!sched_edge_cluster_info_get(c, &k->info)) {
			bzero(&k->info, sizeof(k->info));
		}
	}

	thread_group_perfctl_iterate(pc_visit, p);
	for (uint32_t i = 0; i < p->nchanges; i++) {
		pc_apply(&p->changes[i]);
		thread_group_release(p->changes[i].tg);
	}
	if (pc_observe) {
		return;
	}
	pc_choose_prime(p);
	pc_edges(p);
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		if (p->floor_now[c]) {
			pc_floor_boosts++;
		}
		hk_cpufreq_floor(pc_clusters[c].domain, p->floor_khz[c], p->floor_now[c]);
	}
}

/* A touch or a key (bsd/dev/hk_cpufreq.c's boost, any context): recently interactive groups back on P now */
void
hk_perfctl_touch(void)
{
	if (pc_on) {
		os_atomic_store(&pc_touched, true, relaxed);
		thread_wakeup((event_t)&pc_event);
	}
}

void
hk_perfctl_display(bool on)
{
	os_atomic_store(&pc_dark, !on, relaxed);
	if (pc_on) {
		thread_wakeup((event_t)&pc_event);
	}
}

/* (Mach's wait, not tsleep(): the controller starts before BSD, GenericARMPE) */
static void
pc_wait(uint32_t ms)
{
	assert_wait_timeout((event_t)&pc_event, THREAD_UNINT, ms, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pc_thread(__unused void *arg, __unused wait_result_t wr)
{
	const ml_topology_info_t *t = ml_get_topology_info();

	uint32_t cpufreq = 0;
	bool speeds_known;

	thread_group_join_perf_controller();
	PE_parse_boot_argn("cpufreq", &cpufreq, sizeof(cpufreq));
	/*
	 * The capacities are measured when every core is up (the E cores start late, GenericARMPE) and, with cpufreq=1,
	 * when the governor knows each group's top speed (it starts with the drivers, after the root is mounted).
	 */
	for (int i = 0; i < 3000; i++) {
		speeds_known = true;
		for (uint32_t c = 0; c < pc_nclusters && cpufreq; c++) {
			speeds_known &= pc_clusters[c].max_khz != 0;
		}
		if (processor_avail_count >= t->num_cpus && speeds_known) {
			break;
		}
		pc_wait(PC_TICK_MS);
		pc_tick();
	}
	pc_calibrate();
	for (;;) {
		bool dark = os_atomic_load(&pc_dark, relaxed);

		pc_wait(dark ? PC_TICK_DARK_MS : PC_TICK_MS);
		pc_tick();
	}
}

/* Levels to clusters: E the E cluster (the slowest if none), P the P cluster of most cores, prime the fastest */
static void
pc_choose_levels(void)
{
	uint32_t e = UINT32_MAX, gold = 0, prime = 0, slowest = 0;
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];

		if (k->type == CLUSTER_TYPE_E && (e == UINT32_MAX || k->ncpus > pc_clusters[e].ncpus)) {
			e = c;
		}
		if (k->type == CLUSTER_TYPE_P && (pc_clusters[gold].type != CLUSTER_TYPE_P || k->ncpus > pc_clusters[gold].ncpus)) {
			gold = c;
		}
		if (k->type == CLUSTER_TYPE_P && (pc_clusters[prime].type != CLUSTER_TYPE_P || k->cap > pc_clusters[prime].cap)) {
			prime = c;     /* (the fastest P cluster: the prime level is a P cluster, whatever the E cores measure) */
		}
		if (k->cap < pc_clusters[slowest].cap) {
			slowest = c;
		}
	}
	pc_e_cluster = e;
	pc_level_cluster[LVL_E] = e != UINT32_MAX ? e : slowest;
	pc_level_cluster[LVL_P] = gold;
	pc_level_cluster[LVL_PRIME] = prime;
	sched_edge_set_promotion_cluster(e != UINT32_MAX ? gold : UINT32_MAX);
	pc_prime_slots = prime != gold ? MIN(pc_clusters[prime].ncpus, 4u) : 0;
	/* a touch raises the clusters interactive work goes to */
	uint32_t boost = 0;
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		if (c == gold || c == prime || e == UINT32_MAX) {
			boost |= 1u << pc_clusters[c].domain;
		}
	}
	hk_cpufreq_boost_domains(boost);
}

void
hk_perfctl_init(void)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	uint32_t on = 1, log = 0;
	thread_t thread;

	PE_parse_boot_argn("perfctl", &on, sizeof(on));
	PE_parse_boot_argn("perfctl_log", &log, sizeof(log));
	pc_log = log != 0;
	if (!on) {
		printf("perfctl: off (perfctl=0)\n");
		return;
	}
	if (thread_group_machine_data_size() < sizeof(struct pc_tg)) {
		printf("perfctl: off: thread groups have %u bytes for it, it needs %lu\n", thread_group_machine_data_size(), sizeof(struct pc_tg));
		return;
	}
	pc_ncpus = MIN(t->num_cpus, MAX_CPUS);
	/*
	 * With any other scheduler (sched=clutch), or one cluster, it only observes: the same accounting, capacities and
	 * energy model per frequency domain, the groups' demand -- so that the baseline is measured with the same meter --
	 * and no recommendation, graph or clock floor.
	 */
	pc_observe = !sched_edge_active() || t->num_clusters < 2;
	if (pc_observe) {
		for (uint32_t i = 0; i < pc_ncpus; i++) {
			uint32_t d = ml_cpu_frequency_domain((int)i), c;

			for (c = 0; c < pc_nclusters && pc_clusters[c].domain != d; c++) {
				;
			}
			if (c == pc_nclusters) {
				if (pc_nclusters == SCHED_PERFCONTROL_MAX_CLUSTERS) {
					continue;
				}
				pc_nclusters++;
				pc_clusters[c].id = c;
				pc_clusters[c].domain = d;
				pc_clusters[c].first_cpu = i;
				pc_clusters[c].type = ml_cpu_is_small_core((int)i) ? CLUSTER_TYPE_E : CLUSTER_TYPE_P;
			}
			pc_clusters[c].cpus |= 1ULL << i;
			pc_clusters[c].ncpus++;
		}
	} else {
		pc_nclusters = MIN(t->num_clusters, SCHED_PERFCONTROL_MAX_CLUSTERS);
		for (uint32_t c = 0; c < pc_nclusters; c++) {
			pc_clusters[c].id = t->clusters[c].cluster_id;
			pc_clusters[c].type = t->clusters[c].cluster_type;
			pc_clusters[c].cpus = t->clusters[c].cpu_mask;
			pc_clusters[c].ncpus = MAX(1u, t->clusters[c].num_cpus);
			pc_clusters[c].first_cpu = t->clusters[c].first_cpu_id;
		}
	}
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];

		k->domain = ml_cpu_frequency_domain((int)k->first_cpu);
		k->max_khz = hk_cpufreq_max_khz(k->domain);
		/*
		 * The energy model's coefficient per domain: board_cpu_dpc<d>=, by default the OnePlus 8's (SM8250, its OxygenOS
		 * device tree: cpu@0-300 dynamic-power-coefficient 100, cpu@400-600 533, cpu@700 642).
		 */
		static const uint32_t sm8250_dpc[] = { 100, 533, 642 };
		char name[16];
		snprintf(name, sizeof(name), "cpu_dpc%u", k->domain);
		k->dpc = hk_board_number(name, k->domain < 3 ? sm8250_dpc[k->domain] : 0);
		for (uint32_t i = 0; i < pc_ncpus; i++) {
			if (k->cpus & (1ULL << i)) {
				pc_cpus[i].cluster = c;
			}
		}
	}
	pc_estimate_capacities();
	pc_update_cur_caps();
	if (!pc_observe) {
		pc_choose_levels();
	}

	pc_on = true;
	sched_perfcontrol_register_callbacks(&pc_callbacks, sizeof(struct pc_thread_state));
	pc_tell("perfctl: %s: %u clusters; E work to cluster %u, P to %u, prime to %u (%u group(s) at a time)\n",
	    pc_observe ? "observing only" : "on", pc_nclusters,
	    pc_level_cluster[LVL_E], pc_level_cluster[LVL_P], pc_level_cluster[LVL_PRIME], pc_prime_slots);
	if (kernel_thread_start_priority(pc_thread, NULL, 94, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "performance controller");
		thread_deallocate(thread);
	}
}

/* For bsd/dev/hk_sched_check.c: the level (0 E, 1 P, 2 prime) the controller gave a group's class, -1 if none */
int hk_perfctl_group_level(struct thread_group *tg, int class);
bool hk_perfctl_ready(void);

int
hk_perfctl_group_level(struct thread_group *tg, int class)
{
	struct pc_tg *g = thread_group_get_machine_data(tg);

	if (!pc_on || class < 0 || class >= PC_CLASSES || !g->applied) {
		return -1;
	}
	return g->level[class];
}

bool
hk_perfctl_ready(void)
{
	return pc_on && pc_calibrated;
}

/* ---- kern.hackios.amp ---------------------------------------------------------------------------------------------- */

#define PC_REPORT_GROUPS 24
struct pc_report_group {
	uint64_t        id, work;
	char            name[THREAD_GROUP_MAXNAME + 1];
	struct pc_tg    g;
};
struct pc_report {
	uint32_t        n;
	struct pc_report_group groups[PC_REPORT_GROUPS];
};

static bool
pc_report_visit(struct thread_group *tg, void *data, void *arg)
{
	struct pc_report *r = arg;
	struct pc_tg *g = data;
	uint64_t work = g->total_work;
	uint32_t slot = r->n;

	if (work == 0) {
		return false;
	}
	if (r->n == PC_REPORT_GROUPS) {
		slot = 0;
		for (uint32_t i = 1; i < r->n; i++) {
			if (r->groups[i].work < r->groups[slot].work) {
				slot = i;
			}
		}
		if (r->groups[slot].work >= work) {
			return false;
		}
	} else {
		r->n++;
	}
	r->groups[slot].id = thread_group_get_id(tg);
	r->groups[slot].work = work;
	strlcpy(r->groups[slot].name, thread_group_get_name(tg), sizeof(r->groups[slot].name));
	r->groups[slot].g = *g;
	return false;
}

/* The report into text (size bytes): its length. Also for bsd/dev/hk_sched_check.c, which builds it once (QEMU has no reader). */
size_t hk_perfctl_report(char *text, size_t size);

size_t
hk_perfctl_report(char *text, size_t size)
{
	struct pc_report *r = kalloc_type(struct pc_report, Z_WAITOK | Z_ZERO);
	struct sched_edge_counters ec;
	size_t at = 0;

	if (r == NULL) {
		return 0;
	}
#define OUT(...) do { if (at < size) at += (size_t)snprintf(text + at, size - at, __VA_ARGS__); } while (0)
	OUT("amp: scheduler %s, controller %s, calibrated %d, ticks %llu, screen %s\n", sched_edge_active() ? "edge" : "clutch",
	    !pc_on ? "off" : pc_observe ? "observing" : "on", pc_calibrated, pc_ticks, os_atomic_load(&pc_dark, relaxed) ? "off" : "on");
	{
		const ml_topology_info_t *t = ml_get_topology_info();
		for (unsigned i = 0; i < t->num_cpus; i++) {
			OUT("core %u domain %u %s cluster %u\n", i, ml_cpu_frequency_domain((int)i), ml_cpu_is_small_core((int)i) ? "small" : "big",
			    t->cpus[i].cluster_id);
		}
	}
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		struct pc_cluster *k = &pc_clusters[c];

		OUT("cluster %u %s cpus 0x%llx domain %u %s l2 %u cap %u dpc %u maxmhz %u mhz %u util %u avail %u runq %u foreign %u busyms %llu load",
		    c, k->type == CLUSTER_TYPE_E ? "E" : "P", k->cpus, k->domain, pc_part_name(k->midr), k->l2_kb, k->cap, k->dpc, k->max_khz / 1000,
		    hk_cpufreq_cur_khz(k->domain) / 1000, k->util, k->info.available_cpus, k->info.runnable, k->info.running_foreign,
		    (unsigned long long)us_from_abs(k->busy_total) / 1000);
		for (int b = 0; b < SCHED_PERFCONTROL_BUCKETS; b++) {
			OUT(" %u", k->info.load[b] == UINT32_MAX ? 0 : k->info.load[b]);
		}
		OUT(" exec");
		for (int b = 0; b < SCHED_PERFCONTROL_BUCKETS; b++) {
			OUT(" %u", k->info.exec_us[b]);
		}
		OUT("\n");
	}
	OUT("cpus busyms");
	for (uint32_t i = 0; i < pc_ncpus; i++) {
		OUT(" %u:%llu", i, (unsigned long long)us_from_abs(pc_cpus[i].busy_total) / 1000);
	}
	OUT("\n");
	{
		uint64_t all = 0;
		OUT("energy mj");
		for (uint32_t c = 0; c < pc_nclusters; c++) {
			uint64_t e = 0;
			for (uint32_t i = 0; i < pc_ncpus; i++) {
				if (pc_cpus[i].cluster == c) {
					e += pc_cpus[i].energy;
				}
			}
			all += e;
			OUT(" %llu", e / 1000000);
		}
		OUT(" total %llu mw", all / 1000000);
		for (uint32_t c = 0; c < pc_nclusters; c++) {
			OUT(" %u", pc_clusters[c].cur_mw);
		}
		OUT("\n");
	}
	if (pc_nclusters && !pc_observe) {     /* (observing, the clusters are frequency domains, not the scheduler's psets) */
		sched_clutch_edge m[SCHED_PERFCONTROL_MAX_CLUSTERS * SCHED_PERFCONTROL_MAX_CLUSTERS];
		bool req[SCHED_PERFCONTROL_MAX_CLUSTERS * SCHED_PERFCONTROL_MAX_CLUSTERS];
		uint32_t n = pc_nclusters;

		memset(req, 1, sizeof(req));
		sched_perfcontrol_edge_matrix_get(m, req, 0, n);
		OUT("edges");
		for (uint32_t i = 0; i < n * n; i++) {
			if (i / n != i % n) {
				OUT(" %u>%u:%s%s/%u", i / n, i % n, m[i].sce_migration_allowed ? "m" : "-", m[i].sce_steal_allowed ? "s" : "-",
				    m[i].sce_migration_weight);
			}
		}
		OUT("\n");
	}
	sched_edge_counters_get(&ec);
	OUT("edge placed");
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		OUT(" %llu", ec.placed[c]);
	}
	OUT(" rebalance_ipi");
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		OUT(" %llu", ec.rebalance_ipi[c]);
	}
	OUT(" amp_rebalance");
	for (uint32_t c = 0; c < pc_nclusters; c++) {
		OUT(" %llu", ec.amp_rebalance[c]);
	}
	OUT(" tg_migrated %llu\n", ec.tg_migrated);
	for (uint32_t s = 0; s < pc_nclusters; s++) {
		OUT("edge from %u: away", s);
		for (uint32_t d = 0; d < pc_nclusters; d++) {
			OUT(" %llu", ec.placed_away[s][d]);
		}
		OUT(" steal");
		for (uint32_t d = 0; d < pc_nclusters; d++) {
			OUT(" %llu", ec.steal[s][d]);
		}
		OUT(" pull");
		for (uint32_t d = 0; d < pc_nclusters; d++) {
			OUT(" %llu", ec.foreign_pull[s][d]);
		}
		OUT(" moves");
		for (uint32_t d = 0; d < pc_nclusters; d++) {
			OUT(" %llu", os_atomic_load(&pc_mig[s][d], relaxed));
		}
		OUT("\n");
	}
	OUT("controller rec_up %llu rec_down %llu prime_changes %llu prime", pc_rec_up, pc_rec_down, pc_prime_changes);
	for (uint32_t i = 0; i < pc_prime_slots; i++) {
		OUT(" %llu", pc_prime_ids[i]);
	}
	OUT(" ep_open %d ep_opened %llu w_pe %u frames %llu missed %llu rs_blocks %llu floor_boosts %llu\n", pc_ep_open, pc_ep_opened,
	    pc_w_pe, os_atomic_load(&pc_frames, relaxed), os_atomic_load(&pc_missed, relaxed), os_atomic_load(&pc_rs_blocks, relaxed),
	    pc_floor_boosts);
	OUT("latency bins_us <20 <50 <100 <250 <1000 <4000 <16000 more\n");
	for (uint32_t l = 0; l < LAT_CLASSES; l++) {
		for (uint32_t c = 0; c < pc_nclusters; c++) {
			OUT("latency %s %u", lat_name[l], c);
			for (uint32_t b = 0; b < LAT_BINS; b++) {
				OUT(" %llu", os_atomic_load(&pc_lat[l][c][b], relaxed));
			}
			OUT("\n");
		}
	}
	if (pc_on) {
		thread_group_perfctl_iterate(pc_report_visit, r);
		for (uint32_t i = 0; i < r->n; i++) {
			struct pc_report_group *x = &r->groups[i];
			struct pc_tg *g = &x->g;

			OUT("group %llu %s flags 0x%x level %s/%s/%s/%s%s demand %u/%u/%u/%u width %u.%u pt %u workms %llu runms %llu moves %u "
			    "frames %u missed %u latmax %u changes %u wi 0x%x busy %u need %u mj %llu\n", x->id, x->name[0] ? x->name : "-", g->flags,
			    level_name[g->level[PC_FG]], level_name[g->level[PC_DF]], level_name[g->level[PC_UT]], level_name[g->level[PC_BG]],
			    g->prime ? " (prime)" : "", g->demand[PC_FG], g->demand[PC_DF], g->demand[PC_UT], g->demand[PC_BG], g->width16 / 16,
			    (g->width16 % 16) * 10 / 16, g->pt, (unsigned long long)us_from_abs(g->total_work) / 1000,
			    (unsigned long long)us_from_abs(g->total_run) / 1000, g->total_migrations, g->total_frames, g->total_missed,
			    g->lat_max_us, g->changes, g->wi_types, g->busy, g->need, (unsigned long long)g->energy / 1000000);
		}
	}
#undef OUT
	kfree_type(struct pc_report, r);
	return strnlen(text, size);
}

static int
sysctl_amp(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	const size_t size = 12288;
	char *text = kalloc_data(size, Z_WAITOK | Z_ZERO);
	int error;

	if (text == NULL) {
		return ENOMEM;
	}
	error = SYSCTL_OUT(req, text, hk_perfctl_report(text, size) + 1);
	kfree_data(text, size);
	return error;
}

SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, amp, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_amp, "A",
    "the Edge scheduler's clusters, migration graph and counts, and the performance controller's groups");

#else /* CONFIG_SCHED_EDGE && HK_SCHED_SELECTABLE */

void
hk_perfctl_init(void)
{
}

void
hk_perfctl_display(__unused bool on)
{
}

void
hk_perfctl_touch(void)
{
}

#endif /* CONFIG_SCHED_EDGE && HK_SCHED_SELECTABLE */
