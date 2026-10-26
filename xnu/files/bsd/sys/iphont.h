#ifndef IPHONT_H
#define IPHONT_H
#include <stdint.h>
#define IPHONT_MAGIC UINT64_C(0x31544e4f485049)
#define IPHONT_VERSION 1
#define IPHONT_REQUEST 4096
#define IPHONT_RESPONSE 65536
/* Each writer owns separate cache lines. Publish sequence numbers with release/acquire ordering. */
struct iphont_box {
    uint64_t magic;
    uint32_t version, size;
    uint32_t enabled, control;
    uint8_t reserved[40];
    volatile uint32_t request_seq;
    uint32_t request_len;
    uint8_t request_pad[56];
    char request[IPHONT_REQUEST];
    volatile uint32_t response_seq;
    int32_t error;
    uint32_t response_len, reserved2;
    uint64_t device_ns;
    uint8_t response_pad[40];
    char response[IPHONT_RESPONSE];
} __attribute__((aligned(64)));
/* The exact same response header goes to USB and /dev/iphont. */
struct iphont_reply {
    uint32_t version, sequence;
    int32_t error;
    uint32_t length;
    uint64_t device_ns;
};
#ifdef KERNEL
void iphont_init(void);
void iphont_trace_init(void);
int iphont_registry(char *out, unsigned capacity);
extern struct iphont_box iphont_box;
#endif
#endif
