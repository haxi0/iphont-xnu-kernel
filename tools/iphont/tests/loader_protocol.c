/* Host-only protocol fixture. This does not emulate or validate any phone hardware. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../xnu/files/bsd/sys/iphont.h"
struct kernel_live { uint64_t magic4, log_total, agent, log_head, log_ring, log; };
static struct kernel_live *running_live;
static uint64_t running_slide;
static const char *staged_data;
static uint64_t staged_len;
static char reply_code[5], reply_text[160];
static int starts_with(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static size_t str_len(const char *s) { return strlen(s); }
static void fb_reply(const char *code, const char *message) { snprintf(reply_code, sizeof(reply_code), "%s", code); snprintf(reply_text, sizeof(reply_text), "%s", message); }
#include "../../../xnuloader/iphont.c"
static struct iphont_box box = {.magic=IPHONT_MAGIC, .version=1, .size=sizeof(box), .enabled=1};
static void command(const char *s, const char *expected) { iphont_command(s); assert(!strcmp(reply_code, expected)); }
int main(void)
{
    assert(sizeof(struct iphont_reply) == 24);
    command("oem iphont-put 0 68656c6c6f", "FAIL");
    struct kernel_live live = {.magic4=UINT64_C(0x344556494c554e58), .agent=(uintptr_t)&box}; running_live = &live;
    command("oem iphont-put 0 68656c6c6f", "OKAY");
    command("oem iphont-put 9 61", "FAIL");
    command("oem iphont-put 5 gg", "FAIL");
    command("oem iphont-go 2a 5", "OKAY");
    assert(box.request_seq == 42 && box.request_len == 5 && !memcmp(box.request, "hello", 5));
    command("oem iphont-get 2a", "FAIL");
    command("oem iphont-put 0 61", "OKAY");
    command("oem iphont-go 2b 1", "FAIL");
    memcpy(box.response, "{}", 2); box.response_len = 2; box.device_ns = 123; box.response_seq = 42;
    command("oem iphont-get 2a", "OKAY");
    struct iphont_reply header; memcpy(&header, staged_data, sizeof(header));
    assert(header.sequence == 42 && header.device_ns == 123 && header.length == 2 && staged_len == 26);
    command("oem iphont-go 2a 1", "FAIL");
    command("oem iphont-go 2b 2", "FAIL");
    command("oem iphont-go 2b 1", "OKAY");
    command("oem iphont-get 2a", "FAIL"); /* never read a prior response while the kernel overwrites it */
    assert(strstr(reply_text, "no longer current"));
    command("oem iphont-go xyz 1", "FAIL");
    command("oem iphont-get 2b garbage", "FAIL");
    command("oem iphont-put ffff 61", "FAIL");
    /* A real ring layout fixture: first four bytes kept, four-byte ring retaining bytes 8..11. */
    uint64_t total = 12; char log[] = "abcdijkl";
    live.log_total = (uintptr_t)&total; live.log_head = 4; live.log_ring = 4; live.log = (uintptr_t)log;
    command("oem iphont-log 0", "OKAY");
    uint64_t fields[4]; memcpy(fields, staged_data, 32);
    assert(fields[0] == 0 && fields[1] == 4 && fields[2] == 12 && !fields[3]);
    assert(staged_len == 36 && !memcmp(staged_data + 32, "abcd", 4));
    command("oem iphont-log 4", "OKAY"); memcpy(fields, staged_data, 32);
    assert(fields[0] == 8 && fields[1] == 12 && fields[3] == 4 && !memcmp(staged_data + 32, "ijkl", 4));
    puts("loader mailbox and ring offset checks passed (host only)");
    return 0;
}
