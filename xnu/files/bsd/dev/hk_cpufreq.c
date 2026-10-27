/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/task.h>
#include <mach/task_info.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hk_input.h>
#include <sys/hk_board.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hk_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern bool ml_cpu_online(int cpu_id);

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hk_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
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
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               hk_board_addr("epss", 0x18591000UL)    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define EPSS_VOLT_TABLE         0x200           /* a word per row: [11:0] the row's voltage, mV (read as Linux reads it) */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/*
 * The group of a processor, by the kernel's number for it: the board's description of its frequency domains
 * (osfmk/arm64/machine_routines.c, board_cpu_domains=; on both phones cores 0-3, 4-6 and 7), which is also
 * how the Edge scheduler's clusters are made.
 */
extern uint32_t ml_cpu_frequency_domain(int cpu_id);

static uint32_t
domain_of(unsigned int cpu)
{
	return ml_cpu_frequency_domain((int)cpu);
}

/* each group's registers and the highest row of its table, once raised: for the thermal service's caps */
static vm_offset_t epss_base[EPSS_DOMAINS];
static int epss_top[EPSS_DOMAINS] = { -1, -1, -1 };
static uint32_t epss_khz[EPSS_DOMAINS][EPSS_TABLE_ROWS];
static uint32_t epss_mv[EPSS_DOMAINS][EPSS_TABLE_ROWS];         /* for the performance controller's energy model */
/*
 * The rows in force: epss_cap is the thermal service's ceiling, epss_row what the governor (below) has asked for
 * under it. Without the governor (cpufreq_gov=0) the group runs at its ceiling, as before there was one.
 */
static int epss_cap[EPSS_DOMAINS], epss_row[EPSS_DOMAINS];
static bool gov_on;

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
		epss_khz[domain][row] = khz;
		epss_mv[domain][row] = *(volatile uint32_t *)(base + EPSS_VOLT_TABLE + 4 * row) & 0xfff;
	}
	epss_base[domain] = base;
	epss_top[domain] = (int)top;
	epss_cap[domain] = epss_row[domain] = (int)top;
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz (%u mV; lowest row %u MHz, %u mV)\n",
	    domain, cores, was, top, top_khz / 1000, epss_mv[domain][top], epss_khz[domain][0] / 1000, epss_mv[domain][0]);
}

/*
 * For the thermal service (bsd/dev/hk_thermal.c): the highest row of a group's table (-1 when the
 * group was not raised -- none of our cores in it, or cpufreq=0), how fast a row is, and a cap: the
 * row the group is to run at, never above its top. One register write, as raising it was; EPSS moves
 * the clock and the voltage by itself.
 */
int
hk_cpufreq_top(uint32_t domain)
{
	return domain < EPSS_DOMAINS ? epss_top[domain] : -1;
}

uint32_t
hk_cpufreq_khz(uint32_t domain, int row)
{
	if (domain >= EPSS_DOMAINS || row < 0 || row > epss_top[domain]) {
		return 0;
	}
	return epss_khz[domain][row];
}


static void
epss_set(uint32_t domain, int row)
{
	if (row > epss_cap[domain]) {
		row = epss_cap[domain];
	}
	if (row < 0) {
		row = 0;
	}
	epss_row[domain] = row;
	*(volatile uint32_t *)(epss_base[domain] + EPSS_PERF_STATE) = (uint32_t)row;
}

void
hk_cpufreq_cap(uint32_t domain, int row)
{
	if (domain >= EPSS_DOMAINS || epss_top[domain] < 0) {
		return;
	}
	if (row < 0) {
		row = 0;
	}
	if (row > epss_top[domain]) {
		row = epss_top[domain];
	}
	epss_cap[domain] = row;
	/* (lower at once; higher only when the governor wants it, or at once without one) */
	epss_set(domain, gov_on && epss_row[domain] < row ? epss_row[domain] : row);
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint32_t threads;               /* how many are to be ready before they start */
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

/*
 * One more thread is ready; the last one starts them all. (Not the caller: with a busy thread of kernel priority on every
 * core, a user thread waiting to say "go" may never get a core again.)
 */
static void
bench_ready(void)
{
	if (os_atomic_inc(&bench.ready, relaxed) == bench.threads) {
		os_atomic_store(&bench.go, 1, release);
	}
}

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	/* one thread on each core, whatever the scheduler would do with seven busy threads (they spin until all are ready) */
	if (ml_cpu_online((int)index)) {
		thread_bind(cpu_to_processor((int)index));
		thread_block(THREAD_CONTINUE_NULL);
	}

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.done, relaxed);
		bench_ready();
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	bench_ready();
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_run(uint64_t total[3])
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */

	bzero(&bench, sizeof(bench));
	bench.threads = threads;
	total[0] = total[1] = total[2] = 0;
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.done, relaxed);
			bench_ready();
		}
	}
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t total[3];

	delay_for_interval(2000, NSEC_PER_MSEC);
	bench_run(total);
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS), total[0], total[1], total[2]);
}

/*
 * kern.hackios.membench: what all the cores together get from memory right now, writing, in MB a second (a read of it
 * runs the test, about a second, on every core of ours at once). Tests read it before a sleep and after the wake: the
 * memory's vote and the cores that were started again show in it. (tools/t: "mem")
 */
static int
sysctl_membench(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static lck_mtx_t *lock;
	static uint64_t last[3];
	uint64_t total[3] = { 0, 0, 0 };

	if (lock == NULL) {
		lock = lck_mtx_alloc_init(lck_grp_alloc_init("hk_membench", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	}
	lck_mtx_lock(lock);
	bench_run(total);
	memcpy(last, total, sizeof(last));
	lck_mtx_unlock(lock);
	tell("membench: now, all together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", total[0], total[1], total[2]);
	return SYSCTL_OUT(req, &total[0], sizeof(total[0]));
}

SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, membench, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_membench, "Q",
    "memory write bandwidth of all cores together, MB/s (runs the test)");

/*
 * kern.hackios.cpuspeed: how fast a core of each group really runs now -- a loop measured on it (loop_speed(), thousands of
 * iterations a millisecond) -- with the row the governor has asked for and the row EPSS says it has. A read of it binds the
 * reading thread to a core of each group in turn; "d0 row 16/16 speed 1234 ..." for groups 0 to 2 (a group none of our cores is
 * in, or whose core is not online, says "-"). Tests read it before a sleep and after the wake (tools/t).
 */
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern bool ml_cpu_online(int cpu_id);

static int
sysctl_cpuspeed(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[240] = "";
	size_t used = 0;
	const ml_topology_info_t *topology = ml_get_topology_info();

	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		int cpu = -1;

		for (unsigned i = 0; i < topology->num_cpus && cpu < 0; i++) {
			if (epss_top[d] >= 0 && domain_of(i) == d && ml_cpu_online((int) i)) {
				cpu = (int) i;
			}
		}
		if (cpu < 0) {
			used += (size_t) snprintf(text + used, sizeof(text) - used, "d%u - ", d);
			continue;
		}
		processor_t was = thread_bind(cpu_to_processor(cpu));
		uint32_t speed;

		thread_block(THREAD_CONTINUE_NULL);
		speed = loop_speed();
		thread_bind(was);
		thread_block(THREAD_CONTINUE_NULL);
		used += (size_t) snprintf(text + used, sizeof(text) - used, "d%u asked %d hw %u of %d speed %u ", d, epss_row[d],
		    *(volatile uint32_t *)(epss_base[d] + EPSS_PERF_STATE), epss_top[d], speed);
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_kern_hackios, OID_AUTO, cpuspeed, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_cpuspeed, "A",
    "each group's row asked for, EPSS's row, and a loop measured on one of its cores");

/*
 * kern.hackios.top: the processes that used the most processor time since the last read (user plus system, all cores added:
 * "name[pid] ms" for the first eight), what a profile of a gesture needs and ps(1) does not show here. The first read only sets
 * the starting point.
 */
#define TOP_PROCS       512
static struct top_entry {
	int pid;
	uint64_t ns, system_ns;
	char name[20];
} top_now[TOP_PROCS], top_before[TOP_PROCS];
static unsigned top_count, top_before_count;

extern task_t proc_task(proc_t);
extern void proc_iterate(unsigned int flags, int (*callout)(proc_t, void *), void *arg, int (*filterfn)(proc_t, void *),
    void *filterarg);

static int
top_visit(proc_t p, __unused void *arg)
{
	task_t task = proc_task(p);
	struct task_absolutetime_info info;
	mach_msg_type_number_t count = TASK_ABSOLUTETIME_INFO_COUNT;
	uint64_t ns;

	if (task == TASK_NULL || top_count == TOP_PROCS ||
	    task_info(task, TASK_ABSOLUTETIME_INFO, (task_info_t) &info, &count) != KERN_SUCCESS) {
		return 0;
	}
	absolutetime_to_nanoseconds(info.total_user + info.total_system, &ns);
	top_now[top_count].pid = proc_pid(p);
	top_now[top_count].ns = ns;
	absolutetime_to_nanoseconds(info.total_system, &top_now[top_count].system_ns);
	proc_name(proc_pid(p), top_now[top_count].name, sizeof(top_now[0].name));
	top_count++;
	return 0;
}

static int
sysctl_top(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static lck_mtx_t *lock;
	char text[480] = "";
	size_t used = 0;
	struct { int index; uint64_t ms, system_ms; } best[8];
	unsigned found = 0;

	if (lock == NULL) {
		lock = lck_mtx_alloc_init(lck_grp_alloc_init("hk_top", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	}
	lck_mtx_lock(lock);
	top_count = 0;
	proc_iterate(1 /* PROC_ALLPROCLIST */, top_visit, NULL, NULL, NULL);
	for (unsigned i = 0; i < top_count; i++) {
		uint64_t was = 0, was_system = 0, ms;

		for (unsigned k = 0; k < top_before_count; k++) {
			if (top_before[k].pid == top_now[i].pid) {
				was = top_before[k].ns;
				was_system = top_before[k].system_ns;
				break;
			}
		}
		if (top_before_count == 0) {
			break;
		}
		ms = (top_now[i].ns - MIN(was, top_now[i].ns)) / NSEC_PER_MSEC;
		unsigned at = found;

		while (at > 0 && best[at - 1].ms < ms) {
			at--;
		}
		if (at < 8) {
			for (unsigned m = MIN(found, 7); m > at; m--) {
				best[m] = best[m - 1];
			}
			best[at].index = (int) i;
			best[at].ms = ms;
			best[at].system_ms = (top_now[i].system_ns - MIN(was_system, top_now[i].system_ns)) / NSEC_PER_MSEC;
			found = MIN(found + 1, 8);
		}
	}
	for (unsigned i = 0; i < found; i++) {
		used += (size_t) snprintf(text + used, sizeof(text) - used, "%s[%d] %llu(sys %llu) ", top_now[best[i].index].name,
		    top_now[best[i].index].pid, best[i].ms, best[i].system_ms);
	}
	memcpy(top_before, top_now, sizeof(top_now[0]) * top_count);
	top_before_count = top_count;
	lck_mtx_unlock(lock);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_kern_hackios, OID_AUTO, top, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_top, "A",
    "processor time by process since the last read, in ms: the first eight");

/*
 * kern.hackios.kprof: where the processors were in the kernel when the clock interrupted them (osfmk/arm/rtclock.c fills the
 * samples, up to 65536). Write 1: clear and start; write 0: stop. Read: the places the samples fell in, 32 bytes each, as
 * "address:count" (the kernel's own unslid address: `nm -n mach.development.bcm2837.unstripped` names it; tools/kprof.py), the 40 most
 * often hit, and how many samples were in a program (0) and in all. A read stops the sampling.
 */
#define HK_KPROF_SAMPLES 65536
extern volatile uint32_t hk_kprof_on;
extern uint32_t hk_kprof_next;
extern uint64_t hk_kprof_pcs[HK_KPROF_SAMPLES];

static int
sysctl_kprof_ctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = 0, error = SYSCTL_IN(req, &value, sizeof(value));

	if (error == 0 && req->newptr) {
		if (value) {
			hk_kprof_next = 0;
		}
		hk_kprof_on = value != 0;
	}
	return error;
}

SYSCTL_PROC(_kern_hackios, OID_AUTO, kprof_ctl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_kprof_ctl, "I",
    "1: start sampling where the processors are in the kernel; 0: stop");

static int
sysctl_kprof(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static struct { uint64_t bucket; uint32_t count; } places[2048];
	char *text = kalloc_data(2048, Z_WAITOK | Z_ZERO);
	unsigned used = 0, total = MIN(hk_kprof_next, (uint32_t) HK_KPROF_SAMPLES), program = 0;
	size_t length = 0;
	int error;

	hk_kprof_on = 0;
	if (text == NULL) {
		return ENOMEM;
	}
	bzero(places, sizeof(places));
	for (unsigned i = 0; i < total; i++) {
		uint64_t bucket = hk_kprof_pcs[i] >> 5;
		unsigned k = (unsigned)((bucket * 0x9e3779b97f4a7c15ull) >> 53) % 2048, tries = 0;

		if (hk_kprof_pcs[i] == 0) {
			program++;
			continue;
		}
		while (places[k].count != 0 && places[k].bucket != bucket && tries++ < 2048) {
			k = (k + 1) % 2048;
		}
		if (tries < 2048) {
			places[k].bucket = bucket;
			places[k].count++;
			used += places[k].count == 1;
		}
	}
	length += (size_t) snprintf(text, 2048, "%u in programs of %u:", program, total);
	for (unsigned n = 0; n < 40; n++) {
		unsigned best = 0;

		for (unsigned k = 1; k < 2048; k++) {
			best = places[k].count > places[best].count ? k : best;
		}
		if (places[best].count == 0 || length + 24 > 2048) {
			break;
		}
		length += (size_t) snprintf(text + length, 2048 - length, " %llx:%u", places[best].bucket << 5, places[best].count);
		places[best].count = 0;
	}
	error = SYSCTL_OUT(req, text, length + 1);
	kfree_data(text, 2048);
	(void) used;
	return error;
}

SYSCTL_PROC(_kern_hackios, OID_AUTO, kprof, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_kprof, "A",
    "the places in the kernel the clock interrupts found a processor at, 32 bytes each (see kprof_ctl)");

/*
 * kern.hackios.memlat: how long a dependent load takes from a working set that fits the core's L2 (256 KB), the shared L3
 * (3 MB) and only memory (96 MB), in tenths of a nanosecond, measured on the core the reader happens to be on: a chain of
 * loads each of which gives the address of the next, a cache line apart, in an order a prefetcher cannot follow. The cores'
 * speed, the L3's and the memory's all show in it, and a vote that was lost across a sleep shows as a longer time.
 */
static uint32_t
chase(vm_size_t size)
{
	vm_offset_t memory = 0;
	uint64_t *table, began, nanos;
	uint64_t lines = size / 64, at = 0, loads = 3000000;
	uint32_t result = 0;

	if (kmem_alloc(kernel_map, &memory, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		return 0;
	}
	table = (uint64_t *) memory;
	/* a cycle through all the lines in a fixed pseudo-random order: line i leads to (a * i + c) mod lines, a odd and prime to lines */
	for (uint64_t i = 0; i < lines; i++) {
		table[i * 8] = ((i * 2654435761ull + 40503) % lines);
	}
	/* (that is not always one cycle: whichever it is, it is long enough for the loads below to leave the caches) */
	for (uint64_t i = 0; i < lines; i++) {
		(void) table[at * 8];
		at = table[at * 8];
	}
	began = mach_absolute_time();
	for (uint64_t i = 0; i < loads; i++) {
		at = table[at * 8];
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &nanos);
	result = (uint32_t)(nanos * 10 / loads + (at == ~0ull));
	kmem_free(kernel_map, memory, size);
	return result;
}

static int
sysctl_memlat(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[96];

	snprintf(text, sizeof(text), "l2 %u l3 %u dram %u", chase(256u << 10), chase(3u << 20), chase(96u << 20));
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_kern_hackios, OID_AUTO, memlat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_memlat, "A",
    "dependent-load latency from 256 KB, 3 MB and 96 MB, in tenths of a nanosecond (runs the test)");

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		hk_tl("%s\n", line);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             hk_board_addr("cmd_db", 0x80860000UL)
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

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
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			if (entry[i].length >= sizeof(aux) && slave->data_offset + entry[i].offset + sizeof(aux) < CMD_DB_SIZE - 0x40) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                hk_board_addr("rsc", 0x18220000UL)
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (hk_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hk_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
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

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as bsd/dev/hk_goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done = false;

	/* (shared with the other drivers that write slot set 0: sys/hk_board.h) */
	if (!hk_rpmh_claim(ml_get_interrupts_enabled())) {
		tell("ddr: RPMh: another driver holds the command slots\n");
		return false;
	}
	irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			hk_rpmh_release();
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	hk_rpmh_release();
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void hk_ddr_quiesce(void);

void
hk_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

/*
 * The governor (boot-arg cpufreq_gov, 1 by default with cpufreq=1; 0: every group at its top row for the whole run,
 * as before). The top rows for all of the run kept every core at its highest voltage even in WFI, the screen off
 * included. Now each group's row follows the busiest of its cores (ml_cpu_idle_time()), as Linux's schedutil
 * sets it: the next speed is 1.25 times the present one times the busy share -- twice it from 90% busy --, the
 * lowest row that is at least that; up at once, down GOV_DOWN_ROWS rows a tick at most. A touch or a key
 * (hk_cpufreq_boost(), from hk_touch_post()) puts every group at its top for GOV_BOOST_MS, as the scroll that
 * follows needs it before any load could show it. Never above the thermal service's ceiling (hk_cpufreq_cap()).
 * The top rows until GOV_START_SECONDS after the start: the boot is as fast as it was.
 *
 * The memory's bandwidth (ddr=2, the default) follows too: the most there is (ddr_vote=) while the screen is
 * lit and something is busy -- a core over half busy or over half its rows, a touch, the GPU above its lowest
 * speed -- and for GOV_DDR_IDLE_MS after; ddr_idle= (default 1024, the firmware's own middle range) when lit and
 * idle, ddr_dark= (default 1024) with the screen off. Both from this one thread; the RPMh slot set is shared with other drivers through hk_rpmh_claim().
 */
#define GOV_TICK_MS             20
#define GOV_TICK_DARK_MS        250
#define GOV_START_SECONDS       45      /* the lock screen comes at 26 s, the slower cores at 28 s (boot-arg gov_start=N) */
#define GOV_BOOST_MS            150
#define GOV_DOWN_ROWS           2
#define GOV_DDR_IDLE_MS         1000
#define DDR_IDLE                1024

extern bool ml_cpu_online(int cpu_id);
extern int hz;                          /* (bsd/kern/kern_clock.c: the ticks a second tsleep() counts in) */
extern uint32_t hk_gpu_speed_level(void);      /* iokit/Kernel/HKGPU.cpp: 0 or 1 is idle */
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);              /* (osfmk/kern/thread.h, private to osfmk's headers) */
extern void thread_set_thread_name(thread_t thread, const char *name);

static uint64_t gov_boost_until;
static bool gov_dark;
/*
 * From the performance controller (bsd/dev/hk_perfctl.c), which knows what runs where: the lowest speed each group
 * is to have for the work it was given (kHz, 0: none), and which groups a touch is to raise (by default all: the
 * controller narrows it to the clusters the interactive work is on).
 */
static uint32_t gov_floor_khz[EPSS_DOMAINS];
static uint32_t gov_boost_domains = (1u << EPSS_DOMAINS) - 1;
static uint32_t gov_ddr_high = 0x3fff, gov_ddr_idle = DDR_IDLE, gov_ddr_dark = DDR_IDLE, gov_ddr_now;
static uint64_t gov_ticks, gov_row_sum[EPSS_DOMAINS], gov_ddr_changes;
static int gov_event;

/* A touch or a key: every group to its top at once (any context: one register write a group) */
void
hk_cpufreq_boost(void)
{
	uint64_t until;

	hk_perfctl_touch();
	if (!gov_on) {
		return;
	}
	clock_interval_to_deadline(GOV_BOOST_MS, NSEC_PER_MSEC, &until);
	os_atomic_store(&gov_boost_until, until, relaxed);
	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		if (!(os_atomic_load(&gov_boost_domains, relaxed) & (1u << d))) {
			continue;
		}
		if (epss_top[d] >= 0 && epss_row[d] < epss_cap[d]) {
			epss_set(d, epss_cap[d]);
		}
	}
}

/* The lowest row of a group at or above khz (the top if none is) */
static int
row_for_khz(uint32_t d, uint32_t khz)
{
	int row = 0;

	while (row < epss_top[d] && epss_khz[d][row] < khz) {
		row++;
	}
	return row;
}

/*
 * The performance controller's floor for a group (see gov_floor_khz). With now, a floor above the group's row
 * raises it at once (one register write), as a touch does: a frame that is late cannot wait for the next tick.
 */
void
hk_cpufreq_floor(uint32_t domain, uint32_t khz, bool now)
{
	if (domain >= EPSS_DOMAINS || epss_top[domain] < 0) {
		return;
	}
	os_atomic_store(&gov_floor_khz[domain], khz, relaxed);
	if (now && os_atomic_load(&gov_on, acquire)) {
		int row = row_for_khz(domain, khz);

		if (row > epss_row[domain]) {
			epss_set(domain, row);
		}
	}
}

void
hk_cpufreq_boost_domains(uint32_t mask)
{
	os_atomic_store(&gov_boost_domains, mask, relaxed);
}

/* What a group runs at now and at most (kHz; 0: not ours, or cpufreq=0) */
uint32_t
hk_cpufreq_cur_khz(uint32_t domain)
{
	if (domain >= EPSS_DOMAINS || epss_top[domain] < 0) {
		return 0;
	}
	return epss_khz[domain][epss_row[domain]];
}

/* The row a group was asked for and a row's voltage (mV, 0 if not known) */
int
hk_cpufreq_row(uint32_t domain)
{
	return domain < EPSS_DOMAINS && epss_top[domain] >= 0 ? epss_row[domain] : -1;
}

uint32_t
hk_cpufreq_mv(uint32_t domain, int row)
{
	if (domain >= EPSS_DOMAINS || row < 0 || row > epss_top[domain]) {
		return 0;
	}
	return epss_mv[domain][row];
}

uint32_t
hk_cpufreq_max_khz(uint32_t domain)
{
	if (domain >= EPSS_DOMAINS || epss_top[domain] < 0) {
		return 0;
	}
	return epss_khz[domain][epss_top[domain]];
}

/* The screen off or on (bsd/dev/hk_power.c): the memory's vote changes on the governor's thread, at once */
void
hk_cpufreq_display(bool on)
{
	os_atomic_store(&gov_dark, !on, relaxed);
	hk_perfctl_display(on);
	if (on) {
		hk_cpufreq_boost();
	}
	wakeup(&gov_event);
}

/*
 * The vote, from the governor's thread or from the system's sleep (hk_cpufreq_sleep()): never both at once in the
 * one RPMh slot set -- the governor may have been stopped in the middle of a write when the system went to sleep.
 * 0: no request of ours at all (the memory at its floor).
 */
static uint32_t ddr_busy;

static bool
gov_ddr(uint32_t vote)
{
	bool done;

	if (!ddr_voted || vote == gov_ddr_now) {
		return true;
	}
	if (!os_atomic_cmpxchg(&ddr_busy, 0, 1, acquire)) {
		return false;
	}
	done = vote != 0 ? ddr_vote(vote) :
	    ddr_rpmh_write((uint32_t[]){ ddr_mc0, ddr_sh0 }, (uint32_t[]){ BCM_COMMIT, BCM_COMMIT }, 2);
	if (done) {
		gov_ddr_now = vote;
		gov_ddr_changes++;
	}
	os_atomic_store(&ddr_busy, 0, release);
	return done;
}

/*
 * The system's sleep (bsd/dev/hk_power.c), from its thread before the processor's interrupts are masked, and after
 * they are back: every group at its lowest row and the memory's vote at ddr_sleep= (default 0, no request: the
 * memory at its floor) for the sleep; on the way out the memory's vote as for a busy screen and every group at its
 * top for a moment, the governor taking over from its next tick. Without the governor (cpufreq_gov=0) the rows
 * go back to the thermal service's ceiling.
 */
static uint32_t gov_ddr_sleep;

void
hk_cpufreq_sleep(bool asleep)
{
	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		if (epss_top[d] >= 0) {
			epss_set(d, asleep ? 0 : epss_cap[d]);
		}
	}
	if (!asleep) {
		hk_cpufreq_boost();
	}
	if (!gov_ddr(asleep ? gov_ddr_sleep : gov_ddr_high)) {
		tell("cpufreq: the memory's vote is being written by the governor: left as it is for the %s\n",
		    asleep ? "sleep" : "wake");
	}
}

static void
gov_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then, busy_at;

	{
		uint32_t start = GOV_START_SECONDS;

		PE_parse_boot_argn("gov_start", &start, sizeof(start));     /* tests: the steady state is what they measure, sooner */
		delay_for_interval(start * 1000, NSEC_PER_MSEC);
	}
	then = busy_at = mach_absolute_time();
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		ml_cpu_idle_time((int)i, &idle_before[i]);
	}
	os_atomic_store(&gov_on, true, release);
	tell("cpufreq: the governor takes over: rows follow the load (ddr %u busy, %u idle, %u screen off)\n",
	    gov_ddr_high, gov_ddr_idle, gov_ddr_dark);
	for (;;) {
		uint32_t busy[EPSS_DOMAINS] = { 0 };
		bool dark = os_atomic_load(&gov_dark, relaxed), boosted, active = false;
		uint64_t now, span;

		tsleep(&gov_event, PZERO, "cpufreq", MAX(1, (dark ? GOV_TICK_DARK_MS : GOV_TICK_MS) * hz / 1000));
		now = mach_absolute_time();
		span = now - then;
		then = now;
		if (span == 0) {
			continue;
		}
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, spent;
			uint32_t d = domain_of(i);

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			spent = idle - idle_before[i];
			idle_before[i] = idle;
			if (!ml_cpu_online((int)i) || d >= EPSS_DOMAINS) {
				continue;               /* (switched off, or not started yet: no load of its own) */
			}
			busy[d] = MAX(busy[d], (uint32_t)(span > spent ? (span - spent) * 100 / span : 0));
		}
		boosted = now < os_atomic_load(&gov_boost_until, relaxed);
		for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
			uint64_t want;
			int target = 0;

			if (epss_top[d] < 0) {
				continue;
			}
			want = (uint64_t)epss_khz[d][epss_row[d]] * busy[d] * (busy[d] >= 90 ? 200 : 125) / 10000;
			while (target < epss_top[d] && epss_khz[d][target] < want) {
				target++;
			}
			if (boosted && (os_atomic_load(&gov_boost_domains, relaxed) & (1u << d))) {
				target = epss_top[d];
			}
			/* (the controller sets floors with the screen off only for work with deadlines: audio, say) */
			target = MAX(target, row_for_khz(d, os_atomic_load(&gov_floor_khz[d], relaxed)));
			if (target < epss_row[d] - GOV_DOWN_ROWS) {
				target = epss_row[d] - GOV_DOWN_ROWS;
			}
			if (target != epss_row[d]) {
				epss_set(d, target);
			}
			active |= busy[d] >= 50 || epss_row[d] > epss_top[d] / 2;
			gov_row_sum[d] += (uint64_t)epss_row[d];
		}
		active |= boosted || hk_gpu_speed_level() > 1;
		if (active) {
			busy_at = now;
		}
		if (dark) {
			gov_ddr(gov_ddr_dark);
		} else {
			uint64_t ms;

			absolutetime_to_nanoseconds(now - busy_at, &ms);
			gov_ddr(ms / NSEC_PER_MSEC < GOV_DDR_IDLE_MS ? gov_ddr_high : gov_ddr_idle);
		}
		if (++gov_ticks % (60000 / GOV_TICK_MS) == 0) {
			tell("cpufreq: over the last %llu ticks the groups ran at rows %llu/%d %llu/%d %llu/%d on average; memory vote %u "
			    "(%llu changes)\n", (uint64_t)(60000 / GOV_TICK_MS), gov_row_sum[0] / (60000 / GOV_TICK_MS), epss_top[0],
			    gov_row_sum[1] / (60000 / GOV_TICK_MS), epss_top[1], gov_row_sum[2] / (60000 / GOV_TICK_MS), epss_top[2],
			    gov_ddr_now, gov_ddr_changes);
			bzero(gov_row_sum, sizeof(gov_row_sum));
		}
	}
}

static void
gov_start(void)
{
	uint32_t on = 1;
	thread_t thread;

	PE_parse_boot_argn("cpufreq_gov", &on, sizeof(on));
	PE_parse_boot_argn("ddr_vote", &gov_ddr_high, sizeof(gov_ddr_high));
	PE_parse_boot_argn("ddr_idle", &gov_ddr_idle, sizeof(gov_ddr_idle));
	PE_parse_boot_argn("ddr_dark", &gov_ddr_dark, sizeof(gov_ddr_dark));
	PE_parse_boot_argn("ddr_sleep", &gov_ddr_sleep, sizeof(gov_ddr_sleep));
	gov_ddr_sleep = MIN(gov_ddr_sleep, 0x3fff);
	gov_ddr_now = ddr_voted ? gov_ddr_high : 0;     /* (what ddr_raise() voted, from the same ddr_vote=) */
	gov_ddr_high = MIN(gov_ddr_high, 0x3fff);
	gov_ddr_idle = MIN(gov_ddr_idle, gov_ddr_high);
	gov_ddr_dark = MIN(gov_ddr_dark, gov_ddr_high);
	if (!on) {
		return;
	}
	if (kernel_thread_start_priority(gov_thread, NULL, 93, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "cpufreq governor");
		thread_deallocate(thread);
	}
}

void
hk_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		if (domain_of(i) < EPSS_DOMAINS) {
			cores[domain_of(i)]++;
		}
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	gov_start();
	if (PE_parse_boot_argn("membench", &enabled, sizeof(enabled)) && enabled == 1) {
		memory_bench();
	} else if (enabled == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
