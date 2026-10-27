/*
 * Bluetooth for iOS's own stack: the controller brought up the way its vendor's driver does it, then
 * handed to iOS's bluetoothd and BlueTool through the interfaces Apple's UART driver gives them on an
 * iPhone, so that the stock daemons run unmodified.
 *
 * The controller (OnePlus 8: the Bluetooth half of the QCA6390, bsd/dev/instantnoodle_bt.c) boots
 * from ROM and wants, before it is any use, its RAM patch and its configuration (NVM) loaded over HCI
 * with Qualcomm's own commands: what Linux's drivers/bluetooth/btqca.c and hci_qca.c do, done here in
 * the same order (the protocol, read as data from those files):
 *   version  vendor command 0xfc00 [0x19] at 115200 baud: its Command Complete [00 19 0c ...] (older
 *            chips: a vendor event 0xff [00 02 ...]) with the product, patch, ROM and SoC ids; the ROM version names the files (QCA6390 2.0:
 *            htbtfw20.tlv, htnv20.bin -- the phone's own, from its "bluetooth" partition, fw/ in the
 *            small image: userland/build.sh, from ../fw-bt);
 *   speed    0xfc48 [14] (3 Mbaud), then the host's side; the version asked again, at the new rate;
 *   patch    the TLV file in segments of 243 bytes, 0xfc00 [0x1e len data]; its header says whether
 *            the chip acknowledges each segment (Command Complete [00 1e]; older chips a vendor event
 *            [00 04 result]) or only the last;
 *   NVM      the same; on the way three of its tags are changed: 2, the device's address (the one
 *            the loader's device tree gives, /chosen mac-address-bluetooth0); 17, the UART's rate
 *            (3 Mbaud) and Qualcomm's in-band sleep on; 27, deep sleep on (both as Linux sets them;
 *            the chip sleeps in-band whatever the NVM says): this file speaks the in-band sleep for
 *            iOS, whose H4 has no such thing (see "in-band sleep" below);
 *   then     0xfc17 [14 00] (the chip's own logging off, as Linux does for this chip), HCI_Reset, the
 *            address read back.
 * bt_test=1 then has the controller itself discover: version, an LE scan of 4 s and an inquiry of
 * 5 s, what each found counted -- the radio proved before iOS is involved.
 *
 * What iOS finds (the interfaces of an iPhone X, whose Bluetooth is a Broadcom chip on a UART, "H4"):
 *   - an IOService named "bluetooth" (iokit/Kernel/HKBluetooth.cpp) with the device tree's
 *     transport-encoding (1: H4), transport-speed (3000000) and local-mac-address, which bluetoothd
 *     and BlueTool read;
 *   - the UART as Apple's AppleOnboardSerial gives it: a kernel control socket named
 *     "com.apple.uart.bluetooth" (PF_SYSTEM, SYSPROTO_CONTROL, a stream): read() and write() are the
 *     bytes on the wire, its socket options TIOCGETA/TIOCSETA the termios (the speed, the flow
 *     control). One client at a time;
 *   - /dev/btpoweroff: open, the chip is off; closed again, on (BlueTool's "power off|on" for a UART
 *     chip, bluetool_command_power, and btboot's); here "on" includes loading the patch and the NVM,
 *     which on an iPhone BlueTool's own boot script would do for its Broadcom chip. A connect while that is
 *     going on waits for it (8 s at most).
 * The rest is iOS's: bluetoothd sends HCI and gets HCI, whole packets either way, the chip's in-band
 * sleep bytes taken out. Controller capability bits and HCI replies pass through unchanged. What BlueTool is asked to do (init, boot,
 * deepsleep) is done for this board by userland's src/btboot.c under BlueTool's service name, and
 * bluetoothd is told which socket to use by the
 * environment its job gives it (BT_HCI_TRANSPORT): without it, it would ask for the Skywalk channel
 * "com.apple.uart.sk.bluetooth", which this kernel does not have.
 *
 * boot-args: bt=1 (board=instantnoodle) all of the above; bt_probe=1 what the firmware left, read
 * only, first; bt_test=1 the controller's own discovery after the bring-up; bt_trace=1 every HCI
 * command and event in the kernel's log (2: and the ACL/SCO headers; sysctl kern.hackios.bt_trace
 * changes it while running); bt_sco_hci=0 iOS's voice links set up as it asks (see "voice (SCO/eSCO) over HCI" below),
 * bt_sco_pace=0 its voice data sent as it comes (see "the voice data's pace").
 * sysctl kern.hackios.bt: the state, the chip's versions and the counters.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/instantnoodle.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>

extern const uint8_t *hk_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

void hk_bt_say(const char *format, ...) __printflike(1, 2);
bool hk_bt_wanted(void);
void hk_bt_set_address(const uint8_t address[6]);
void hk_bt_init(void);

#define say hk_bt_say

/* to the loader's log as well: the start of a run's kernel log may not reach the host */
void
hk_bt_say(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

#define OPER_BAUD               3000000u
#define QCA_BAUD_3000000        14              /* Qualcomm's index of 3 Mbaud (btqca.h enum qca_baudrate) */
#define EDL_PATCH_CMD           0xfc00
#define EDL_SET_BAUDRATE        0xfc48
#define EDL_DISABLE_LOGGING     0xfc17
#define EDL_VERSION_REQ         0x19
#define EDL_TLV_REQ             0x1e
#define EDL_APP_VER_RES         0x02
#define EDL_TLV_DNLD_RES        0x04
#define TLV_SEGMENT             243
#define EV_COMMAND_COMPLETE     0x0e
#define EV_COMMAND_STATUS       0x0f
#define EV_HARDWARE_ERROR       0x10
#define EV_VENDOR               0xff

static uint32_t bt_mode, bt_test, bt_trace;
static uint8_t address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };   /* the loader's, until HKBluetooth.cpp says */
static volatile bool address_known;

static LCK_GRP_DECLARE(hk_bt_lck_grp, "hk-bt");
static LCK_MTX_DECLARE(power_lock, &hk_bt_lck_grp);     /* power transitions and the bring-up */

/* the state, for the threads, the socket and the sysctl */
static volatile bool chip_ready;        /* patched, reset: iOS may talk to it */
static volatile bool bringing_up, bringup_failed;
static volatile bool want_off;          /* someone holds /dev/btpoweroff */
static volatile bool power_request;     /* the power thread has something to do */
static volatile bool pump_busy;
static uint32_t bringups, bringup_failures;
static struct {
	uint32_t product, soc;
	uint16_t patch, rom;
	bool known;
} chipver;
static uint64_t bytes_in, bytes_out, packets_in, packets_out, dropped_unconnected, enqueue_waits;

/* ---- HCI over H4, as the controller sends it ------------------------------------------------------- */

struct h4 {
	uint8_t type;
	uint32_t have, need, header;
	uint8_t data[2048];             /* the largest event is 2 + 255; an ACL here at most 1024 + 4 (Read_Buffer_Size) */
	bool overlong;
};

static uint32_t h4_resyncs;
static struct h4 trace_in, trace_out;    /* what comes (the pump) and goes (the client), packet by packet */

/* One byte in; true when it ended a packet (data = header and body, without the type). */
static bool
h4_feed(struct h4 *p, uint8_t byte)
{
	if (p->type == 0) {
		switch (byte) {
		case 0x01: p->header = 3; break;        /* (a command: only in the trace of what goes out) */
		case 0x02: p->header = 4; break;
		case 0x03: p->header = 3; break;
		case 0x04: p->header = 2; break;
		case 0x05: p->header = 4; break;
		default:
			if (h4_resyncs++ < 32) {
				say("bt: byte %#x where a packet should begin (%s)\n", byte, byte == 0xfd ? "Qualcomm's in-band sleep: wake" :
				    byte == 0xfe ? "Qualcomm's in-band sleep: sleep" : byte == 0xfc ? "Qualcomm's in-band sleep: wake ack" :
				    "out of step");
			}
			return false;
		}
		p->type = byte;
		p->have = 0;
		p->need = p->header;
		p->overlong = false;
		return false;
	}
	if (p->have < sizeof(p->data)) {
		p->data[p->have] = byte;
	} else {
		p->overlong = true;
	}
	p->have++;
	if (p->have == p->header) {
		switch (p->type) {
		case 0x01: p->need = p->header + p->data[2]; break;
		case 0x02: p->need = p->header + (p->data[2] | p->data[3] << 8); break;
		case 0x03: p->need = p->header + p->data[2]; break;
		case 0x04: p->need = p->header + p->data[1]; break;
		case 0x05: p->need = p->header + ((p->data[2] | p->data[3] << 8) & 0x3fff); break;
		}
	}
	if (p->have >= p->header && p->have == p->need) {
		return true;
	}
	return false;
}

static void
h4_done(struct h4 *p)
{
	p->type = 0;
}

static void
trace(char direction, uint8_t type, const uint8_t *data, uint32_t length)
{
	char hex[3 * 24 + 1];
	uint32_t shown = length < 24 ? length : 24, at = 0;

	if (bt_trace == 0 || ((type == 0x02 || type == 0x03 || type == 0x05) && bt_trace < 2)) {
		return;
	}
	for (uint32_t i = 0; i < shown && at + 4 < sizeof(hex); i++) {
		at += (uint32_t)snprintf(hex + at, sizeof(hex) - at, "%02x ", data[i]);
	}
	hex[at] = 0;
	if (type == 0x01) {
		say("bt: %c cmd %04x len %u: %s\n", direction, data[0] | data[1] << 8, length - 3, hex);
	} else if (type == 0x04) {
		say("bt: %c evt %02x len %u: %s\n", direction, data[0], length - 2, hex);
	} else {
		say("bt: %c %s %u bytes: %s\n", direction, type == 0x02 ? "acl" : type == 0x03 ? "sco" : "iso", length, hex);
	}
}

/*
 * ---- Qualcomm's in-band sleep (IBS) -------------------------------------------------------------------
 *
 * Once its NVM is in, the chip sleeps whenever the line has been quiet, and says so with one byte
 * between packets; the host does the same for its own side (Linux hci_qca.c, which this follows):
 *   0xfe  "I sleep"     (either side);
 *   0xfd  "wake up"     the chip, before it sends anything after its sleep: it waits for 0xfc and then
 *                       sends; the host, before it sends to a sleeping chip: it waits for the chip's 0xfc
 *                       (sent again every 100 ms until it comes);
 *   0xfc  "I am awake"  the answer to 0xfd.
 * The host's side sleeps after 2 s without sending (IBS_HOST_TX_IDLE_TIMEOUT_MS). None of these bytes
 * reaches iOS: its H4 has no such thing. The chip's 0xfd is answered at once, by whoever is reading.
 */
#define IBS_SLEEP               0xfe
#define IBS_WAKE                0xfd
#define IBS_WAKE_ACK            0xfc
#define IBS_TX_ASLEEP           0
#define IBS_TX_WAKING           1
#define IBS_TX_AWAKE            2

static LCK_MTX_DECLARE(ibs_lock, &hk_bt_lck_grp);       /* one sender at a time, and the sleep between packets only */
static volatile int ibs_tx = IBS_TX_ASLEEP;
static volatile bool ibs_host;          /* the host side sleeps and wakes the chip (after the bring-up) */
static uint64_t ibs_last_tx;
static uint32_t ibs_wakes_sent, ibs_acks_got, ibs_wakes_got, ibs_sleeps_got, ibs_sleeps_sent, ibs_wake_failures;

/* one of the three bytes from the chip, where a packet would begin */
static void
ibs_from_chip(uint8_t byte)
{
	uint8_t ack = IBS_WAKE_ACK;

	switch (byte) {
	case IBS_WAKE:
		ibs_wakes_got++;
		instantnoodle_bt_uart_write(&ack, 1);
		break;
	case IBS_SLEEP:
		ibs_sleeps_got++;
		break;
	case IBS_WAKE_ACK:
		ibs_acks_got++;
		if (ibs_tx == IBS_TX_WAKING) {
			ibs_tx = IBS_TX_AWAKE;
			thread_wakeup((event_t)&ibs_tx);
		}
		break;
	}
}

static inline bool
is_ibs(uint8_t byte)
{
	return byte == IBS_SLEEP || byte == IBS_WAKE || byte == IBS_WAKE_ACK;
}

static volatile bool pump_active;       /* the pump thread reads the UART (after the bring-up) */
static void own_poll(uint32_t ms);

/* What the host sends, the chip woken first when it sleeps. 0 or an errno. Thread context. */
static int
host_write(const uint8_t *bytes, uint32_t n)
{
	int error;

	if (!ibs_host) {
		return instantnoodle_bt_uart_write(bytes, n);
	}
	lck_mtx_lock(&ibs_lock);
	if (ibs_tx != IBS_TX_AWAKE) {
		uint8_t wake = IBS_WAKE;

		ibs_tx = IBS_TX_WAKING;
		for (int tries = 0; tries < 20 && ibs_tx != IBS_TX_AWAKE; tries++) {
			instantnoodle_bt_uart_write(&wake, 1);
			ibs_wakes_sent++;
			if (pump_active) {
				assert_wait_timeout((event_t)&ibs_tx, THREAD_UNINT, 100, NSEC_PER_MSEC);
				if (ibs_tx == IBS_TX_AWAKE) {
					clear_wait(current_thread(), THREAD_AWAKENED);
				} else {
					thread_block(THREAD_CONTINUE_NULL);
				}
			} else {
				own_poll(100);
			}
		}
		if (ibs_tx != IBS_TX_AWAKE) {
			ibs_tx = IBS_TX_ASLEEP;
			ibs_wake_failures++;
			lck_mtx_unlock(&ibs_lock);
			say("bt: the chip does not wake (no 0xfc to 20 times 0xfd)\n");
			instantnoodle_bt_uart_state("wake");
			return ETIMEDOUT;
		}
	}
	ibs_last_tx = mach_absolute_time();
	error = instantnoodle_bt_uart_write(bytes, n);
	lck_mtx_unlock(&ibs_lock);
	return error;
}

/* The host's side to sleep when it has sent nothing for 2 s (the pump's loop). */
static void
ibs_idle(void)
{
	uint64_t idle;
	uint8_t sleep = IBS_SLEEP;

	if (!ibs_host || ibs_tx != IBS_TX_AWAKE) {
		return;
	}
	nanoseconds_to_absolutetime(2000 * NSEC_PER_MSEC, &idle);
	if (mach_absolute_time() - ibs_last_tx < idle || !lck_mtx_try_lock(&ibs_lock)) {
		return;
	}
	if (ibs_tx == IBS_TX_AWAKE && mach_absolute_time() - ibs_last_tx >= idle) {
		instantnoodle_bt_uart_write(&sleep, 1);
		ibs_sleeps_sent++;
		ibs_tx = IBS_TX_ASLEEP;
	}
	lck_mtx_unlock(&ibs_lock);
}

/* ---- talking to the chip ourselves (the bring-up, the self-test) ------------------------------------ */

static struct h4 own;

static int
send_command(uint16_t opcode, const uint8_t *params, uint8_t length)
{
	uint8_t packet[4 + 255];

	packet[0] = 0x01;
	packet[1] = (uint8_t)opcode;
	packet[2] = (uint8_t)(opcode >> 8);
	packet[3] = length;
	if (length != 0) {
		memcpy(packet + 4, params, length);
	}
	trace('>', 0x01, packet + 1, 3u + length);
	return host_write(packet, 4u + length);
}

/*
 * Events for us while the pump reads (the self-test, after the bring-up): the pump puts every event
 * there while no client has the UART.
 */
#define OWN_QUEUE               32
static uint8_t own_queue[OWN_QUEUE][2 + 255];
static volatile uint32_t own_head, own_tail, own_lost;
static volatile bool own_mode;          /* events are ours, not a client's */

static void
own_put(const uint8_t *event, uint32_t length)
{
	if (own_head - own_tail >= OWN_QUEUE) {
		own_lost++;
		return;
	}
	memcpy(own_queue[own_head % OWN_QUEUE], event, length < 2 + 255 ? length : 2 + 255);
	own_head++;
	thread_wakeup((event_t)&own_head);
}

/* The UART read here (the bring-up: no pump yet) for `ms', bytes into the parser `own', IBS bytes answered. */
static bool
own_byte(uint8_t *event, uint32_t ms)
{
	uint8_t byte;

	if (instantnoodle_bt_uart_read(&byte, 1, ms) == 0) {
		return false;
	}
	if (own.type == 0 && is_ibs(byte)) {
		ibs_from_chip(byte);
		return false;
	}
	if (h4_feed(&own, byte)) {
		bool event_here = own.type == 0x04 && !own.overlong;

		if (event_here) {
			trace('<', 0x04, own.data, own.have);
			memcpy(event, own.data, own.have);
		}
		h4_done(&own);
		return event_here;
	}
	return false;
}

/* (host_write()'s wait for the chip's 0xfc when nobody else reads; an event that comes meanwhile is kept) */
static void
own_poll(uint32_t ms)
{
	uint64_t deadline;
	uint8_t event[260];

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline && ibs_tx != IBS_TX_AWAKE) {
		if (own_byte(event, 5)) {
			own_put(event, 2u + event[1]);
		}
	}
}

/*
 * The next event the chip sends, within `ms'; its code and parameters into `event' (code, length,
 * parameters). Packets that are not events are dropped. false: none came.
 */
static bool
next_event(uint8_t *event, uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	for (;;) {
		uint64_t now = mach_absolute_time();
		uint32_t left_ms = 0;

		if (own_tail != own_head) {
			memcpy(event, own_queue[own_tail % OWN_QUEUE], 2 + 255);
			own_tail++;
			return true;
		}
		if (now < deadline) {
			uint64_t ns;

			absolutetime_to_nanoseconds(deadline - now, &ns);
			left_ms = (uint32_t)(ns / NSEC_PER_MSEC) + 1;
		}
		if (left_ms == 0) {
			return false;
		}
		if (pump_active) {
			assert_wait_timeout((event_t)&own_head, THREAD_UNINT, left_ms, NSEC_PER_MSEC);
			if (own_tail != own_head) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			continue;
		}
		if (own_byte(event, left_ms)) {
			return true;
		}
	}
}

/* An event matching (code, and for 0x0e/0x0f the opcode, for 0xff the first two parameters). */
static bool
wait_for(uint8_t code, uint16_t key, uint8_t *event, uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint64_t ns;

		absolutetime_to_nanoseconds(deadline - mach_absolute_time(), &ns);
		if (!next_event(event, (uint32_t)(ns / NSEC_PER_MSEC) + 1)) {
			return false;
		}
		if (event[0] == EV_HARDWARE_ERROR) {
			say("bt: the chip reports a hardware error, code %#x\n", event[1] >= 1 ? event[2] : 0);
		}
		if (event[0] != code) {
			continue;
		}
		if (code == EV_COMMAND_COMPLETE && event[1] >= 3 && (event[3] | event[4] << 8) == key) {
			return true;
		}
		if (code == EV_COMMAND_STATUS && event[1] >= 4 && (event[4] | event[5] << 8) == key) {
			return true;
		}
		if (code == EV_VENDOR && event[1] >= 2 && (event[2] | event[3] << 8) == key) {
			return true;
		}
		if (code != EV_COMMAND_COMPLETE && code != EV_COMMAND_STATUS && code != EV_VENDOR) {
			return true;
		}
	}
	return false;
}

/* A standard command and its Command Complete: its status (0 = success), or -1 when none came. */
static int
command(uint16_t opcode, const uint8_t *params, uint8_t length, uint8_t *event, uint32_t ms)
{
	uint8_t scratch[260];

	if (event == NULL) {
		event = scratch;
	}
	if (send_command(opcode, params, length) != 0) {
		return -1;
	}
	if (!wait_for(EV_COMMAND_COMPLETE, opcode, event, ms)) {
		say("bt: no Command Complete for %04x in %u ms\n", opcode, ms);
		instantnoodle_bt_uart_state("no answer");
		return -1;
	}
	return event[1] >= 4 ? event[5] : 0;
}

/*
 * The answer to one of the chip's own commands (0xfc00 [sub-command ...]): older chips send it as a
 * vendor event [00 rtype data] (the ROME family, rtype the answer's own code), the QCA6390 and newer
 * as the command's Command Complete [status sub-command data] (btqca.c, soc_type >= QCA_WCN3991).
 * Both are taken; `*data' the bytes after the type, `*status' 0 for success. false: none came.
 */
static bool
wait_edl(uint8_t vendor_rtype, uint8_t sub_command, uint8_t *event, uint32_t ms, const uint8_t **data, uint32_t *length,
    uint8_t *status, bool *complete)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint64_t ns;

		absolutetime_to_nanoseconds(deadline - mach_absolute_time(), &ns);
		if (!next_event(event, (uint32_t)(ns / NSEC_PER_MSEC) + 1)) {
			return false;
		}
		if (event[0] == EV_VENDOR && event[1] >= 2 && event[2] == 0x00 && event[3] == vendor_rtype) {
			*data = event + 4;
			*length = event[1] - 2u;
			*status = 0;
			*complete = false;
			return true;
		}
		if (event[0] == EV_COMMAND_COMPLETE && event[1] >= 5 && (event[3] | event[4] << 8) == EDL_PATCH_CMD &&
		    event[6] == sub_command) {
			*data = event + 7;
			*length = event[1] - 5u;
			*status = event[5];
			*complete = true;
			return true;
		}
		if (event[0] == EV_COMMAND_COMPLETE && event[1] >= 4 && (event[3] | event[4] << 8) == EDL_PATCH_CMD) {
			say("bt: the chip refuses its command %#x: status %#x\n", sub_command, event[5]);
			*data = event + 6;
			*length = 0;
			*status = event[5] != 0 ? event[5] : 0xff;
			*complete = true;
			return true;
		}
		if (event[0] == EV_HARDWARE_ERROR) {
			say("bt: the chip reports a hardware error, code %#x\n", event[1] >= 1 ? event[2] : 0);
		}
	}
	return false;
}

/* 0xfc00 [0x19]: the chip's ids (struct qca_btsoc_version: product, patch, ROM, SoC). */
static bool
read_version(void)
{
	uint8_t request = EDL_VERSION_REQ, event[260], status;
	const uint8_t *v;
	uint32_t length;
	bool complete;

	if (send_command(EDL_PATCH_CMD, &request, 1) != 0 ||
	    !wait_edl(EDL_APP_VER_RES, EDL_VERSION_REQ, event, 1000, &v, &length, &status, &complete) || status != 0) {
		return false;
	}
	if (complete) {
		v++;                            /* (the Command Complete form has a byte more before the ids: their length) */
		length = length > 0 ? length - 1 : 0;
	}
	if (length < 12) {
		say("bt: the version answer is %u bytes, not 12\n", length);
		return false;
	}
	chipver.product = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
	chipver.patch = (uint16_t)(v[4] | v[5] << 8);
	chipver.rom = (uint16_t)(v[6] | v[7] << 8);
	chipver.soc = v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24;
	chipver.known = true;
	return chipver.soc != 0 && chipver.rom != 0;
}

/*
 * A TLV file to the chip (qca_download_firmware()): segments of 243 bytes; the mode in a patch's
 * header says which segments the chip acknowledges (0 all, otherwise only the last).
 */
static bool
download(const char *name, const uint8_t *file, uint64_t size, bool patch)
{
	uint8_t segment[2 + TLV_SEGMENT], event[260], mode = 0;
	uint64_t sent = 0;
	uint32_t count = 0;

	if (size < 4 || size > 1024 * 1024) {
		say("bt: %s: %llu bytes is not a TLV file\n", name, size);
		return false;
	}
	if (patch && size >= 4 + 24) {
		mode = file[4 + 10];            /* struct tlv_type_patch: download_mode */
		say("bt: %s: patch of %u bytes, version %#x, ROM build %#x, segments acknowledged: %s\n", name,
		    file[8] | file[9] << 8 | file[10] << 16 | (uint32_t)file[11] << 24, file[4 + 16] | file[4 + 17] << 8,
		    file[4 + 14] | file[4 + 15] << 8, mode == 0 ? "each" : "the last");
	}
	while (sent < size) {
		uint32_t length = size - sent < TLV_SEGMENT ? (uint32_t)(size - sent) : TLV_SEGMENT;
		bool last = sent + length == size || length < TLV_SEGMENT;

		segment[0] = EDL_TLV_REQ;
		segment[1] = (uint8_t)length;
		memcpy(segment + 2, file + sent, length);
		if (send_command(EDL_PATCH_CMD, segment, (uint8_t)(length + 2)) != 0) {
			say("bt: %s: segment %u could not be sent (the chip holds CTS)\n", name, count);
			return false;
		}
		if (mode == 0 || last) {
			const uint8_t *data;
			uint32_t got;
			uint8_t status;
			bool complete;

			if (!wait_edl(EDL_TLV_DNLD_RES, EDL_TLV_REQ, event, 2000, &data, &got, &status, &complete)) {
				say("bt: %s: no answer to segment %u (%llu of %llu bytes)\n", name, count, sent, size);
				return false;
			}
			/* the vendor event's result is its first byte; the Command Complete's, its status */
			if (status != 0 || (!complete && got >= 1 && data[0] != 0)) {
				say("bt: %s: segment %u refused, result %#x\n", name, count, complete ? status : data[0]);
				return false;
			}
		}
		sent += length;
		count++;
	}
	say("bt: %s: %llu bytes in %u segments, taken\n", name, size, count);
	return true;
}

/*
 * The NVM's tags changed as the top of this file says (qca_tlv_check_data()): in a copy. Its layout: a
 * TLV header (type 2; or 4, a container whose first entry is the type 2 one), then tags of
 * { u16 id, u16 length, u32, u32, data }.
 */
static bool
nvm_fix(uint8_t *nvm, uint64_t size)
{
	uint8_t *tlv = nvm;
	uint32_t type, length, at = 0;
	bool address_set = false;

	if (size < 4 || size > 1024 * 1024) {
		return false;
	}
	type = tlv[0];
	length = tlv[1] | tlv[2] << 8 | tlv[3] << 16;
	if (type == 4 && size >= 8) {
		tlv += 4;
		type = tlv[0];
		length = tlv[1] | tlv[2] << 8 | tlv[3] << 16;
	}
	if (type != 2 || (uint64_t)(tlv - nvm) + 4 + length > size) {
		say("bt: the NVM is of type %u, %u bytes: rejected\n", type, length);
		return false;
	}
	while (at + 12 <= length) {
		uint8_t *tag = tlv + 4 + at;
		uint16_t id = (uint16_t)(tag[0] | tag[1] << 8), tag_length = (uint16_t)(tag[2] | tag[3] << 8);
		uint8_t *data = tag + 12;

		if (at + 12 + tag_length > length) {
			say("bt: the NVM's tag %u runs past its end\n", id);
			return false;
		}
		if (id == 2 && tag_length == 6) {
			for (int i = 0; i < 6; i++) {
				data[i] = address[5 - i];       /* little-endian, as bdaddr_t */
			}
			address_set = true;
		} else if (id == 17 && tag_length >= 3) {
			/* the rate is its second byte from the WCN3991 on, the QCA6390 included (qca_tlv_check_data()) */
			say("bt: NVM tag 17 (HCI transport) was %02x %02x %02x: in-band sleep on, 3 Mbaud\n", data[0], data[1], data[2]);
			data[0] |= 0x80;
			data[1] = QCA_BAUD_3000000;
		} else if (id == 27 && tag_length >= 1) {
			say("bt: NVM tag 27 (sleep) was %02x: deep sleep on\n", data[0]);
			data[0] |= 0x01;
		}
		at += 12 + tag_length;
	}
	if (at != length) {
		say("bt: the NVM ends with an incomplete tag\n");
		return false;
	}
	if (!address_set) {
		say("bt: the NVM has no address tag: the chip keeps its own\n");
	}
	return true;
}

static const uint8_t *
firmware(const char *name, uint64_t *size)
{
	const uint8_t *data = hk_image_file("fw", name, size, NULL);

	if (data == NULL) {
		say("bt: no fw/%s in the small image: put the phone's bluetooth partition's image/%s into fw-bt/ and run "
		    "userland/build.sh\n", name, name);
	}
	return data;
}

/* The whole bring-up (the top of this file), the chip's power on. power_lock held. */
static bool
bring_up(void)
{
	char patch_name[32], nvm_name[32];
	const uint8_t *patch, *nvm_file;
	uint8_t *nvm, event[260];
	uint64_t patch_size = 0, nvm_size = 0;
	uint32_t rom;
	bool ok;

	if (!instantnoodle_bt_power(true) || !instantnoodle_bt_uart_open()) {
		return false;
	}
	h4_done(&own);
	instantnoodle_bt_uart_flush();
	if (!read_version()) {
		say("bt: the chip does not answer at 115200 baud (vendor command 0xfc00/0x19)\n");
		return false;
	}
	say("bt: the chip: product %#x, SoC %#x, ROM %#x, patch %#x (at 115200 baud)\n", chipver.product, chipver.soc,
	    chipver.rom, chipver.patch);
	{
		uint8_t rate = QCA_BAUD_3000000;

		/* no answer is waited for: the chip changes rate at once and answers, if at all, at the new one */
		if (send_command(EDL_SET_BAUDRATE, &rate, 1) != 0) {
			return false;
		}
		delay_for_interval(300, NSEC_PER_MSEC);        /* (qca_set_baudrate(): 300 ms for ROME and QCA6390) */
		if (!instantnoodle_bt_uart_speed(OPER_BAUD)) {
			return false;
		}
		instantnoodle_bt_uart_flush();
		h4_done(&own);
	}
	if (!read_version()) {
		say("bt: the chip does not answer at %u baud\n", OPER_BAUD);
		return false;
	}
	rom = ((((uint32_t)chipver.soc << 16 | chipver.rom) & 0xf00) >> 4) | (chipver.rom & 0xf);
	snprintf(patch_name, sizeof(patch_name), "htbtfw%02x.tlv", rom);
	snprintf(nvm_name, sizeof(nvm_name), "htnv%02x.bin", rom);
	say("bt: at %u baud; the chip's files are %s and %s\n", OPER_BAUD, patch_name, nvm_name);
	if ((patch = firmware(patch_name, &patch_size)) == NULL || (nvm_file = firmware(nvm_name, &nvm_size)) == NULL) {
		return false;
	}
	if (!download(patch_name, patch, patch_size, true)) {
		return false;
	}
	delay_for_interval(10, NSEC_PER_MSEC);         /* (qca_uart_setup(): the chip readies itself for the NVM) */
	if (nvm_size < 4 || nvm_size > 1024 * 1024) {
		say("bt: NVM size %llu rejected\n", nvm_size);
		return false;
	}
	nvm = kalloc_data(nvm_size, Z_WAITOK);
	if (nvm == NULL) {
		return false;
	}
	memcpy(nvm, nvm_file, nvm_size);
	ok = nvm_fix(nvm, nvm_size) && download(nvm_name, nvm, nvm_size, false);
	kfree_data(nvm, nvm_size);
	if (!ok) {
		return false;
	}
	{
		static const uint8_t no_logging[2] = { 0x14, 0x00 };

		if (command(EDL_DISABLE_LOGGING, no_logging, 2, event, 1000) != 0) {
			say("bt: the chip did not take \"logging off\" (0xfc17): carried on\n");
		}
	}
	if (command(0x0c03, NULL, 0, event, 2000) != 0) {
		say("bt: HCI_Reset after the patch failed\n");
		return false;
	}
	if (command(0x1009, NULL, 0, event, 1000) == 0 && event[1] >= 10) {
		bool same = true;

		for (int i = 0; i < 6; i++) {
			same = same && event[6 + i] == address[5 - i];
		}
		say("bt: the chip's address: ..:%02x:%02x, %s\n", event[7], event[6], same ? "the one asked for" : "NOT the one asked for");
	}
	return true;
}

/* bt_test=1: what the controller finds by itself (an LE scan of 4 s, an inquiry of 5 s). */
static void
self_test(void)
{
	uint8_t event[260];
	uint32_t le_reports = 0, inquiry_results = 0, other = 0;
	uint64_t deadline;

	if (command(0x1001, NULL, 0, event, 1000) == 0 && event[1] >= 12) {
		say("bt: test: HCI %u.%u, LMP %u.%u, manufacturer %u\n", event[6], event[7] | event[8] << 8, event[9],
		    event[12] | event[13] << 8, event[10] | event[11] << 8);
	}
	/* Diagnose legacy/extended command selection before exposing the controller to iOS. */
	{
		const uint8_t random[6] = { 1, 2, 3, 4, 5, 0xc0 };
		const uint8_t scan[7] = { 0, 0x30, 0, 0x30, 0, 1, 0 };
		for (int periodic = 0; periodic < 2; periodic++) {
			command(0x0c03, NULL, 0, event, 1000);
			if (periodic) {
				say("bt: test: periodic list size: status %d\n", command(0x204a, NULL, 0, event, 1000));
			}
			say("bt: test: mode periodic=%d random-address status %d\n", periodic,
			    command(0x2005, random, 6, event, 1000));
			say("bt: test: mode periodic=%d legacy-tx-power status %d\n", periodic,
			    command(0x2007, NULL, 0, event, 1000));
			say("bt: test: mode periodic=%d scan-params status %d\n", periodic,
			    command(0x200b, scan, 7, event, 1000));
		}
		command(0x0c03, NULL, 0, event, 1000);
	}
	/* the Supported Commands mask (64 octets), whole, and whether two commands it leaves out work all the same */
	if (command(0x1002, NULL, 0, event, 1000) == 0 && event[1] >= 4 + 64) {
		char hex[3 * 64 + 1];

		for (int i = 0; i < 64; i++) {
			snprintf(hex + 3 * i, 4, "%02x ", event[6 + i]);
		}
		say("bt: test: supported commands: %s\n", hex);
	}
	{
		static const uint8_t enable = 0x01;

		say("bt: test: Read Default Erroneous Data Reporting (0c5a): status %d\n", command(0x0c5a, NULL, 0, event, 1000));
		say("bt: test: Write Default Erroneous Data Reporting (0c5b, 1): status %d\n", command(0x0c5b, &enable, 1, event, 1000));
		say("bt: test: Read Default Erroneous Data Reporting again: status %d, value %d\n", command(0x0c5a, NULL, 0, event, 1000),
		    event[1] >= 5 ? event[6] : -1);
	}
	{
		/* passive, 100 ms interval and window, public address, all advertisers */
		static const uint8_t params[7] = { 0x00, 0xa0, 0x00, 0xa0, 0x00, 0x00, 0x00 };
		static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };   /* + LE meta */
		static const uint8_t on[2] = { 0x01, 0x00 }, off[2] = { 0x00, 0x00 };

		command(0x0c01, mask, 8, event, 1000);
		if (command(0x200b, params, 7, event, 1000) != 0 || command(0x200c, on, 2, event, 1000) != 0) {
			say("bt: test: the LE scan was refused\n");
		} else {
			clock_interval_to_deadline(4000, NSEC_PER_MSEC, &deadline);
			while (mach_absolute_time() < deadline) {
				if (next_event(event, 100) && event[0] == 0x3e && event[2] == 0x02) {
					le_reports += event[3];
				}
			}
			command(0x200c, off, 2, event, 1000);
			say("bt: test: LE scan, 4 s: %u advertising reports\n", le_reports);
		}
	}
	{
		/* the general inquiry access code, 5 x 1.28 s, no limit */
		static const uint8_t inquiry[5] = { 0x33, 0x8b, 0x9e, 0x04, 0x00 };

		if (send_command(0x0401, inquiry, 5) != 0 || !wait_for(EV_COMMAND_STATUS, 0x0401, event, 1000) || event[2] != 0) {
			say("bt: test: the inquiry was refused (status %#x)\n", event[0] == EV_COMMAND_STATUS ? event[2] : 0xff);
			instantnoodle_bt_uart_state("inquiry");
		} else {
			clock_interval_to_deadline(7000, NSEC_PER_MSEC, &deadline);
			while (mach_absolute_time() < deadline) {
				if (!next_event(event, 200)) {
					continue;
				}
				if (event[0] == 0x01) {
					break;          /* Inquiry Complete */
				}
				if (event[0] == 0x02 || event[0] == 0x22 || event[0] == 0x2f) {
					inquiry_results += event[0] == 0x2f ? 1 : event[2];
				} else {
					other++;
				}
			}
			say("bt: test: inquiry, 5 s: %u results (%u other events)\n", inquiry_results, other);
		}
	}
	command(0x0c03, NULL, 0, event, 2000);
}

/* ---- the power thread: on at the start and whenever /dev/btpoweroff is let go, off while it is held ---- */

static void
power_off_locked(void)
{
	chip_ready = false;
	pump_active = false;
	ibs_host = false;
	for (int i = 0; i < 100 && pump_busy; i++) {
		delay_for_interval(5, NSEC_PER_MSEC);
	}
	instantnoodle_bt_uart_close();
	instantnoodle_bt_power(false);
}

static void
power_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool tested = false;

	for (;;) {
		lck_mtx_lock(&power_lock);
		power_request = false;
		if (want_off) {
			if (chip_ready) {
				say("bt: off (/dev/btpoweroff is held)\n");
			}
			power_off_locked();
			bringup_failed = false;
		} else if (!chip_ready) {
			bool ok;

			bringing_up = true;
			bringups++;
			ok = bring_up();
			if (!ok) {
				/* once more from the chip's power off: the ROM starts over */
				say("bt: the bring-up failed; once more from power off\n");
				power_off_locked();
				delay_for_interval(200, NSEC_PER_MSEC);
				ok = bring_up();
			}
			if (!ok) {
				bringup_failures++;
				power_off_locked();
				bringup_failed = true;
				say("bt: THE CONTROLLER IS NOT UP\n");
			} else {
				/* from here on the pump reads, and the host's side of the in-band sleep is on (qca_setup()) */
				h4_done(&trace_in);
				own_mode = true;
				ibs_tx = IBS_TX_ASLEEP;
				ibs_host = true;
				pump_active = true;
				thread_wakeup((event_t)&pump_active);
				say("bt: THE CONTROLLER IS UP (QCA6390 SoC %#x, patch %#x) and iOS's\n", chipver.soc, chipver.patch);
				if (bt_test && !tested) {
					tested = true;
					self_test();
				}
				own_mode = false;
				own_tail = own_head;
				chip_ready = true;
			}
			bringing_up = false;
			thread_wakeup((event_t)&chip_ready);
		}
		if (!power_request) {
			assert_wait((event_t)&power_request, THREAD_UNINT);
			lck_mtx_unlock(&power_lock);
			thread_block(THREAD_CONTINUE_NULL);
		} else {
			lck_mtx_unlock(&power_lock);
		}
	}
}

static void
power_poke(void)
{
	power_request = true;
	thread_wakeup((event_t)&power_request);
}

/* ---- /dev/btpoweroff ----------------------------------------------------------------------------------- */

static int
poweroff_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	if (!want_off) {
		want_off = true;
		power_poke();
	}
	return 0;
}

/* (a character device's close is called at its last close only) */
static int
poweroff_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	want_off = false;
	power_poke();
	return 0;
}

static const struct cdevsw poweroff_cdevsw = {
	.d_open = poweroff_open, .d_close = poweroff_close, .d_read = eno_rdwrt, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- the UART for iOS: kernel control "com.apple.uart.bluetooth" ----------------------------------------- */

static kern_ctl_ref ctl_ref;
static volatile uint32_t client_unit;   /* 0: nobody */
static struct termios client_termios;

static void
termios_default(void)
{
	memset(&client_termios, 0, sizeof(client_termios));
	client_termios.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
	client_termios.c_ispeed = client_termios.c_ospeed = OPER_BAUD;
}

static errno_t
ctl_connect_cb(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	uint64_t deadline;

	if (client_unit != 0) {
		say("bt: a second client for the UART (unit %u) refused: unit %u has it\n", sac->sc_unit, client_unit);
		return EBUSY;
	}
	/* the chip on its way up: waited for, as a real UART would be there at once and the chip answer after its boot */
	clock_interval_to_deadline(8000, NSEC_PER_MSEC, &deadline);
	while (!chip_ready && !want_off && !bringup_failed && mach_absolute_time() < deadline) {
		assert_wait_deadline((event_t)&chip_ready, THREAD_UNINT, deadline);
		if (chip_ready || want_off || bringup_failed) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
	termios_default();
	h4_done(&trace_in);
	h4_done(&trace_out);
	client_unit = sac->sc_unit;
	say("bt: the UART is unit %u's (the chip %s)\n", sac->sc_unit, chip_ready ? "ready" : want_off ? "off" : "NOT up");
	return 0;
}

static errno_t
ctl_disconnect_cb(__unused kern_ctl_ref ref, uint32_t unit, __unused void *unitinfo)
{
	if (unit == client_unit) {
		client_unit = 0;
		say("bt: unit %u let the UART go\n", unit);
	}
	return 0;
}

static LCK_MTX_DECLARE(client_lock, &hk_bt_lck_grp);    /* whole packets to the client, one sender at a time */
static LCK_MTX_DECLARE(send_lock, &hk_bt_lck_grp);      /* the client's packets assembled one at a time */

/* A whole packet (its H4 type first) to the client, waiting while its socket is full. */
static void
to_client(const uint8_t *bytes, uint32_t n)
{
	lck_mtx_lock(&client_lock);
	for (;;) {
		uint32_t unit = client_unit;
		errno_t error;

		if (unit == 0) {
			dropped_unconnected += n;
			break;
		}
		error = ctl_enqueuedata(ctl_ref, unit, (void *)(uintptr_t)bytes, n, 0);
		if (error == 0) {
			break;
		}
		if (error != ENOBUFS) {
			dropped_unconnected += n;
			break;
		}
		/* the client reads slower than the chip talks: the chip is held by RTS meanwhile */
		enqueue_waits++;
		assert_wait_timeout((event_t)&client_unit, THREAD_UNINT, 10, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
	lck_mtx_unlock(&client_lock);
}

/*
 * ---- voice (SCO/eSCO) over HCI ---------------------------------------------------------------------------
 *
 * iOS sets up its voice links (HFP: calls, Voice Memos through a headset) with the legacy Setup and
 * Accept Synchronous Connection (0x0428, 0x0429), which leave the path of the voice data to the
 * controller's default, and then sends and expects that data as H4 SCO packets. On an iPhone, BlueTool's
 * boot script routes its Broadcom chip's voice to the transport; nothing here did that for the QCA6390,
 * whose NVM was made for Android (voice over SLIMbus). Observed in 002629: the eSCO link came up
 * (status 0), bluetoothd sent its first SCO packets, and from then on the chip held CTS for good -- every
 * later write timed out, Voice Memos' recording, mediaserverd and then bluetoothd went down with it
 * (that the default path is not HCI was the inference; 005834, translated, has the chip's voice over HCI and
 * the same stall, whose cause is the pace: see "the voice data's pace" below. The legacy path was not traced.)
 * So each legacy command goes to the chip as its enhanced form (0x043d, 0x043e: Bluetooth Core vol 4
 * part E 7.1.45/7.1.46) with the same link parameters and the data path of both directions 0, HCI;
 * the codec fields as Linux's hci_conn.c (hci_enhanced_setup_sync()) fills them in, which chooses QCA's
 * other path, 1, the same way (hci_qca.c, qca_get_data_path_id()). The chip's Command Status for it
 * goes back to iOS with the legacy opcode; the Synchronous Connection Complete is the same for both.
 * Only what iOS uses is translated: CVSD from 16-bit linear PCM (voice setting 0x0060) and transparent
 * data (0x0063, mSBC that iOS codes itself); anything else goes as it came. bt_sco_hci=0: nothing is.
 */
static uint32_t bt_sco_hci = 1;
static uint32_t sco_setups, sco_accepts;        /* translated; their Command Status still to come */

static void
put_le(uint8_t **p, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; i++) {
		*(*p)++ = i < 4 ? (uint8_t)(value >> (8 * i)) : 0;      /* (a coding format's 5 bytes: its id, then zeros) */
	}
}

/* `packet' (the H4 type first) of `size' bytes, a command from the client: its enhanced form when it is one of the two; the new size. */
static uint32_t
sco_enhance(uint8_t *packet, uint32_t size)
{
	uint16_t opcode = (uint16_t)(packet[1] | packet[2] << 8), voice;
	uint32_t prefix = opcode == 0x0428 ? 2 : 6;     /* the connection handle, or the peer's address */
	uint8_t legacy[21], *p = packet + 4;
	bool transparent;

	if (!bt_sco_hci || !((opcode == 0x0428 && size == 4 + 17) || (opcode == 0x0429 && size == 4 + 21))) {
		return size;
	}
	/* after the prefix: transmit and receive bandwidth (4, 4), max latency 2, voice setting 2, retransmission effort 1, packet type 2 */
	memcpy(legacy, packet + 4, size - 4);
	voice = (uint16_t)(legacy[prefix + 10] | legacy[prefix + 11] << 8);
	if (voice != 0x0060 && voice != 0x0063) {
		return size;
	}
	transparent = voice == 0x0063;
	memcpy(p, legacy, prefix + 8);                  /* the prefix and the air's bandwidths */
	p += prefix + 8;
	for (int i = 0; i < 2; i++) {                   /* the air's coding format: CVSD or transparent */
		put_le(&p, transparent ? 0x03 : 0x02, 5);
	}
	put_le(&p, 60, 2);                              /* codec frame sizes */
	put_le(&p, 60, 2);
	for (int i = 0; i < 2; i++) {                   /* the host's side: bytes per second */
		put_le(&p, transparent ? 8000 : 16000, 4);
	}
	for (int i = 0; i < 2; i++) {                   /* its coding format: transparent or linear PCM */
		put_le(&p, transparent ? 0x03 : 0x04, 5);
	}
	put_le(&p, 16, 2);                              /* coded data sizes, bits */
	put_le(&p, 16, 2);
	*p++ = 2;                                       /* PCM data formats: 2's complement */
	*p++ = 2;
	*p++ = 0;                                       /* PCM sample MSB positions */
	*p++ = 0;
	*p++ = 0;                                       /* the data paths: HCI */
	*p++ = 0;
	*p++ = transparent ? 1 : 16;                    /* transport unit sizes */
	*p++ = transparent ? 1 : 16;
	memcpy(p, legacy + prefix + 8, 2);              /* max latency */
	memcpy(p + 2, legacy + prefix + 13, 2);         /* packet type */
	p[4] = legacy[prefix + 12];                     /* retransmission effort */
	p += 5;
	packet[1] = opcode == 0x0428 ? 0x3d : 0x3e;
	packet[3] = (uint8_t)(p - (packet + 4));
	os_atomic_inc(opcode == 0x0428 ? &sco_setups : &sco_accepts, relaxed);
	say("bt: %s Synchronous Connection (%04x, voice setting %#06x) sent as %04x: voice data over HCI\n",
	    opcode == 0x0428 ? "Setup" : "Accept", opcode, voice, packet[1] | packet[2] << 8);
	return (uint32_t)(p - packet);
}

static void sco_link_up(uint8_t status);

/*
 * An event from the chip (code first): the Command Status of a translated command with the opcode iOS sent;
 * a Synchronous Connection Complete starts the pacing below over.
 */
static void
sco_event(uint8_t *event, uint32_t length)
{
	uint16_t opcode;

	if (event[0] == 0x2c && length >= 3) {
		sco_link_up(event[2]);
		return;
	}
	if (event[0] != EV_COMMAND_STATUS || length < 6) {
		return;
	}
	opcode = (uint16_t)(event[4] | event[5] << 8);
	if (opcode == 0x043d && os_atomic_load(&sco_setups, relaxed) != 0) {
		os_atomic_dec(&sco_setups, relaxed);
		event[4] = 0x28;
	} else if (opcode == 0x043e && os_atomic_load(&sco_accepts, relaxed) != 0) {
		os_atomic_dec(&sco_accepts, relaxed);
		event[4] = 0x29;
	} else {
		return;
	}
	if (event[2] != 0) {
		say("bt: the chip refuses the enhanced synchronous connection %04x: status %#x\n", opcode, event[2]);
	}
}

/*
 * ---- the voice data's pace ----------------------------------------------------------------------------------
 *
 * With its voice over HCI (above), the chip sends one SCO packet per eSCO interval (005834: 120 bytes, 16-bit PCM,
 * every 7.5 ms) and has 4 buffers for the host's (Read_Buffer_Size); iOS turns flow control for them off
 * (Write_Synchronous_Flow_Control_Enable 0), so nothing tells the host when one is free. bluetoothd paces its
 * own: in 005834 it ran 5 packets ahead of the air, logged "Assertion failed: fAudioBytesOutstanding <=
 * MAX_VOICE_SIZE_IN_BYTES" at each, sent one packet of 8 bytes, then 5 more at once -- and the chip held CTS
 * for good, as in 002629 (which of the two it cannot take is not known). Linux's audio servers on these chips
 * send a packet of the size the chip sends for each one it sends (PipeWire's SCO sink); the same is done here,
 * under iOS: its SCO bytes are queued (8 packets at most, the oldest dropped), and go to the chip in packets of
 * the size the chip's last one had, no more than SCO_AHEAD ahead of those it has sent. bt_sco_pace=0: as iOS
 * sends them. Counters in kern.hackios.bt; bt_trace=2 shows what really goes as "}".
 */
#define SCO_AHEAD               2
#define SCO_QUEUE               (8 * 240)
static uint32_t bt_sco_pace = 1;
static LCK_MTX_DECLARE(sco_lock, &hk_bt_lck_grp);
static uint8_t sco_queue[SCO_QUEUE];
static uint32_t sco_have, sco_unit = 120, sco_credit = SCO_AHEAD;
static uint16_t sco_handle;
static uint64_t sco_queued, sco_sent, sco_dropped, sco_from_chip;

static void
sco_link_up(uint8_t status)
{
	lck_mtx_lock(&sco_lock);
	say("bt: voice link %s (status %#x); the last one's SCO bytes: %llu from iOS, %llu sent, %llu dropped; %llu packets from the chip\n",
	    status == 0 ? "up" : "refused", status, sco_queued, sco_sent, sco_dropped, sco_from_chip);
	sco_have = 0;
	sco_credit = SCO_AHEAD;
	lck_mtx_unlock(&sco_lock);
}

/* An SCO packet from the client (header and data, without the H4 type): its bytes queued. */
static void
sco_put(const uint8_t *packet, uint32_t have)
{
	uint16_t handle = (uint16_t)((packet[0] | packet[1] << 8) & 0x0fff);
	uint32_t n = have - 3;

	lck_mtx_lock(&sco_lock);
	if (handle != sco_handle) {
		sco_handle = handle;
		sco_have = 0;
	}
	if (sco_have + n > SCO_QUEUE) {
		uint32_t old = sco_have + n - SCO_QUEUE;

		memmove(sco_queue, sco_queue + old, sco_have - old);
		sco_have -= old;
		sco_dropped += old;
	}
	memcpy(sco_queue + sco_have, packet + 3, n);
	sco_have += n;
	sco_queued += n;
	lck_mtx_unlock(&sco_lock);
}

/* What the credit allows of the queue, to the chip. send_lock held (one sender of client packets at a time). */
static int
sco_flush(void)
{
	uint8_t packet[4 + 255];

	for (;;) {
		uint32_t unit;

		lck_mtx_lock(&sco_lock);
		unit = sco_unit;
		if (sco_credit == 0 || sco_have < unit) {
			lck_mtx_unlock(&sco_lock);
			return 0;
		}
		packet[0] = 0x03;
		packet[1] = (uint8_t)sco_handle;
		packet[2] = (uint8_t)(sco_handle >> 8);
		packet[3] = (uint8_t)unit;
		memcpy(packet + 4, sco_queue, unit);
		memmove(sco_queue, sco_queue + unit, sco_have - unit);
		sco_have -= unit;
		sco_credit--;
		sco_sent += unit;
		lck_mtx_unlock(&sco_lock);
		trace('}', 0x03, packet + 1, 3 + unit);
		if (host_write(packet, 4 + unit) != 0) {
			return EIO;
		}
	}
}

/* An SCO packet from the chip (header first): its size the unit, one more packet allowed. */
static void
sco_chip_sent(const uint8_t *packet, uint32_t have)
{
	if (have < 3 || packet[2] == 0) {
		return;
	}
	lck_mtx_lock(&sco_lock);
	sco_unit = packet[2];
	if (sco_credit < SCO_AHEAD) {
		sco_credit++;
	}
	sco_from_chip++;
	lck_mtx_unlock(&sco_lock);
}

static uint8_t send_packet[1 + sizeof(((struct h4 *)0)->data)];

static errno_t
ctl_send_cb(__unused kern_ctl_ref ref, uint32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	uint8_t buffer[1024];
	size_t length = 0, at = 0;
	errno_t error = 0;

	for (mbuf_t piece = m; piece != NULL; piece = mbuf_next(piece)) {
		length += mbuf_len(piece);
	}
	if (unit != client_unit || !chip_ready) {
		mbuf_freem(m);
		return unit != client_unit ? ENOTCONN : ENXIO;
	}
	/* Whole packets to the chip, as the parser finds their ends. */
	lck_mtx_lock(&send_lock);
	while (at < length && error == 0) {
		size_t n = length - at < sizeof(buffer) ? length - at : sizeof(buffer);

		error = mbuf_copydata(m, at, n, buffer);
		if (error != 0) {
			break;
		}
		for (size_t i = 0; i < n && error == 0; i++) {
			if (!h4_feed(&trace_out, buffer[i])) {
				continue;
			}
			packets_out++;
			trace('>', trace_out.type, trace_out.data, trace_out.have);
			if (trace_out.overlong) {
				say("bt: a packet of type %u from the client is longer than %zu bytes: dropped\n", trace_out.type,
				    sizeof(trace_out.data));
			} else {
				uint32_t size = 1 + trace_out.have;

				send_packet[0] = trace_out.type;
				memcpy(send_packet + 1, trace_out.data, trace_out.have);
				if (trace_out.type == 0x01) {
					size = sco_enhance(send_packet, size);
				}
				if (trace_out.type == 0x03 && bt_sco_pace && trace_out.have >= 3) {
					sco_put(trace_out.data, trace_out.have);
					error = sco_flush();
				} else {
					error = host_write(send_packet, size);
				}
			}
			h4_done(&trace_out);
		}
		bytes_out += n;
		at += n;
	}
	lck_mtx_unlock(&send_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
ctl_setopt_cb(__unused kern_ctl_ref ref, uint32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	if (unit != client_unit) {
		return ENOTCONN;
	}
	if ((opt == (int)TIOCSETA || opt == (int)TIOCSETAW || opt == (int)TIOCSETAF) && len >= sizeof(struct termios)) {
		struct termios *t = (struct termios *)data;
		uint32_t speed = (uint32_t)t->c_ospeed;

		if (speed != (uint32_t)client_termios.c_ospeed || (t->c_cflag & CRTSCTS) != (client_termios.c_cflag & CRTSCTS)) {
			say("bt: unit %u sets the UART to %u baud%s\n", unit, speed, (t->c_cflag & CRTSCTS) ? ", RTS/CTS" :
			    " WITHOUT flow control (kept on)");
		}
		if (speed != 0 && !instantnoodle_bt_uart_speed(speed)) {
			say("bt: %u baud cannot be had\n", speed);
			return EINVAL;
		}
		memcpy(&client_termios, t, sizeof(client_termios));
		if (opt == (int)TIOCSETAF) {
			instantnoodle_bt_uart_flush();
		}
		return 0;
	}
	say("bt: unit %u: socket option %#x (%zu bytes) not known: refused\n", unit, opt, len);
	return ENOPROTOOPT;
}

static errno_t
ctl_getopt_cb(__unused kern_ctl_ref ref, uint32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	if (unit != client_unit) {
		return ENOTCONN;
	}
	if (opt == (int)TIOCGETA && *len >= sizeof(struct termios)) {
		if (data != NULL) {
			memcpy(data, &client_termios, sizeof(client_termios));
		}
		*len = sizeof(client_termios);
		return 0;
	}
	say("bt: unit %u: socket option %#x asked for, not known\n", unit, opt);
	return ENOPROTOOPT;
}

static void
ctl_rcvd_cb(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *unitinfo, __unused int flags)
{
	thread_wakeup((event_t)&client_unit);
}

/*
 * What the chip sends, read as it comes (the pump, from the end of the bring-up): its in-band sleep
 * bytes answered and taken out, every packet traced and passed to the client whole -- or, while the kernel itself talks to the chip (own_mode), its events to the own queue.
 */
static void
pump_thread(__unused void *parameter, __unused wait_result_t result)
{
	static uint8_t buffer[2048], out[1 + sizeof(((struct h4 *)0)->data)];

	for (;;) {
		uint32_t n;

		if (!pump_active) {
			assert_wait_timeout((event_t)&pump_active, THREAD_UNINT, 200, NSEC_PER_MSEC);
			if (pump_active) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			continue;
		}
		pump_busy = true;
		if (!pump_active) {
			pump_busy = false;
			continue;
		}
		n = instantnoodle_bt_uart_read(buffer, sizeof(buffer), 50);
		ibs_idle();
		pump_busy = false;
		if (n == 0) {
			continue;
		}
		bytes_in += n;
		for (uint32_t i = 0; i < n; i++) {
			if (trace_in.type == 0 && is_ibs(buffer[i])) {
				ibs_from_chip(buffer[i]);
				continue;
			}
			if (!h4_feed(&trace_in, buffer[i])) {
				continue;
			}
			packets_in++;
			trace('<', trace_in.type, trace_in.data, trace_in.have);
			if (trace_in.overlong) {
				say("bt: a packet of type %u from the chip is longer than %zu bytes: dropped\n", trace_in.type,
				    sizeof(trace_in.data));
			} else if (own_mode) {
				if (trace_in.type == 0x04) {
					own_put(trace_in.data, trace_in.have);
				}
			} else {
				if (trace_in.type == 0x04) {
					sco_event(trace_in.data, trace_in.have);
				} else if (trace_in.type == 0x03 && bt_sco_pace) {
					sco_chip_sent(trace_in.data, trace_in.have);
					/* the next packet at once, when no client write holds the sender (which then sends it) and the chip is awake */
					if ((!ibs_host || ibs_tx == IBS_TX_AWAKE) && lck_mtx_try_lock(&send_lock)) {
						sco_flush();
						lck_mtx_unlock(&send_lock);
					}
				}
				out[0] = trace_in.type;
				memcpy(out + 1, trace_in.data, trace_in.have);
				to_client(out, 1 + trace_in.have);
			}
			h4_done(&trace_in);
		}
	}
}

static void
ctl_start(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	memset(&reg, 0, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_REG_EXTENDED;
	reg.ctl_sendsize = 64 * 1024;
	reg.ctl_recvsize = 256 * 1024;
	reg.ctl_connect = ctl_connect_cb;
	reg.ctl_disconnect = ctl_disconnect_cb;
	reg.ctl_send = ctl_send_cb;
	reg.ctl_setopt = ctl_setopt_cb;
	reg.ctl_getopt = ctl_getopt_cb;
	reg.ctl_rcvd = ctl_rcvd_cb;
	error = ctl_register(&reg, &ctl_ref);
	if (error != 0) {
		say("bt: the kernel control com.apple.uart.bluetooth cannot be registered: %d\n", error);
	}
}

/* ---- the sysctls ------------------------------------------------------------------------------------------ */

static int
bt_sysctl_text(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[1024];
	uint32_t irqs = 0, overruns = 0, dropped = 0, errors = 0;

	instantnoodle_bt_uart_counters(&irqs, &overruns, &dropped, &errors);
	snprintf(text, sizeof(text),
	    "state %s; chip product %#x SoC %#x ROM %#x patch %#x; bring-ups %u (failed %u); client unit %u; "
	    "in %llu bytes %llu packets, out %llu bytes %llu packets; dropped (no client) %llu, enqueue waits %llu, resyncs %u; "
	    "uart irqs %u overruns %u ring-drops %u line-errors %u; ibs: wakes sent %u acked %u failed %u, chip wakes %u sleeps %u, "
	    "host sleeps %u, tx %s; sco: %llu bytes from iOS, %llu sent, %llu dropped, %llu packets from the chip, unit %u\n",
	    chip_ready ? "up" : bringing_up ? "coming-up" : want_off ? "off" : bringup_failed ? "FAILED" : "down",
	    chipver.product, chipver.soc, chipver.rom, chipver.patch, bringups, bringup_failures, client_unit, bytes_in,
	    packets_in, bytes_out, packets_out, dropped_unconnected, enqueue_waits, h4_resyncs, irqs, overruns, dropped, errors,
	    ibs_wakes_sent, ibs_acks_got, ibs_wake_failures, ibs_wakes_got, ibs_sleeps_got, ibs_sleeps_sent,
	    ibs_tx == IBS_TX_AWAKE ? "awake" : ibs_tx == IBS_TX_WAKING ? "waking" : "asleep", sco_queued, sco_sent, sco_dropped,
	    sco_from_chip, sco_unit);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, bt, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, bt_sysctl_text, "A",
    "Bluetooth: the controller's state, versions and counters");
SYSCTL_UINT(_kern_hackios, OID_AUTO, bt_trace, CTLFLAG_RW | CTLFLAG_LOCKED, &bt_trace, 0,
    "1: HCI commands and events in the kernel's log, 2: and ACL/SCO headers");

/* ---- the start ---------------------------------------------------------------------------------------------- */

bool
hk_bt_wanted(void)
{
	uint32_t on = 0;

	return instantnoodle_board() && PE_parse_boot_argn("bt", &on, sizeof(on)) && on == 1;
}

/* From HKBluetooth.cpp: the address of the device tree's /chosen (mac-address-bluetooth0), as shown, first byte first. */
void
hk_bt_set_address(const uint8_t from[6])
{
	memcpy(address, from, 6);
	address_known = true;
	thread_wakeup((event_t)&address_known);
}

static void
start_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t probe = 0;
	thread_t thread;

	PE_parse_boot_argn("bt_probe", &probe, sizeof(probe));
	if (probe) {
		instantnoodle_bt_probe();
	}
	/* the address comes with the IOService (HKBluetooth.cpp), once IOKit's platform has started */
	for (int i = 0; i < 100 && !address_known; i++) {
		assert_wait_timeout((event_t)&address_known, THREAD_UNINT, 100, NSEC_PER_MSEC);
		if (address_known) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
	if (!address_known) {
		say("bt: the device tree's address never came: the loader's default is used\n");
	}
	if (kernel_thread_start(pump_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	power_thread(NULL, THREAD_AWAKENED);
}

/* While the system starts (hk_input.c), after devfs and the socket layer: bt=1 on board=instantnoodle. */
void
hk_bt_init(void)
{
	int major;
	thread_t thread;

	if (!hk_bt_wanted()) {
		return;
	}
	PE_parse_boot_argn("bt_test", &bt_test, sizeof(bt_test));
	PE_parse_boot_argn("bt_trace", &bt_trace, sizeof(bt_trace));
	PE_parse_boot_argn("bt_sco_hci", &bt_sco_hci, sizeof(bt_sco_hci));
	PE_parse_boot_argn("bt_sco_pace", &bt_sco_pace, sizeof(bt_sco_pace));
	bt_mode = 1;
	major = cdevsw_add(-1, &poweroff_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btpoweroff") == NULL) {
		say("bt: /dev/btpoweroff cannot be made\n");
	}
	termios_default();
	ctl_start();
	if (kernel_thread_start(start_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
