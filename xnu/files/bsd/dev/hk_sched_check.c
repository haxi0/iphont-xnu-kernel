/*
 * Boot-arg sched_check=1: the scheduler's invariants and the performance controller's placement, checked by the
 * kernel itself a few seconds after every core is up -- where there is no userland to test with (QEMU without a
 * root image, tools/sched-test/qemu.sh) as on the phone. One line each, "sched_check: PASS|FAIL name: detail".
 *
 *  topology    the clusters' cores are disjoint and cover every core, and each core, run on, says the cluster the
 *              topology gave it (ml_get_cluster_number_local(), from its own MPIDR)
 *  bind        a thread bound to each core runs there (thread_bind)
 *  cluster     a thread bound to each cluster (thread_bind_cluster_id), sleeping and waking, never leaves it
 *  rt          a real-time thread (period 5 ms, 1 ms of work) wakes on time with every core busy at default QoS
 *  placement   with the controller: a group of two threads busy at default QoS is moved off E within 1 s and runs
 *              on P; a group of two busy background threads stays on E meanwhile
 *  spill       eight busy threads of one group use every core (spill and steal across the clusters)
 * Each takes well under two seconds; the spinners stop at their end.
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
#include <sys/systm.h>

#if CONFIG_SCHED_EDGE && HK_SCHED_SELECTABLE

extern bool sched_edge_active(void);
extern bool ml_cpu_online(int cpu_id);
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern uint32_t processor_avail_count;
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern kern_return_t thread_policy_set(thread_t thread, thread_policy_flavor_t flavor, thread_policy_t policy_info,
    mach_msg_type_number_t count);
extern void loader_log_line(const char *line);
extern int hk_perfctl_group_level(struct thread_group *tg, int class);
extern bool hk_perfctl_ready(void);

static int passed, failed;

static void
verdict(bool ok, const char *name, const char *format, ...) __printflike(3, 4);

static void
verdict(bool ok, const char *name, const char *format, ...)
{
	char detail[200], line[260];
	va_list list;

	va_start(list, format);
	vsnprintf(detail, sizeof(detail), format, list);
	va_end(list);
	snprintf(line, sizeof(line), "sched_check: %s %s: %s\n", ok ? "PASS" : "FAIL", name, detail);
	ok ? passed++ : failed++;
	printf("%s", line);
	loader_log_line(line);
}

static void
sleep_ms(uint32_t ms)
{
	assert_wait_timeout((event_t)&passed, THREAD_UNINT, ms, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- the spinners: busy threads of a thread group, counting where they run ------------------------------------- */

struct spin_group {
	struct thread_group *tg;
	_Atomic uint32_t running, sampling, stop;
	uint32_t        created;
	_Atomic uint32_t exited;               /* (the group is released only when every created thread has left it) */
	_Atomic uint64_t on_cluster[SCHED_PERFCONTROL_MAX_CLUSTERS];
	_Atomic uint64_t cpus_seen;
};

static void
spinner(void *arg, __unused wait_result_t wr)
{
	struct spin_group *s = arg;

	if (s->tg != NULL) {
		thread_set_work_interval_thread_group(current_thread(), s->tg);
	}
	os_atomic_inc(&s->running, relaxed);
	while (!os_atomic_load(&s->stop, relaxed)) {
		uint64_t until = mach_absolute_time() + 50 * 19;        /* ~50 us of work between samples (19.2 MHz timer: approximate) */
		while (mach_absolute_time() < until) {
			;
		}
		if (os_atomic_load(&s->sampling, relaxed)) {
			boolean_t was = ml_set_interrupts_enabled(FALSE);
			int cpu = cpu_number(), cluster = ml_get_cluster_number_local();
			ml_set_interrupts_enabled(was);
			if (cluster >= 0 && cluster < SCHED_PERFCONTROL_MAX_CLUSTERS) {
				os_atomic_inc(&s->on_cluster[cluster], relaxed);
			}
			os_atomic_or(&s->cpus_seen, 1ULL << cpu, relaxed);
		}
	}
	if (s->tg != NULL) {
		thread_set_work_interval_thread_group(current_thread(), NULL);
	}
	os_atomic_dec(&s->running, relaxed);
	os_atomic_inc(&s->exited, release);
}

static void
spin_start(struct spin_group *s, const char *name, int count, integer_t pri)
{
	bzero(s, sizeof(*s));
	if (name != NULL) {
		s->tg = thread_group_create_and_retain(THREAD_GROUP_FLAGS_DEFAULT);
		thread_group_set_name(s->tg, name);
	}
	for (int i = 0; i < count; i++) {
		thread_t t;

		if (kernel_thread_start_priority(spinner, s, pri, &t) == KERN_SUCCESS) {
			s->created++;
			thread_set_thread_name(t, "sched_check spinner");
			thread_deallocate(t);
		}
	}
}

static void
spin_stop(struct spin_group *s)
{
	os_atomic_store(&s->stop, 1, relaxed);
	while (os_atomic_load(&s->exited, acquire) != s->created) {
		sleep_ms(2);
	}
	if (s->tg != NULL) {
		thread_group_release(s->tg);
		s->tg = NULL;
	}
}

static uint64_t
spin_share(struct spin_group *s, uint32_t type, uint64_t *total)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	uint64_t in = 0;

	*total = 0;
	for (uint32_t c = 0; c < t->num_clusters && c < SCHED_PERFCONTROL_MAX_CLUSTERS; c++) {
		uint64_t n = os_atomic_load(&s->on_cluster[c], relaxed);
		*total += n;
		if (t->clusters[c].cluster_type == type) {
			in += n;
		}
	}
	return *total ? in * 100 / *total : 0;
}

/* ---- the checks ------------------------------------------------------------------------------------------------- */

static void
check_topology(void)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	uint64_t all = 0, seen = 0;
	bool ok = true;
	char why[120] = "";

	for (uint32_t c = 0; c < t->num_clusters; c++) {
		uint64_t m = t->clusters[c].cpu_mask;
		if (m == 0 || (seen & m)) {
			ok = false;
			snprintf(why, sizeof(why), "cluster %u cores 0x%llx overlap or are none", c, m);
		}
		seen |= m;
	}
	for (uint32_t i = 0; i < t->num_cpus; i++) {
		all |= 1ULL << i;
	}
	if (seen != all) {
		ok = false;
		snprintf(why, sizeof(why), "clusters cover 0x%llx of 0x%llx", seen, all);
	}
	for (uint32_t i = 0; i < t->num_cpus && ok; i++) {
		if (!ml_cpu_online((int)i)) {
			continue;
		}
		processor_t was = thread_bind(cpu_to_processor((int)i));
		thread_block(THREAD_CONTINUE_NULL);
		boolean_t ints = ml_set_interrupts_enabled(FALSE);
		int here = cpu_number(), cluster = ml_get_cluster_number_local();
		ml_set_interrupts_enabled(ints);
		thread_bind(was);
		thread_block(THREAD_CONTINUE_NULL);
		if (here != (int)i) {
			ok = false;
			snprintf(why, sizeof(why), "bound to cpu %u, ran on %d", i, here);
			verdict(false, "bind", "%s", why);
		} else if (cluster != (int)t->cpus[i].cluster_id) {
			ok = false;
			snprintf(why, sizeof(why), "cpu %u says cluster %d, the topology %u", i, cluster, t->cpus[i].cluster_id);
		}
	}
	verdict(ok, "topology", "%s", ok ? "clusters disjoint, every core in one, each core's own MPIDR agrees" : why);
	if (ok) {
		verdict(true, "bind", "a thread bound to each of %u cores ran there", processor_avail_count);
	}
}

static void
check_cluster_bind(void)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	bool ok = true;
	char why[120] = "";

	for (uint32_t c = 0; c < t->num_clusters && ok; c++) {
		uint32_t samples = 0;

		if (thread_bind_cluster_id(current_thread(), c, 0) != KERN_SUCCESS) {
			ok = false;
			snprintf(why, sizeof(why), "thread_bind_cluster_id(%u) refused", c);
			break;
		}
		for (int i = 0; i < 40 && ok; i++) {
			sleep_ms(1);
			boolean_t ints = ml_set_interrupts_enabled(FALSE);
			int cpu = cpu_number(), cluster = ml_get_cluster_number_local();
			ml_set_interrupts_enabled(ints);
			samples++;
			if (cluster != (int)c) {
				ok = false;
				snprintf(why, sizeof(why), "bound to cluster %u, woke on cpu %d of cluster %d (sample %u)", c, cpu, cluster, samples);
			}
		}
		thread_bind_cluster_id(current_thread(), 0, THREAD_UNBIND);
	}
	verdict(ok, "cluster", "%s", ok ? "a thread bound to each cluster woke 40 times in it" : why);
}

static _Atomic uint32_t rt_lat_max, rt_lat_over, rt_samples, rt_done;

static void
rt_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t period, work, deadline;
	thread_time_constraint_policy_data_t policy;

	nanoseconds_to_absolutetime(5 * NSEC_PER_MSEC, &period);
	nanoseconds_to_absolutetime(1 * NSEC_PER_MSEC, &work);
	policy.period = (uint32_t)period;
	policy.computation = (uint32_t)work / 2;
	policy.constraint = (uint32_t)work * 2;
	policy.preemptible = TRUE;
	thread_policy_set(current_thread(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	deadline = mach_absolute_time() + period;
	for (int i = 0; i < 100; i++) {
		assert_wait_deadline((event_t)&rt_samples, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
		uint64_t late = mach_absolute_time() - deadline, ns;
		absolutetime_to_nanoseconds(late, &ns);
		uint32_t us = (uint32_t)(ns / NSEC_PER_USEC);
		os_atomic_max(&rt_lat_max, us, relaxed);
		if (us > 2000) {
			os_atomic_inc(&rt_lat_over, relaxed);
		}
		os_atomic_inc(&rt_samples, relaxed);
		uint64_t until = mach_absolute_time() + work / 4;
		while (mach_absolute_time() < until) {
			;
		}
		deadline += period;
	}
	os_atomic_store(&rt_done, 1, release);
}

static void
check_rt(void)
{
	static struct spin_group load;
	thread_t t;

	spin_start(&load, "sched_check rt load", (int)processor_avail_count + 1, 31);
	sleep_ms(50);
	if (kernel_thread_start_priority(rt_thread, NULL, 80, &t) != KERN_SUCCESS) {
		verdict(false, "rt", "no thread");
		spin_stop(&load);
		return;
	}
	thread_deallocate(t);
	for (int i = 0; i < 400 && !os_atomic_load(&rt_done, acquire); i++) {
		sleep_ms(5);
	}
	spin_stop(&load);
	uint32_t n = os_atomic_load(&rt_samples, relaxed), over = os_atomic_load(&rt_lat_over, relaxed);
	verdict(n == 100 && over <= 2, "rt", "%u wakes with every core busy at default QoS: latest %u us, %u over 2 ms", n,
	    os_atomic_load(&rt_lat_max, relaxed), over);
}

static void
check_placement(void)
{
	static struct spin_group df, bg;
	uint64_t total_df, total_bg;

	spin_start(&df, "sched_check df", 2, 31);
	spin_start(&bg, "sched_check bg", 2, 4);
	sleep_ms(1000);
	os_atomic_store(&df.sampling, 1, relaxed);
	os_atomic_store(&bg.sampling, 1, relaxed);
	sleep_ms(500);
	os_atomic_store(&df.sampling, 0, relaxed);
	os_atomic_store(&bg.sampling, 0, relaxed);
	int df_level = hk_perfctl_group_level(df.tg, 1), bg_level = hk_perfctl_group_level(bg.tg, 3);
	uint64_t df_p = spin_share(&df, CLUSTER_TYPE_P, &total_df), bg_e = spin_share(&bg, CLUSTER_TYPE_E, &total_bg);
	spin_stop(&df);
	spin_stop(&bg);
	verdict(df_level >= 1 && df_p >= 80, "placement df", "2 busy default-QoS threads: level %d, %llu%% of %llu samples on P", df_level,
	    df_p, total_df);
	verdict(bg_level == 0 && bg_e >= 80, "placement bg", "2 busy background threads: level %d, %llu%% of %llu samples on E", bg_level,
	    bg_e, total_bg);
}

static void
check_spill(void)
{
	static struct spin_group wide;
	uint64_t total;

	spin_start(&wide, "sched_check wide", (int)processor_avail_count, 31);
	sleep_ms(300);
	os_atomic_store(&wide.sampling, 1, relaxed);
	sleep_ms(500);
	os_atomic_store(&wide.sampling, 0, relaxed);
	uint64_t seen = os_atomic_load(&wide.cpus_seen, relaxed);
	(void)spin_share(&wide, CLUSTER_TYPE_P, &total);
	spin_stop(&wide);
	verdict((uint32_t)__builtin_popcountll(seen) >= processor_avail_count - 1, "spill", "%u busy threads of one group ran on cores 0x%llx",
	    processor_avail_count, seen);
}

static void
run_checks(void)
{
	const ml_topology_info_t *t = ml_get_topology_info();

	passed = failed = 0;
	uint64_t began = now_us();
	check_topology();
	if (sched_edge_active() && t->num_clusters > 1) {
		check_cluster_bind();
	}
	check_rt();
	if (hk_perfctl_ready() && t->num_clusters > 1) {
		check_placement();
	}
	check_spill();
	{
		/* the report kern.hackios.amp gives, built once (under every scheduler and topology it must not fault) */
		extern size_t hk_perfctl_report(char *text, size_t size);
		const size_t size = 12288;
		char *text = kalloc_data(size, Z_WAITOK | Z_ZERO);
		size_t n = text ? hk_perfctl_report(text, size) : 0;
		verdict(n > 0 && strnstr(text, "core 0 domain", size) != NULL, "report", "kern.hackios.amp: %lu bytes", n);
		kfree_data(text, size);
	}
	char line[120];
	snprintf(line, sizeof(line), "sched_check: done in %llu ms: %d passed, %d failed\n", (now_us() - began) / 1000, passed, failed);
	printf("%s", line);
	loader_log_line(line);
}

static void
check_thread(__unused void *arg, __unused wait_result_t wr)
{
	const ml_topology_info_t *t = ml_get_topology_info();

	for (int i = 0; i < 600 && (processor_avail_count < t->num_cpus || (sched_edge_active() && t->num_clusters > 1 && !hk_perfctl_ready())); i++) {
		sleep_ms(100);
	}
	sleep_ms(500);
	run_checks();
}

/*
 * kern.hackios.sched_check: written (root), the checks run now, in the writer's thread (a few seconds); read, the
 * last run's counts: passed << 16 | failed. For a settled system (sched_test check), where the boot's own load does
 * not decide where a busy thread may run.
 */
#include <sys/sysctl.h>
static LCK_GRP_DECLARE(check_grp, "sched_check");
static LCK_MTX_DECLARE(check_lock, &check_grp);

static int
sysctl_sched_check(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = 0, error;

	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error == 0 && req->newptr != USER_ADDR_NULL && value != 0) {
		lck_mtx_lock(&check_lock);
		run_checks();
		lck_mtx_unlock(&check_lock);
	}
	value = passed << 16 | failed;
	return SYSCTL_OUT(req, &value, sizeof(value));
}

SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, sched_check, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_sched_check, "I",
    "the scheduler's own checks: write to run them now, read the last counts (passed << 16 | failed)");

void hk_sched_check_start(void);

void
hk_sched_check_start(void)
{
	uint32_t on = 0;
	thread_t t;

	if (PE_parse_boot_argn("sched_check", &on, sizeof(on)) && on &&
	    kernel_thread_start_priority(check_thread, NULL, 60, &t) == KERN_SUCCESS) {
		thread_set_thread_name(t, "sched_check");
		thread_deallocate(t);
	}
}

#else

void hk_sched_check_start(void);

void
hk_sched_check_start(void)
{
}

#endif /* CONFIG_SCHED_EDGE && HK_SCHED_SELECTABLE */
