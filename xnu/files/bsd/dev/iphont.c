/* Opt-in, local USB diagnostics through the resident loader. No listening network service. */
#include <sys/iphont.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/task_info.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/hk_input.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>

struct iphont_box iphont_box = { .magic = IPHONT_MAGIC, .version = IPHONT_VERSION, .size = sizeof(struct iphont_box) };
extern int kernel_sysctlbyname(const char *, void *, size_t *, void *, size_t);
extern task_t proc_task(proc_t);
extern int iphont_threads(int, char *, unsigned);
extern void ustacks_now(const char *);
static char command[IPHONT_REQUEST], sysbuf[16384];
struct event { uint64_t seq, ns, a, b; uint32_t component, level, code, reserved; };
static struct event events[512], event_copy[512];
static uint64_t event_sequence, event_dropped;
static uint32_t event_lock, event_level = 1, trace_enabled;
/* Interrupt-safe: a diagnostic producer never waits for another CPU or allocates memory. */
void iphont_event(unsigned component, unsigned level, unsigned code, uint64_t a, uint64_t b);
void iphont_event(unsigned component, unsigned level, unsigned code, uint64_t a, uint64_t b)
{
    if (!trace_enabled || level < __atomic_load_n(&event_level, __ATOMIC_RELAXED)) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&event_lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&event_dropped, 1, __ATOMIC_RELAXED); return;
    }
    uint64_t seq = ++event_sequence, ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    events[(seq - 1) % 512] = (struct event){seq, ns, a, b, component, level, code, 0};
    __atomic_store_n(&event_lock, 0, __ATOMIC_RELEASE);
}
static unsigned used;
static int overflow;
static LCK_GRP_DECLARE(iphont_group, "iphont");
static LCK_MTX_DECLARE(iphont_lock, &iphont_group);
static uint32_t user_pending, user_claimed;
static int user_open;
static unsigned selftest_enabled;

static void emit(const char *fmt, ...) __printflike(1, 2);
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(iphont_box.response + used, IPHONT_RESPONSE - used, fmt, ap);
    va_end(ap);
    if (n < 0 || (unsigned)n >= IPHONT_RESPONSE - used) { overflow = 1; return; }
    used += (unsigned)n;
}
static void quote(const char *s)
{
    emit("\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') emit("\\%c", c);
        else if (c < 32 || c >= 127) emit("\\u%04x", c);
        else emit("%c", c);
    }
    emit("\"");
}
static int process(proc_t p, void *arg)
{
    unsigned *count = arg;
    task_t task = proc_task(p);
    char name[64] = "";
    proc_name(proc_pid(p), name, sizeof(name));
    struct task_absolutetime_info times;
    mach_msg_type_number_t n = TASK_ABSOLUTETIME_INFO_COUNT;
    int error = task ? task_info(task, TASK_ABSOLUTETIME_INFO, (task_info_t)&times, &n) : KERN_INVALID_TASK;
    uint64_t ns = 0;
    if (!error) absolutetime_to_nanoseconds(times.total_user + times.total_system, &ns);
    if (*count) emit(",");
    emit("{\"pid\":%d,\"ppid\":%d,\"cpu_ns\":%llu,\"cpu_error\":%d,\"name\":", proc_pid(p), proc_ppid(p), ns, error);
    quote(name); emit("}");
    (*count)++;
    return 0;
}
static void complete(uint32_t sequence, int error)
{
    if (overflow) error = ENOSPC;
    iphont_box.error = error;
    iphont_box.response_len = error ? 0 : used;
    absolutetime_to_nanoseconds(mach_absolute_time(), &iphont_box.device_ns);
    __atomic_store_n(&iphont_box.response_seq, sequence, __ATOMIC_RELEASE);
}
static int execute(void)
{
    if (!strncmp(command, "events ", 7)) {
        uint64_t after = strtouq(command + 7, NULL, 10), newest;
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&event_lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return EAGAIN;
        newest = event_sequence;
        memcpy(event_copy, events, sizeof(events));
        __atomic_store_n(&event_lock, 0, __ATOMIC_RELEASE);
        uint64_t first = newest >= 512 ? newest - 511 : 1, begin = after + 1;
        if (begin < first) begin = first;
        uint64_t end = newest;
        if (end >= begin && end - begin >= 128) end = begin + 127;
        emit("{\"latest\":%llu,\"next\":%llu,\"more\":%s,\"overwritten\":%llu,\"dropped_contention\":%llu,\"events\":[", newest,
            end, end < newest ? "true" : "false", after + 1 < first ? first - after - 1 : 0,
            __atomic_load_n(&event_dropped, __ATOMIC_RELAXED));
        for (uint64_t seq = begin; seq <= end; seq++) {
            struct event *e = &event_copy[(seq - 1) % 512];
            emit("%s{\"seq\":%llu,\"device_ns\":%llu,\"component\":%u,\"level\":%u,\"code\":%u,\"a\":%llu,\"b\":%llu}",
                seq == begin ? "" : ",", e->seq, e->ns, e->component, e->level, e->code, e->a, e->b);
        }
        emit("]}");
    } else if (!strncmp(command, "trace ", 6)) {
        if (!iphont_box.control) return EPERM;
        unsigned level = (unsigned)strtol(command + 6, NULL, 10);
        if (level > 4) return EINVAL;
        __atomic_store_n(&event_level, level, __ATOMIC_RELAXED);
        emit("{\"minimum_level\":%u}", level);
    } else if (!strcmp(command, "hello")) {
        emit("{\"version\":1,\"control\":%s,\"device\":\"XNU\",\"response_limit\":%u,\"userspace\":%s}",
            iphont_box.control ? "true" : "false", IPHONT_RESPONSE, user_open ? "true" : "false");
    } else if (!strcmp(command, "processes")) {
        unsigned count = 0;
        emit("{\"processes\":["); proc_iterate(PROC_ALLPROCLIST, process, &count, NULL, NULL); emit("]}");
    } else if (!strncmp(command, "threads ", 8)) {
        int n = iphont_threads((int)strtol(command + 8, NULL, 10), iphont_box.response, IPHONT_RESPONSE);
        if (n < 0) return -n;
        used = (unsigned)n;
    } else if (!strncmp(command, "sysctl ", 7)) {
        if (strncmp(command + 7, "kern.", 5) && strncmp(command + 7, "hw.", 3) && strncmp(command + 7, "vm.", 3)) return EPERM;
        size_t len = sizeof(sysbuf);
        int error = kernel_sysctlbyname(command + 7, sysbuf, &len, NULL, 0);
        if (error) return error;
        emit("{\"hex\":\"");
        for (size_t i = 0; i < len; i++) emit("%02x", (unsigned char)sysbuf[i]);
        emit("\",\"bytes\":%lu}", len);
    } else if (!strcmp(command, "registry")) {
        int n = iphont_registry(iphont_box.response, IPHONT_RESPONSE);
        if (n < 0) return -n;
        used = (unsigned)n;
    } else if (!strncmp(command, "stacks ", 7)) {
        ustacks_now(command + 7); emit("{\"destination\":\"kernel log\",\"requested\":true}");
    } else if (!strncmp(command, "gesture ", 8)) {
        if (!iphont_box.control) return EPERM;
        unsigned x1, y1, x2, y2, ms, steps, finger;
        if (sscanf(command + 8, "%u %u %u %u %u %u %u", &x1, &y1, &x2, &y2, &ms, &steps, &finger) != 7 ||
            x1 > 65535 || y1 > 65535 || x2 > 65535 || y2 > 65535 || ms > 10000 || steps > 120 || !steps || finger >= 10) return EINVAL;
        uint64_t start = mach_absolute_time(), ns;
        hk_touch_inject(HK_TOUCH_DOWN, finger, x1, y1);
        for (unsigned i = 1; i <= steps; i++) {
            if (ms) delay_for_interval(ms / steps, NSEC_PER_MSEC);
            uint32_t x = (uint32_t)((int64_t)x1 + ((int64_t)x2 - x1) * i / steps);
            uint32_t y = (uint32_t)((int64_t)y1 + ((int64_t)y2 - y1) * i / steps);
            hk_touch_inject(HK_TOUCH_MOVE, finger, x, y);
        }
        hk_touch_inject(HK_TOUCH_UP, finger, x2, y2);
        absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
        iphont_event(2, 0, 10, finger, ns);
        emit("{\"events_queued\":%u,\"elapsed_ns\":%llu}", steps + 2, ns);
    } else if (!strncmp(command, "press ", 6)) {
        if (!iphont_box.control) return EPERM;
        unsigned button, ms;
        if (sscanf(command + 6, "%u %u", &button, &ms) != 2 || button < 1 || button > 4 || ms > 10000) return EINVAL;
        hk_touch_inject(HK_BUTTON_DOWN, button, 0, 0);
        if (ms) delay_for_interval(ms, NSEC_PER_MSEC);
        hk_touch_inject(HK_BUTTON_UP, button, 0, 0);
        emit("{\"events_queued\":2,\"button\":%u}", button);
    } else if (!strncmp(command, "input ", 6)) {
        if (!iphont_box.control) return EPERM;
        unsigned type, finger, x, y;
        if (sscanf(command + 6, "%u %u %u %u", &type, &finger, &x, &y) != 4) return EINVAL;
        /* Feed the normal backboardd queue without changing physical recovery-chord state. */
        if (type < 1 || type > 5 || x > 65535 || y > 65535 ||
            (type <= 3 ? finger >= 10 : finger < 1 || finger > 4)) return EINVAL;
        hk_touch_inject(type, finger, x, y);
        iphont_event(2, 0, type, finger, ((uint64_t)x << 32) | y);
        emit("{\"queued\":true,\"type\":%u,\"finger\":%u,\"x\":%u,\"y\":%u}", type, finger, x, y);
    } else return ENOTSUP;
    return 0;
}
static int agent_open(__unused dev_t dev, __unused int f, __unused int t, __unused proc_t p)
{
    lck_mtx_lock(&iphont_lock);
    int error = user_open ? EBUSY : 0;
    if (!error) user_open = 1;
    lck_mtx_unlock(&iphont_lock);
    return error;
}
static int agent_close(__unused dev_t dev, __unused int f, __unused int t, __unused proc_t p)
{
    lck_mtx_lock(&iphont_lock);
    user_open = 0;
    if (user_pending) { used = 0; complete(user_pending, EPIPE); user_pending = user_claimed = 0; }
    lck_mtx_unlock(&iphont_lock);
    return 0;
}
static int agent_read(__unused dev_t dev, struct uio *uio, __unused int flag)
{
    lck_mtx_lock(&iphont_lock);
    int error = 0;
    while (!user_pending || user_claimed) {
        error = msleep(&user_pending, &iphont_lock, PZERO | PCATCH, "iphont", NULL);
        if (error) break;
    }
    if (!error) {
        unsigned len = (unsigned)strlen(command + 5);
        if (uio_resid(uio) < (user_ssize_t)(len + 4)) error = EMSGSIZE;
        else {
            uint32_t seq = user_pending;
            error = uiomove((caddr_t)&seq, 4, uio);
            if (!error) error = uiomove(command + 5, (int)len, uio);
            if (!error) user_claimed = seq;
        }
    }
    lck_mtx_unlock(&iphont_lock);
    return error;
}
static int agent_write(__unused dev_t dev, struct uio *uio, __unused int flag)
{
    struct iphont_reply header;
    if (uio_resid(uio) < (user_ssize_t)sizeof(header)) return EINVAL;
    int error = uiomove((caddr_t)&header, sizeof(header), uio);
    if (error) return error;
    if (header.version != IPHONT_VERSION || header.length > IPHONT_RESPONSE || uio_resid(uio) != header.length) return EINVAL;
    lck_mtx_lock(&iphont_lock);
    if (header.sequence != user_pending || header.sequence != user_claimed) error = ESTALE;
    else {
        error = uiomove(iphont_box.response, header.length, uio);
        used = header.length;
        if (!error) { complete(header.sequence, header.error); user_pending = user_claimed = 0; }
    }
    lck_mtx_unlock(&iphont_lock);
    return error;
}
static const struct cdevsw agent_cdevsw = {
    .d_open = agent_open, .d_close = agent_close, .d_read = agent_read, .d_write = agent_write,
    .d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
    .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};
static void agent_selftest(void)
{
    const char *probes[] = {"hello", "sysctl kern.osrelease", "processes", "registry", "threads 0", "events 0"};
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        strlcpy(command, probes[i], sizeof(command)); used = 0; overflow = 0;
        int error = execute();
        if (overflow) error = ENOSPC;
        printf("iphont-test: {\"probe\":\"%s\",\"errno\":%d,\"result\":%s}\n", probes[i], error,
            error ? "null" : iphont_box.response);
    }
    if (!iphont_box.control) {
        strlcpy(command, "input 1 0 0 0", sizeof(command)); used = 0; overflow = 0;
        printf("iphont-test: {\"probe\":\"control denied\",\"errno\":%d,\"expected_errno\":%d}\n", execute(), EPERM);
    }
    printf("iphont-test: done\n");
}
static void agent_thread(__unused void *arg, __unused wait_result_t wr)
{
    if (selftest_enabled) agent_selftest();
    for (;;) {
        uint32_t seq = __atomic_load_n(&iphont_box.request_seq, __ATOMIC_ACQUIRE);
        if (seq && seq != __atomic_load_n(&iphont_box.response_seq, __ATOMIC_ACQUIRE)) {
            lck_mtx_lock(&iphont_lock);
            if (!user_pending) {
                used = 0; overflow = 0;
                unsigned len = iphont_box.request_len;
                if (!len || len >= sizeof(command)) complete(seq, EINVAL);
                else {
                    memcpy(command, iphont_box.request, len); command[len] = 0;
                    if (!strncmp(command, "user ", 5)) {
                        if (!iphont_box.control) complete(seq, EPERM);
                        else if (!user_open) complete(seq, ENXIO);
                        else { user_pending = seq; user_claimed = 0; wakeup(&user_pending); }
                    } else complete(seq, execute());
                }
            }
            lck_mtx_unlock(&iphont_lock);
        }
        delay_for_interval(10, NSEC_PER_MSEC);
    }
}
void iphont_trace_init(void)
{
    PE_parse_boot_argn("iphont", &trace_enabled, sizeof(trace_enabled));
}
void iphont_init(void)
{
    unsigned enabled = 0, control = 0;
    PE_parse_boot_argn("iphont", &enabled, sizeof(enabled));
    PE_parse_boot_argn("iphont_control", &control, sizeof(control));
    if (!enabled) return;
    iphont_box.control = control != 0;
    PE_parse_boot_argn("iphont_selftest", &selftest_enabled, sizeof(selftest_enabled));
    int major = cdevsw_add(-1, &agent_cdevsw);
    if (major < 0 || !devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "iphont")) {
        printf("iphont: failed to create root-only userspace bridge\n"); return;
    }
    thread_t thread;
    if (kernel_thread_start(agent_thread, NULL, &thread) != KERN_SUCCESS) {
        printf("iphont: failed to start diagnostic thread\n"); return;
    }
    thread_deallocate(thread);
    __atomic_store_n(&iphont_box.enabled, 1, __ATOMIC_RELEASE);
    iphont_event(0, 1, 1, iphont_box.control, 0);
    printf("iphont: agent v1 ready; control %s\n", control ? "authorized by boot argument" : "disabled");
}
