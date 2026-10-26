/* Included by loader.c; commands share its existing fastboot transport and RAM window. */
#include "../xnu/files/bsd/sys/iphont.h"
static char iphont_request[IPHONT_REQUEST];
static unsigned iphont_request_len;
static unsigned char iphont_output[32 + IPHONT_RESPONSE];
static struct iphont_box *iphont_current(void)
{
    if (!running_live || running_live->magic4 != UINT64_C(0x344556494c554e58) || !running_live->agent) return NULL;
    struct iphont_box *box = (struct iphont_box *)(uintptr_t)(running_live->agent + running_slide);
    if (box->magic != IPHONT_MAGIC || box->version != IPHONT_VERSION || box->size != sizeof(*box) ||
        !__atomic_load_n(&box->enabled, __ATOMIC_ACQUIRE)) return NULL;
    return box;
}
static int iphont_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int iphont_number(const char *s, uint64_t *value, unsigned maximum)
{
    unsigned digits = 0;
    *value = 0;
    while (*s) {
        int n = iphont_hex(*s++);
        if (n < 0 || ++digits > maximum) return 0;
        *value = *value * 16 + (unsigned)n;
    }
    return digits != 0;
}
static void iphont_log(uint64_t cursor)
{
    if (!running_live || running_live->magic4 != UINT64_C(0x344556494c554e58) || !running_live->log_total) {
        fb_reply("FAIL", "incremental logs need a running XNULIVE4 kernel"); return;
    }
    uint64_t total = __atomic_load_n((uint64_t *)(uintptr_t)(running_live->log_total + running_slide), __ATOMIC_ACQUIRE);
    uint64_t head = running_live->log_head, ring = running_live->log_ring;
    if (!head || !ring || head > 0x100000 || ring > 0x400000) { fb_reply("FAIL", "invalid log geometry"); return; }
    uint64_t start = cursor > total ? 0 : cursor, lost = 0;
    uint64_t oldest = total > head + ring ? total - ring : head;
    if (start >= head && start < oldest) { lost = oldest - start; start = oldest; }
    uint64_t end = total;
    if (start < head && end > head) end = head;
    if (end - start > IPHONT_RESPONSE) end = start + IPHONT_RESPONSE;
    const char *log = (const char *)(uintptr_t)(running_live->log + running_slide);
    for (uint64_t at = start; at < end; at++)
        iphont_output[32 + at - start] = log[at < head ? at : head + (at - head) % ring];
    uint64_t after = __atomic_load_n((uint64_t *)(uintptr_t)(running_live->log_total + running_slide), __ATOMIC_ACQUIRE);
    if (start >= head && after - start > ring) { fb_reply("FAIL", "log reader overrun; retry from same cursor"); return; }
    uint64_t header[4] = {start, end, total, lost};
    memcpy(iphont_output, header, sizeof(header));
    staged_data = (const char *)iphont_output; staged_len = 32 + end - start;
    fb_reply("OKAY", "");
}
static void iphont_command(const char *cmd)
{
    if (starts_with(cmd, "oem iphont-log ")) {
        uint64_t cursor;
        if (!iphont_number(cmd + 15, &cursor, 16)) fb_reply("FAIL", "invalid log cursor");
        else iphont_log(cursor);
        return;
    }

    struct iphont_box *box = iphont_current();
    if (!box) { fb_reply("FAIL", "iphont unavailable: boot a new kernel with iphont=1"); return; }
    if (starts_with(cmd, "oem iphont-put ")) {
        const char *arg = cmd + 15, *hex = arg;
        unsigned at = 0, digits = 0;
        while (*hex && *hex != ' ') {
            int n = iphont_hex(*hex++);
            if (n < 0 || ++digits > 4) { fb_reply("FAIL", "invalid offset"); return; }
            at = at * 16 + n;
        }
        if (!digits || *hex++ != ' ' || !*hex) { fb_reply("FAIL", "missing request data"); return; }
        unsigned len = (unsigned)str_len(hex);
        if ((len & 1) || at + len / 2 >= IPHONT_REQUEST || (at && at != iphont_request_len)) {
            fb_reply("FAIL", "request size or offset mismatch"); return;
        }
        for (unsigned i = 0; i < len; i++) if (iphont_hex(hex[i]) < 0) {
            fb_reply("FAIL", "invalid request hex"); return;
        }
        if (!at) iphont_request_len = 0;
        for (unsigned i = 0; i < len; i += 2) iphont_request[at + i / 2] = (char)(iphont_hex(hex[i]) * 16 + iphont_hex(hex[i + 1]));
        iphont_request_len = at + len / 2;
        fb_reply("OKAY", "");
    } else if (starts_with(cmd, "oem iphont-go ")) {
        const char *arg = cmd + 14;
        char sequence_text[9]; unsigned digits = 0;
        while (arg[digits] && arg[digits] != ' ' && digits < 8) { sequence_text[digits] = arg[digits]; digits++; }
        sequence_text[digits] = 0;
        uint64_t seq, length;
        if (arg[digits] != ' ' || !iphont_number(sequence_text, &seq, 8) ||
            !iphont_number(arg + digits + 1, &length, 4) || length != iphont_request_len || !seq || !length ||
            __atomic_load_n(&box->request_seq, __ATOMIC_ACQUIRE) != __atomic_load_n(&box->response_seq, __ATOMIC_ACQUIRE)) {
            fb_reply("FAIL", "invalid sequence or pending request (never replay a timed out mutation)"); return;
        }
        if ((uint32_t)seq == box->response_seq) { fb_reply("FAIL", "sequence reused"); return; }
        memcpy(box->request, iphont_request, iphont_request_len);
        box->request_len = iphont_request_len;
        __atomic_store_n(&box->request_seq, (uint32_t)seq, __ATOMIC_RELEASE);
        iphont_request_len = 0;
        fb_reply("OKAY", "");
    } else if (starts_with(cmd, "oem iphont-get ")) {
        uint64_t parsed;
        if (!iphont_number(cmd + 15, &parsed, 8) || !parsed) { fb_reply("FAIL", "invalid sequence"); return; }
        uint32_t seq = (uint32_t)parsed;
        if (__atomic_load_n(&box->request_seq, __ATOMIC_ACQUIRE) != seq) {
            fb_reply("FAIL", "iphont sequence is no longer current"); return;
        }
        if (__atomic_load_n(&box->response_seq, __ATOMIC_ACQUIRE) != seq) {
            fb_reply("FAIL", "iphont pending"); return;
        }
        uint32_t len = box->response_len;
        if (len > IPHONT_RESPONSE) { fb_reply("FAIL", "invalid response size"); return; }
        struct iphont_reply header = { IPHONT_VERSION, seq, box->error, len, box->device_ns };
        memcpy(iphont_output, &header, sizeof(header));
        memcpy(iphont_output + sizeof(header), box->response, len);
        staged_data = (const char *)iphont_output;
        staged_len = sizeof(header) + len;
        fb_reply("OKAY", "");
    } else fb_reply("FAIL", "unknown iphont command");
}
