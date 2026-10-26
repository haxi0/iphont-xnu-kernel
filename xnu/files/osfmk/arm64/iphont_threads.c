#include <kern/task.h>
#include <kern/thread.h>
#include <arm/thread.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <string.h>
#include <stdarg.h>
struct proc;
extern struct proc *proc_find(int);
extern void proc_rele(struct proc *);
#include <sys/errno.h>
extern task_t proc_task(struct proc *);
extern void bsd_getthreadname(void *, char *);
extern int vsnprintf(char *, size_t, const char *, va_list);
static unsigned used, limit;
static int overflow;
static char *out;
static void emit(const char *fmt, ...) __printflike(1, 2);
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + used, limit - used, fmt, ap);
    va_end(ap);
    if (n < 0 || (unsigned)n >= limit - used) { overflow = 1; return; }
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
int iphont_threads(int pid, char *buffer, unsigned capacity)
{
    used = 0; overflow = 0; out = buffer; limit = capacity;
    struct proc *p = proc_find(pid);
    if (!p) return -ESRCH;
    task_t task = proc_task(p);
    if (!task) { proc_rele(p); return -ESRCH; }
    unsigned count = 0;
    emit("{\"pid\":%d,\"threads\":[", pid);
    task_lock(task);
    thread_t t;
    queue_iterate(&task->threads, t, thread_t, task_threads) {
        if (count++) emit(",");
        char name[64] = "";
        if (get_bsdthread_info(t)) bsd_getthreadname(get_bsdthread_info(t), name);
        name[63] = 0;
        emit("{\"id\":%llu,\"state\":%d,\"name\":", thread_tid(t), t->state); quote(name);
        const arm_saved_state_t *state = get_user_regs(t);
        if (state && is_saved_state64(state))
            emit(",\"pc\":\"0x%llx\",\"lr\":\"0x%llx\",\"sp\":\"0x%llx\",\"fp\":\"0x%llx\"",
                get_saved_state_pc(state), get_saved_state_lr(state), get_saved_state_sp(state), get_saved_state_fp(state));
        emit("}");
    }
    task_unlock(task);
    proc_rele(p);
    emit("],\"note\":\"registers at last kernel entry; running threads are not suspended\"}");
    return overflow ? -ENOSPC : (int)used;
}
