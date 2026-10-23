#!/usr/bin/env python3
"""Run the production touch FIFO, wakeup and bus-lock code with host-side hardware stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "xnu-src/xnu-8796.121.2"


def function(source, name):
    start = source.index("\n" + name + "(") + 1
    opened = source.index("{", start)
    depth = 1
    end = opened + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


touch = (SRC / "bsd/dev/instantnoodle_touch.c").read_text()
i2c = (SRC / "bsd/dev/instantnoodle_i2c.c").read_text()
code = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
'''
code += '#include "' + str(SRC / "bsd/sys/instantnoodle.h") + '"\n'
code += r'''
#define MAX_FINGERS 10
#define EVENT_COUNT 32
#define EVENT_LEN 8
#define CMD_READ_EVENT 0x60
#define CMD_READ_ALL 0x61
#define PANEL_W 1080
#define PANEL_H 2400
#define REDWOOD_TOUCH_DOWN 1
#define REDWOOD_TOUCH_MOVE 2
#define REDWOOD_TOUCH_UP 3
static struct { uint32_t x, y; bool down; } finger[MAX_FINGERS];
static unsigned events, posted, reads, left_events;
static int read_error;
static uint8_t packet[8];
static void redwood_touch_post(unsigned kind, unsigned id, unsigned x, unsigned y) {
    assert(kind >= 1 && kind <= 3 && id < 10 && x <= 65535 && y <= 65535);
    posted++;
}
static int ts_read(uint8_t reg, uint8_t *buf, unsigned n) {
    reads++;
    if (reg == CMD_READ_EVENT) {
        assert(n == 8);
        memcpy(buf, packet, 8);
        buf[7] = left_events;
        return read_error == 1 ? EIO : 0;
    }
    assert(reg == CMD_READ_ALL && n == left_events * 8);
    if (read_error == 2) return EIO;
    for (unsigned i = 0; i < n; i += 8) memcpy(buf + i, packet, 8);
    return 0;
}
'''
code += "static uint32_t\n" + function(touch, "scale") + "\n"
code += "static void\n" + function(touch, "handle_event") + "\n"
code += "static int\n" + function(touch, "poll_events") + "\n"
code += r'''
typedef void *event_t;
#define THREAD_UNINT 0
#define THREAD_AWAKENED 0
#define THREAD_CONTINUE_NULL NULL
#define NSEC_PER_MSEC 1000000
#define PIN_IRQ ((struct instantnoodle_pin){0, 39})
static uint64_t irq_time;
static int wake_mode, blocks, clears;
static bool line_high = true;
bool instantnoodle_gpio_level(struct instantnoodle_pin pin) { return line_high; }
static void clock_interval_to_deadline(unsigned n, unsigned unit, uint64_t *d) { *d = n * unit; }
static void assert_wait_deadline(event_t e, int flags, uint64_t d) {
    assert(d == 100000000);
    if (wake_mode == 1) irq_time = 123;
}
static void *current_thread(void) { return NULL; }
static void clear_wait(void *t, int result) { clears++; }
static void thread_block(void *continuation) { blocks++; irq_time = 456; }
'''
code += "static uint64_t\n" + function(touch, "wait_report") + "\n"
code += r'''
struct _lck_mtx_ { pthread_mutex_t mutex; };
typedef struct _lck_mtx_ lck_mtx_t;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static int i2c_lck_grp;
#define LCK_ATTR_NULL NULL
#define panic(...) abort()
static void lck_mtx_lock(void *p) { assert(pthread_mutex_lock(p) == 0); }
static void lck_mtx_unlock(void *p) { assert(pthread_mutex_unlock(p) == 0); }
static lck_mtx_t *lck_mtx_alloc_init(void *group, void *attr) {
    lck_mtx_t *lock = calloc(1, sizeof(*lock));
    assert(lock && pthread_mutex_init(&lock->mutex, NULL) == 0);
    return lock;
}
static void PE_parse_boot_argn(const char *key, uint32_t *v, size_t n) { *v = 0; }
'''
code += "static void\n" + function(i2c, "bus_lock") + "\n"
code += r'''
static struct instantnoodle_i2c gauge = { .name = "se16" }, screen = { .name = "se13" };
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t condition_lock = PTHREAD_MUTEX_INITIALIZER;
static bool touch_finished;
static void *touch_transfer(void *arg) {
    bus_lock(&screen);
    lck_mtx_unlock(screen.lock);
    pthread_mutex_lock(&condition_lock);
    touch_finished = true;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&condition_lock);
    return NULL;
}
int main(void) {
    /* 32 records are independent of the ten contact slots. */
    packet[0] = 0x44;  /* finger 1 down */
    packet[1] = 30; packet[2] = 40;
    left_events = 31;
    assert(poll_events() == 0 && reads == 2 && events == 32 && posted == 32);
    assert(finger[0].down);
    packet[0] = 0xc4; left_events = 0;
    assert(poll_events() == 0 && !finger[0].down && events == 33);
    unsigned before = posted;
    left_events = 32;
    assert(poll_events() == EOVERFLOW && posted == before);
    left_events = 1; read_error = 2;
    assert(poll_events() == EIO && posted == before);
    read_error = 1;
    assert(poll_events() == EIO && posted == before);
    read_error = 0; packet[0] = 0;
    assert(poll_events() == 0 && posted == before);
    packet[0] = 0x6c; left_events = 0; /* invalid finger 11 */
    assert(poll_events() == 0 && posted == before);
    /* IRQ before sleep, during sleep, and an already-low line without an IRQ. */
    irq_time = 42;
    assert(wait_report() == 42 && blocks == 0);
    wake_mode = 1;
    assert(wait_report() == 123 && blocks == 0);
    wake_mode = 0;
    assert(wait_report() == 456 && blocks == 1);
    line_high = false;
    assert(wait_report() == 0 && blocks == 1 && clears == 4);
    /* Hold the gauge bus. Touch must finish before the gauge lock is released. */
    bus_lock(&gauge);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, touch_transfer, NULL) == 0);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&condition_lock);
    while (!touch_finished) {
        assert(pthread_cond_timedwait(&condition, &condition_lock, &deadline) == 0);
    }
    pthread_mutex_unlock(&condition_lock);
    lck_mtx_unlock(gauge.lock);
    pthread_join(worker, NULL);
    assert(gauge.lock != screen.lock);
    puts("PASS: touch FIFO, read failures, IRQ sleep races, independent I2C buses");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="instantnoodle-input-") as temp:
    source = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-pthread", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
