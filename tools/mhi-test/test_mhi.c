/*
 * Tests of the MHI host (wifi/lx/src/mhi_host.c) against a device made in software: the MHI side
 * of a QCA6390 as the host meets it -- BHI and BHIe, READY and M0, the command ring, the IPCR
 * channels, M1/M2 and M3, its reset, its error state. The device checks what the host does as
 * strictly as it can (the image byte for byte, the contexts' fields, where pointers point, the
 * order of things) and answers after a delay, so the host's waiting is exercised too.
 *
 * The device's memory accesses go through addresses the host's dma_alloc() handed out: an arena
 * whose device addresses begin at 0xa0000000, so that a host that hands the device a processor's
 * pointer is caught.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mhi_host.h"

static unsigned failures, checks;

#define CHECK(c) do {									\
	checks++;									\
	if (!(c)) {									\
		failures++;								\
		fprintf(stderr, "%s:%d: CHECK FAILED: %s\n", __FILE__, __LINE__, #c);	\
	}										\
} while (0)

/* ---- the arena: memory the device reaches --------------------------------------------------------- */

#define ARENA		(16u << 20)
#define IOVA_BASE	0xa0000000ull
static uint8_t *arena;
static size_t arena_used;
static unsigned allocations, frees;

static void *
dma_at(uint64_t iova, size_t length)
{
	if (iova < IOVA_BASE || iova + length > IOVA_BASE + arena_used) {
		return NULL;
	}
	return arena + (iova - IOVA_BASE);
}

/* ---- the device ------------------------------------------------------------------------------------- */

#define BHI		0x200
#define BHIE		0x300
#define CHDB		0x1000
#define ERDB		0x1800
#define CTX		44

enum { NONE, AFTER_BHI_READY, AFTER_M0, AFTER_BHIE_AMSS, AFTER_RESET };

static struct device {
	pthread_mutex_t lock;
	uint32_t regs[0x2000 / 4];
	uint32_t ee, state;
	bool ready, reset_pending;
	int pending;
	uint64_t due_ns;
	bool image_ok, sbl_ok;
	const uint8_t *image;
	size_t image_length, sbl_length;
	unsigned event_count;
	uint64_t er_rp[4];			/* device's write position (iova) on each event ring */
	unsigned events_posted;
	/* the IPCR channels as the device sees them */
	uint64_t ch_rp[128];			/* device's read position (iova), by channel */
	bool ch_running[128];
	uint8_t sent[64][256];			/* what the host sent on channel 20 */
	size_t sent_length[64];
	unsigned sent_count;
	unsigned wake_votes_seen;
	uint32_t wake;
	bool no_events;				/* drop the events (a missing interrupt and its event): the registers still say */
	bool refuse;				/* take every boot loader for a bad one */
	unsigned lose_doorbells;		/* the command ring's doorbell not heard so many times (a device on its way) */
	unsigned commands;
} dev;

static uint64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint32_t *
reg(uint32_t offset)
{
	return &dev.regs[offset / 4];
}

static uint64_t
reg64(uint32_t offset)
{
	return (uint64_t)*reg(offset + 4) << 32 | *reg(offset);
}

static uint64_t
ctx_get64(uint64_t ctx, unsigned at)
{
	uint64_t v;

	memcpy(&v, dma_at(ctx + at, 8), 8);
	return v;
}

static uint32_t
ctx_get32(uint64_t ctx, unsigned at)
{
	uint32_t v;

	memcpy(&v, dma_at(ctx + at, 4), 4);
	return v;
}

static void
ctx_put64(uint64_t ctx, unsigned at, uint64_t v)
{
	memcpy(dma_at(ctx + at, 8), &v, 8);
}

static void
set_status(void)
{
	*reg(MHI_REG_MHISTATUS) = dev.state << 8 | (dev.ready ? MHI_STATUS_READY : 0) |
	    (dev.state == MHI_HOST_STATE_SYS_ERR ? MHI_STATUS_SYSERR : 0);
	*reg(BHI + MHI_BHI_EXECENV) = dev.ee;
}

/* An event on ring e: written at the device's position, which moves, if the host has given room. */
static bool
post_event(unsigned e, uint64_t pointer, uint32_t word0, uint32_t word1)
{
	uint64_t ctx = reg64(MHI_REG_ECABAP) + (uint64_t)e * CTX, base = ctx_get64(ctx, 12), length = ctx_get64(ctx, 20);
	uint64_t wp = ctx_get64(ctx, 36), at = dev.er_rp[e], following = at + 16 == base + length ? base : at + 16;
	uint8_t *element;

	CHECK(ctx_get32(ctx, 4) == 1);			/* a valid event ring */
	if (at == wp) {
		fprintf(stderr, "device: event ring %u is full\n", e);
		failures++;
		return false;
	}
	element = dma_at(at, 16);
	CHECK(element != NULL);
	memcpy(element, &pointer, 8);
	memcpy(element + 8, &word0, 4);
	memcpy(element + 12, &word1, 4);
	dev.er_rp[e] = following;
	ctx_put64(ctx, 28, following);
	dev.events_posted++;
	return true;
}

static void
state_event(uint32_t state)
{
	if (!dev.no_events) {
		post_event(0, 0, state << 24, MHI_TYPE_STATE_CHANGE << 16);
	}
}

static void
ee_event(uint32_t ee)
{
	if (!dev.no_events) {
		post_event(0, 0, ee << 24, MHI_TYPE_EE << 16);
	}
}

/* The MMIO the host set up, checked when the host asks for M0 the first time. */
static void
check_mmio(void)
{
	uint64_t ecabap = reg64(MHI_REG_ECABAP), crcbap = reg64(MHI_REG_CRCBAP);

	CHECK(dma_at(reg64(MHI_REG_CCABAP), 128 * CTX) != NULL);
	CHECK(dma_at(ecabap, CTX) != NULL);
	CHECK(dma_at(crcbap, CTX) != NULL);
	dev.event_count = *reg(MHI_REG_MHICFG) >> 16 & 0xff;
	CHECK(dev.event_count == 2);
	for (unsigned e = 0; e < dev.event_count; e++) {
		uint64_t ctx = ecabap + (uint64_t)e * CTX, base = ctx_get64(ctx, 12), length = ctx_get64(ctx, 20);

		CHECK(base % length == 0 || (base & (base - 1)) == 0 || base % 512 == 0);
		CHECK(ctx_get64(ctx, 28) == base);
		CHECK(ctx_get64(ctx, 36) == base + length - 16);	/* all elements but one for the device */
		dev.er_rp[e] = base;
	}
	CHECK(ctx_get64(crcbap, 28) == ctx_get64(crcbap, 12));
}

/* What has come due. Called with the lock, before every access of the host's. */
static void
device_tick(void)
{
	if (dev.pending == NONE || now_ns() < dev.due_ns) {
		return;
	}
	switch (dev.pending) {
	case AFTER_BHI_READY:
		dev.state = MHI_HOST_STATE_READY;
		dev.ready = true;
		break;
	case AFTER_M0:
		dev.state = MHI_HOST_STATE_M0;
		state_event(MHI_HOST_STATE_M0);
		dev.ee = MHI_HOST_EE_SBL;
		ee_event(MHI_HOST_EE_SBL);
		break;
	case AFTER_BHIE_AMSS:
		dev.ee = MHI_HOST_EE_AMSS;
		ee_event(MHI_HOST_EE_AMSS);
		break;
	case AFTER_RESET:
		*reg(MHI_REG_MHICTRL) &= ~MHI_CTRL_RESET;
		dev.state = MHI_HOST_STATE_RESET;
		dev.ready = false;
		break;
	}
	dev.pending = NONE;
	set_status();
}

static void
later(int what, unsigned us)
{
	dev.pending = what;
	dev.due_ns = now_ns() + us * 1000ull;
}

static void
bhi_go(void)
{
	uint64_t address = (uint64_t)*reg(BHI + MHI_BHI_IMGADDR_HIGH) << 32 | *reg(BHI + MHI_BHI_IMGADDR_LOW);
	uint32_t size = *reg(BHI + MHI_BHI_IMGSIZE);
	const uint8_t *at = dma_at(address, size);

	CHECK(dev.ee == MHI_HOST_EE_PBL);
	CHECK((*reg(BHI + MHI_BHI_IMGTXDB) & 0x3fffffff) != 0);
	dev.sbl_ok = at != NULL && size == dev.sbl_length && memcmp(at, dev.image, size) == 0 && !dev.refuse;
	CHECK(dev.sbl_ok || dev.refuse);
	*reg(BHI + MHI_BHI_STATUS) = (dev.sbl_ok ? 2u : 3u) << 30;
	if (dev.sbl_ok) {
		later(AFTER_BHI_READY, 2000);
	}
}

static void
bhie_go(void)
{
	uint64_t table = reg64(BHIE + MHI_BHIE_TXVECADDR_LOW);
	uint32_t size = *reg(BHIE + MHI_BHIE_TXVECSIZE), seq = *reg(BHIE + MHI_BHIE_TXVECDB) & 0x3fffffff;
	const uint8_t *vectors = dma_at(table, size);
	size_t got = 0;
	bool same = vectors != NULL && size % 16 == 0 && seq != 0;

	CHECK(dev.ee == MHI_HOST_EE_SBL);
	for (uint32_t v = 0; same && v < size / 16; v++) {
		uint64_t address, length;
		const uint8_t *piece;

		memcpy(&address, vectors + 16 * v, 8);
		memcpy(&length, vectors + 16 * v + 8, 8);
		piece = dma_at(address, length);
		same = piece != NULL && got + length <= dev.image_length && memcmp(piece, dev.image + got, length) == 0;
		got += length;
	}
	dev.image_ok = same && got == dev.image_length;
	CHECK(dev.image_ok);
	*reg(BHIE + MHI_BHIE_TXVECSTATUS) = (dev.image_ok ? 2u : 3u) << 30 | seq;
	if (dev.image_ok) {
		later(AFTER_BHIE_AMSS, 3000);
	}
}

static void
commands_go(uint64_t wp)
{
	uint64_t crcbap = reg64(MHI_REG_CRCBAP), base = ctx_get64(crcbap, 12), length = ctx_get64(crcbap, 20);
	uint64_t rp = ctx_get64(crcbap, 28);

	CHECK(dev.state == MHI_HOST_STATE_M0);
	CHECK(wp == ctx_get64(crcbap, 36));
	while (rp != wp) {
		const uint8_t *e = dma_at(rp, 16);
		uint32_t word1, chan, type, code = MHI_CODE_SUCCESS;
		uint64_t cctx;

		memcpy(&word1, e + 12, 4);
		chan = word1 >> 24;
		type = word1 >> 16 & 0xff;
		cctx = reg64(MHI_REG_CCABAP) + (uint64_t)chan * CTX;
		if (type == MHI_TYPE_START_CHAN) {
			uint32_t cfg = ctx_get32(cctx, 0);

			CHECK((cfg & 0xff) == 1);			/* enabled by the host */
			CHECK(ctx_get32(cctx, 4) == (chan == 21 ? 2u : 1u));
			CHECK(ctx_get32(cctx, 8) == 1);			/* their completions go to event ring 1 */
			CHECK(ctx_get64(cctx, 28) == ctx_get64(cctx, 12));
			dev.ch_rp[chan] = ctx_get64(cctx, 12);
			dev.ch_running[chan] = true;
			memcpy(dma_at(cctx, 4), &(uint32_t){ (cfg & ~0xffu) | 2 }, 4);	/* running */
		} else {
			code = 0x10;
		}
		dev.commands++;
		post_event(0, rp, code << 24, MHI_TYPE_CMD_DONE << 16 | chan << 24);
		rp = rp + 16 == base + length ? base : rp + 16;
	}
	ctx_put64(crcbap, 28, rp);
}

/* The host's buffers on channel 21, one: `data` in it and its completion posted. false: there is none. */
static bool
device_send(const void *data, uint32_t length)
{
	uint64_t cctx = reg64(MHI_REG_CCABAP) + 21 * CTX, base = ctx_get64(cctx, 12), size = ctx_get64(cctx, 20);
	uint64_t rp = dev.ch_rp[21], wp = ctx_get64(cctx, 36);
	uint8_t *e;
	uint64_t buffer;
	uint32_t room, word1;

	if (!dev.ch_running[21] || rp == wp || dev.state != MHI_HOST_STATE_M0) {
		return false;
	}
	e = dma_at(rp, 16);
	memcpy(&buffer, e, 8);
	memcpy(&room, e + 8, 4);
	memcpy(&word1, e + 12, 4);
	CHECK((word1 >> 16 & 0xff) == MHI_TYPE_TRANSFER);
	CHECK(word1 & MHI_TRE_IEOT);
	CHECK(length <= (room & 0xffff));
	CHECK(dma_at(buffer, length) != NULL);
	memcpy(dma_at(buffer, length), data, length);
	post_event(1, rp, MHI_CODE_EOT << 24 | length, MHI_TYPE_TX << 16 | 21u << 24);
	dev.ch_rp[21] = rp + 16 == base + size ? base : rp + 16;
	ctx_put64(cctx, 28, dev.ch_rp[21]);
	return true;
}

static void
channel_go(uint32_t chan, uint64_t wp)
{
	uint64_t cctx = reg64(MHI_REG_CCABAP) + (uint64_t)chan * CTX, base = ctx_get64(cctx, 12), size = ctx_get64(cctx, 20);

	CHECK(dev.ch_running[chan]);
	CHECK(dev.state == MHI_HOST_STATE_M0);		/* (doorbells are rung in M0 only) */
	CHECK(wp == ctx_get64(cctx, 36));
	if (chan != 20) {
		return;					/* (channel 21's doorbell: more buffers; device_send() takes them) */
	}
	while (dev.ch_rp[20] != wp) {
		uint64_t rp = dev.ch_rp[20], buffer;
		const uint8_t *e = dma_at(rp, 16);
		uint32_t length, word1;

		memcpy(&buffer, e, 8);
		memcpy(&length, e + 8, 4);
		memcpy(&word1, e + 12, 4);
		length &= 0xffff;
		CHECK((word1 >> 16 & 0xff) == MHI_TYPE_TRANSFER);
		CHECK(dma_at(buffer, length) != NULL && length <= 256);
		if (dev.sent_count < 64 && length <= 256) {
			memcpy(dev.sent[dev.sent_count], dma_at(buffer, length), length);
			dev.sent_length[dev.sent_count++] = length;
		}
		post_event(1, rp, MHI_CODE_EOT << 24 | length, MHI_TYPE_TX << 16 | 20u << 24);
		dev.ch_rp[20] = rp + 16 == base + size ? base : rp + 16;
		ctx_put64(cctx, 28, dev.ch_rp[20]);
	}
}

static void
device_write(uint32_t offset, uint32_t value)
{
	*reg(offset) = value;
	if (offset == BHI + MHI_BHI_IMGTXDB) {
		bhi_go();
	} else if (offset == BHIE + MHI_BHIE_TXVECDB) {
		bhie_go();
	} else if (offset == MHI_REG_MHICTRL) {
		uint32_t state = value >> 8 & 0xff;

		if (value & MHI_CTRL_RESET) {
			dev.ch_running[20] = dev.ch_running[21] = false;
			later(AFTER_RESET, 1000);
		} else if (state == MHI_HOST_STATE_M0 && dev.state == MHI_HOST_STATE_READY) {
			check_mmio();
			later(AFTER_M0, 1000);
		} else if (state == MHI_HOST_STATE_M2 && dev.state == MHI_HOST_STATE_M1) {
			dev.state = MHI_HOST_STATE_M2;
			state_event(MHI_HOST_STATE_M2);
		} else if (state == MHI_HOST_STATE_M3 && dev.state == MHI_HOST_STATE_M0) {
			dev.state = MHI_HOST_STATE_M3;
			state_event(MHI_HOST_STATE_M3);
		} else if (state == MHI_HOST_STATE_M0 && dev.state == MHI_HOST_STATE_M3) {
			dev.state = MHI_HOST_STATE_M0;
			state_event(MHI_HOST_STATE_M0);
		}
		set_status();
	} else if (offset == MHI_REG_CRDB) {
		if (dev.lose_doorbells > 0) {
			dev.lose_doorbells--;
		} else {
			commands_go(reg64(MHI_REG_CRDB));
		}
	} else if (offset >= CHDB && offset < CHDB + 8 * 128 && offset % 8 == 0) {
		uint32_t chan = (offset - CHDB) / 8;
		uint64_t v = reg64(offset);

		if (chan == MHI_WAKE_DOORBELL) {
			dev.wake = (uint32_t)v;
			dev.wake_votes_seen += v != 0;
			if (v != 0 && (dev.state == MHI_HOST_STATE_M1 || dev.state == MHI_HOST_STATE_M2)) {
				dev.state = MHI_HOST_STATE_M0;		/* woken */
				state_event(MHI_HOST_STATE_M0);
				set_status();
			}
		} else {
			channel_go(chan, v);
		}
	} else if (offset >= ERDB && offset < ERDB + 8 * 4 && offset % 8 == 0) {
		uint32_t e = (offset - ERDB) / 8;
		uint64_t ctx = reg64(MHI_REG_ECABAP) + (uint64_t)e * CTX;

		if (dev.event_count != 0) {
			CHECK(reg64(offset) == ctx_get64(ctx, 36));
		}
	}
}

static void
device_init(const uint8_t *image, size_t length, size_t sbl)
{
	pthread_mutex_lock(&dev.lock);
	memset(dev.regs, 0, sizeof(dev.regs));
	memset(dev.ch_running, 0, sizeof(dev.ch_running));
	dev.ee = MHI_HOST_EE_PBL;
	dev.state = MHI_HOST_STATE_RESET;
	dev.ready = false;
	dev.pending = NONE;
	dev.image = image;
	dev.image_length = length;
	dev.sbl_length = sbl;
	dev.event_count = 0;
	dev.no_events = false;
	dev.refuse = false;
	*reg(MHI_REG_BHIOFF) = BHI;
	*reg(MHI_REG_BHIEOFF) = BHIE;
	*reg(MHI_REG_CHDBOFF) = CHDB;
	*reg(MHI_REG_ERDBOFF) = ERDB;
	*reg(MHI_REG_MHICFG) = 0x0000ff80;	/* (hardware channels and channels the device has: kept as they are) */
	set_status();
	pthread_mutex_unlock(&dev.lock);
}

/* ---- the host's ops -------------------------------------------------------------------------------- */

static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static int missions, errors, rddms;
static struct { uint8_t data[256]; size_t length; } received[256];
static unsigned received_count;
static bool loud;

static uint32_t
op_read(void *c, uint32_t offset)
{
	uint32_t v;

	pthread_mutex_lock(&dev.lock);
	device_tick();
	v = offset < sizeof(dev.regs) ? *reg(offset) : 0xffffffffu;
	pthread_mutex_unlock(&dev.lock);
	return v;
}

static void
op_write(void *c, uint32_t offset, uint32_t value)
{
	pthread_mutex_lock(&dev.lock);
	device_tick();
	CHECK(offset < sizeof(dev.regs));
	device_write(offset, value);
	pthread_mutex_unlock(&dev.lock);
}

static void *
op_dma_alloc(void *c, size_t size, size_t align, uint64_t *address)
{
	size_t at = (arena_used + align - 1) & ~(align - 1);

	if (at + size > ARENA) {
		return NULL;
	}
	arena_used = at + size;
	memset(arena + at, 0, size);
	*address = IOVA_BASE + at;
	allocations++;
	return arena + at;
}

static void
op_dma_free(void *c, void *memory, size_t size)
{
	frees++;
}

static uint64_t
op_nanoseconds(void *c)
{
	return now_ns();
}

static void
op_lock(void *c)
{
	pthread_mutex_lock(&host_lock);
}

static void
op_unlock(void *c)
{
	pthread_mutex_unlock(&host_lock);
}

static void
op_pause(void *c, unsigned us)
{
	usleep(us);
}

static void
op_random(void *c, void *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		((uint8_t *)bytes)[i] = (uint8_t)rand();
	}
}

static void
op_say(void *c, const char *line)
{
	if (loud) {
		printf("  host: %s\n", line);
	}
}

static void
op_status(void *c, enum mhi_host_status what)
{
	*(what == MHI_HOST_MISSION ? &missions : what == MHI_HOST_RDDM ? &rddms : &errors) += 1;
}

static void
op_receive(void *c, unsigned channel, const void *data, size_t length)
{
	CHECK(channel == 21);
	if (received_count < 256 && length <= 256) {
		memcpy(received[received_count].data, data, length);
		received[received_count++].length = length;
	}
}

static const struct mhi_host_ops ops = {
	op_read, op_write, op_dma_alloc, op_dma_free, op_nanoseconds, op_lock, op_unlock, op_pause, op_random, op_say,
	op_status, op_receive,
};

/* ---- the tests ------------------------------------------------------------------------------------- */

static uint8_t image[1300000];
static struct mhi_host host;

static struct mhi_host_config
config(void)
{
	struct mhi_host_config c = {
		.max_channels = 128, .timeout_ms = 2000, .buffer_length = 8192, .channel_count = 2, .event_count = 2,
		.channels = { { .number = 20, .elements = 64, .event_ring = 1, .inbound = false, .ee_mask = 0x4 },
			      { .number = 21, .elements = 64, .event_ring = 1, .inbound = true, .ee_mask = 0x4 } },
		.events = { { .elements = 32, .vector = 1, .control = true }, { .elements = 256, .vector = 2, .moderation_ms = 1 } },
		.image = image, .image_length = sizeof(image), .sbl_length = 512 * 1024, .segment_length = 512 * 1024,
		.iova_start = 0, .iova_stop = 0xffffffffull,
	};

	return c;
}

static void
service(void)
{
	mhi_host_interrupt(&host, -1);
}

static void
test_boot(void)
{
	struct mhi_host_config c = config();
	int error;

	device_init(image, sizeof(image), 512 * 1024);
	CHECK(mhi_host_init(&host, &ops, NULL, &c) == 0);
	error = mhi_host_power_up(&host);
	CHECK(error == 0);
	CHECK(dev.sbl_ok && dev.image_ok);
	CHECK(missions == 1);
	CHECK(dev.ch_running[20] && dev.ch_running[21]);
	CHECK(dev.commands == 2);
	CHECK(host.state == MHI_HOST_STATE_M0 && host.ee == MHI_HOST_EE_AMSS);
	printf("boot: %s (%u events)\n", error == 0 ? "the image runs, both channels started" : "FAILED", dev.events_posted);
}

static void
test_traffic(void)
{
	uint8_t message[200];
	unsigned before = received_count, sent_before = dev.sent_count;

	/* more than a ring holds, both ways: the rings wrap */
	for (unsigned i = 0; i < 150; i++) {
		size_t length = 16 + i % 100;

		for (size_t b = 0; b < length; b++) {
			message[b] = (uint8_t)(i * 7 + b);
		}
		pthread_mutex_lock(&dev.lock);
		CHECK(device_send(message, (uint32_t)length));
		pthread_mutex_unlock(&dev.lock);
		service();
		CHECK(received_count == before + i + 1);
		CHECK(received[before + i].length == length && memcmp(received[before + i].data, message, length) == 0);

		if (i < 40) {			/* (the device keeps the first 64 it is sent) */
			CHECK(mhi_host_send(&host, 20, message, length) == 0);
			service();
		}
	}
	CHECK(dev.sent_count == sent_before + 40);
	for (unsigned i = 0; i < 40 && sent_before + i < 64; i++) {
		size_t length = 16 + i % 100;

		CHECK(dev.sent_length[sent_before + i] == length && dev.sent[sent_before + i][3] == (uint8_t)(i * 7 + 3));
	}
	/* a send while the outbound ring is full of things the device has not taken: it waits for room, then fails */
	CHECK(mhi_host_send(&host, 21, message, 10) == ENOTCONN);	/* (21 is inbound) */
	CHECK(mhi_host_send(&host, 20, message, 9000) == EMSGSIZE);
	printf("traffic: %u datagrams in, %u out, the rings wrapped\n", received_count - before, dev.sent_count - sent_before);
}

static void
test_low_power(void)
{
	uint8_t message[32] = "hello";

	/* the device goes to M1 by itself: the host sends it to M2; a send wakes it */
	pthread_mutex_lock(&dev.lock);
	dev.state = MHI_HOST_STATE_M1;
	state_event(MHI_HOST_STATE_M1);
	set_status();
	pthread_mutex_unlock(&dev.lock);
	service();
	CHECK(dev.state == MHI_HOST_STATE_M2);
	CHECK(host.state == MHI_HOST_STATE_M2);
	CHECK(mhi_host_send(&host, 20, message, sizeof(message)) == 0);
	CHECK(dev.wake == 0);			/* the vote was given up after the send */
	CHECK(dev.state == MHI_HOST_STATE_M0);
	service();
	/* held awake: M1 is left to the device to come back from */
	CHECK(mhi_host_wake_get(&host) == 0);
	CHECK(dev.wake == 1);
	pthread_mutex_lock(&dev.lock);
	dev.state = MHI_HOST_STATE_M1;
	state_event(MHI_HOST_STATE_M1);
	set_status();
	pthread_mutex_unlock(&dev.lock);
	service();
	CHECK(dev.state == MHI_HOST_STATE_M1);	/* (not sent to M2) */
	pthread_mutex_lock(&dev.lock);
	dev.state = MHI_HOST_STATE_M0;
	state_event(MHI_HOST_STATE_M0);
	set_status();
	pthread_mutex_unlock(&dev.lock);
	mhi_host_wake_put(&host);
	service();
	/* M3 and back */
	CHECK(mhi_host_suspend(&host) == 0);
	CHECK(dev.state == MHI_HOST_STATE_M3);
	CHECK(mhi_host_resume(&host) == 0);
	CHECK(dev.state == MHI_HOST_STATE_M0);
	CHECK(mhi_host_send(&host, 20, message, sizeof(message)) == 0);
	printf("low power: M1 to M2, woken for a send; M3 and back\n");
}

static void
test_missing_events(void)
{
	unsigned allocated = allocations;

	/* down, and up again with no events at all: the registers alone get it there */
	mhi_host_power_down(&host);
	CHECK(!dev.ch_running[20] && (dev.regs[MHI_REG_MHICTRL / 4] & MHI_CTRL_RESET) == 0);
	device_init(image, sizeof(image), 512 * 1024);
	pthread_mutex_lock(&dev.lock);
	dev.no_events = true;
	pthread_mutex_unlock(&dev.lock);
	missions = 0;
	dev.commands = 0;
	CHECK(mhi_host_power_up(&host) == 0);
	CHECK(missions == 1 && dev.commands == 2);
	CHECK(allocations == allocated);	/* (the memory of the first power-up is used again) */
	printf("again, without a single event: %s\n", missions == 1 ? "up" : "NOT UP");
}

static void
test_lost_doorbell(void)
{
	/* the device does not hear the first doorbell of a channel's start: it is rung again, and the channel starts */
	mhi_host_power_down(&host);
	device_init(image, sizeof(image), 512 * 1024);
	pthread_mutex_lock(&dev.lock);
	dev.no_events = false;
	dev.lose_doorbells = 1;
	pthread_mutex_unlock(&dev.lock);
	host.rung_again = 0;
	CHECK(mhi_host_power_up(&host) == 0);
	CHECK(dev.lose_doorbells == 0 && host.rung_again >= 1 && dev.ch_running[20] && dev.ch_running[21]);
	printf("a command's doorbell lost: rung again %u time(s), the channels %s\n", host.rung_again,
	    dev.ch_running[20] && dev.ch_running[21] ? "started" : "NOT STARTED");
}

static void
test_error(void)
{
	/* the device fails while it runs: whoever runs the host is told */
	pthread_mutex_lock(&dev.lock);
	dev.no_events = false;
	dev.state = MHI_HOST_STATE_SYS_ERR;
	set_status();
	pthread_mutex_unlock(&dev.lock);
	service();
	CHECK(errors == 1);
	CHECK(mhi_host_send(&host, 20, "x", 1) == ENOTCONN);
	/* and a power-up from the error state resets the device first */
	mhi_host_power_down(&host);
	device_init(image, sizeof(image), 512 * 1024);
	pthread_mutex_lock(&dev.lock);
	dev.state = MHI_HOST_STATE_SYS_ERR;
	set_status();
	pthread_mutex_unlock(&dev.lock);
	CHECK(mhi_host_power_up(&host) == 0);
	printf("error: told; a power-up from the error state resets and boots\n");
}

/* A device that refuses the boot loader: the power-up gives up, and says so. */
static void
test_dead(void)
{
	struct mhi_host_config c = config();
	static struct mhi_host other;

	c.timeout_ms = 100;
	device_init(image, sizeof(image), 512 * 1024);
	pthread_mutex_lock(&dev.lock);
	dev.refuse = true;
	pthread_mutex_unlock(&dev.lock);
	CHECK(mhi_host_init(&other, &ops, NULL, &c) == 0);
	CHECK(mhi_host_power_up(&other) == EIO);
	printf("a device that refuses the boot loader: the power-up says EIO\n");
}

int
main(int argc, char **argv)
{
	loud = argc > 1;
	pthread_mutex_init(&dev.lock, NULL);
	arena = aligned_alloc(4096, ARENA);
	for (size_t i = 0; i < sizeof(image); i++) {
		image[i] = (uint8_t)(i * 2654435761u >> 13);
	}
	test_boot();
	test_traffic();
	test_low_power();
	test_missing_events();
	test_lost_doorbell();
	test_error();
	test_dead();
	printf("mhi-test: %u checks, %u failed\n", checks, failures);
	free(arena);
	return failures != 0;
}
