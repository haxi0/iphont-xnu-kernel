/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive, except on two phones: a "swap" copies the
 * pixels of the surfaces of its layers into that memory, and "vsync" is a timer -- or, on the phone with
 * fb_scan=1, the panel's own scan (bsd/dev/hk_display.c): then each frame is copied where
 * the scan lets it land whole in one refresh (no tearing), the rate is the panel's (120 Hz), and
 * the refresh notifications come when the panel's frames begin. On the OnePlus 8 (a command-mode panel)
 * each frame is put into one of two framebuffers -- scaled there by the window server's GPU, or copied
 * here -- and pushed to the panel by its MDP on the panel's TE (fb_push, below).
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void hk_tl(const char * format, ...) __printflike(1, 2);
/* the panel's scan (bsd/dev/hk_display.c; sys/hk_display.h, which iokit/ does not see) */
struct hk_display_scan {
	int ok;
	uint32_t lines, shown, millihz;
	uint64_t line_ns, frame_ns;
};
extern "C" int hk_display_scan_start(void);
extern "C" const struct hk_display_scan * hk_display_scan(void);
extern "C" int hk_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns);
extern "C" uint64_t hk_display_next_frame_ns(void);

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
	kFBMethodRedwoodPanelAcquire    = 200,  /* ours (fb_gpu, userland/src/eagl.c): out: index, surface, width, height, bytes a row */
	kFBMethodRedwoodPanelReady      = 201,  /* ours: in: index, the surface whose frame was scaled into it */
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* discard a frame whose GPU fence does not complete */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/hk_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static uint32_t gPanelHz;               /* the OnePlus 8 without fb_hz=: its panel's 90 (start()) */
/* the panel's own rate, when its scan can be read and fb_hz= does not say otherwise (IOMobileFramebuffer::start) */
static uint32_t gScanHz;

static inline uint32_t
refreshHz(void)
{
	if (gPanelHz != 0) {
		return gPanelHz;
	}
	if (gScanHz != 0) {
		return gScanHz;
	}
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}

/* SM8250 handoff capture (fb_probe=1). Offsets: live DTS and Linux dpu_6_0_sm8250,
 * whose MDP-relative offsets start at MDSS + 0x1000. No scanout/DSC/TE writes. */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern "C" void gic_v3_spi_mask(uint32_t intid);
static vm_offset_t gDPU;

static uint32_t
displayReg(vm_offset_t base, unsigned offset)
{
	return *(volatile uint32_t *)(base + offset);
}

extern "C" void loader_log_line(const char *line);

/* The loader's log keeps these; the kernel's own ring is overwritten by iOS long before the run ends. */
static void
displayLog(const char * format, ...)
{
	char line[512];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static bool
instantnoodle(void)
{
	char board[32] = "";

	PE_parse_boot_argn("board", board, sizeof(board));
	return strcmp(board, "instantnoodle") == 0;
}

/* The MDSS registers (gDPU), once: only on a display the firmware left powered, with its register clock on. */
static bool
mapDisplay(void)
{
	if (gDPU != 0) {
		return true;
	}
	vm_offset_t gcc = ml_io_map(0x00100000, 0x10000);
	uint32_t bus = displayReg(gcc, 0xb00c);

	if (!(bus & 1)) {
		displayLog("display: GCC_DISP_AHB_CLK %#x disabled\n", bus);
		return false;
	}
	vm_offset_t cc = ml_io_map(0x0af00000, 0x4000);
	uint32_t power = displayReg(cc, 0x3000), ahb = displayReg(cc, 0x2080);

	displayLog("display: SM8250 GCC_DISP_AHB %#x GDSCR %#x MDSS_AHB %#x\n", bus, power, ahb);
	if (!(power & (1u << 31))) {
		displayLog("display: MDSS power off\n");
		return false;
	}
	/* Enable only the register-access clock; never power up an inactive display. */
	*(volatile uint32_t *)(cc + 0x2080) = ahb | 1;
	__asm__ volatile ("dsb osh" ::: "memory");
	unsigned retry;
	for (retry = 0; retry < 1000 && (displayReg(cc, 0x2080) & (1u << 31)); retry++) {
		IODelay(1);
	}
	if (retry == 1000) {
		displayLog("display: MDSS register clock did not start\n");
		return false;
	}
	gDPU = ml_io_map(0x0ae00000, 0x90000);
	return true;
}

static void
probeDisplay(void)
{
	uint32_t probe = 0;

	PE_parse_boot_argn("fb_probe", &probe, sizeof(probe));
	if (!probe || !instantnoodle() || !mapDisplay()) {
		return;
	}
	displayLog("display: boot framebuffer pa %#llx %ux%u rowbytes %u\n", vinfo.v_physaddr, vinfo.v_width, vinfo.v_height,
	    vinfo.v_rowbytes);
	/* MDP top (MDSS + 0x1000): INTF_SEL, INTR_EN/STATUS, INTR2_EN/STATUS, VSYNC_SEL */
	displayLog("display: HW %#x mdss-intr %#x INTF_SEL %#x INTR en %#x st %#x INTR2 en %#x st %#x VSYNC_SEL %#x\n",
	    displayReg(gDPU, 0), displayReg(gDPU, 0x10), displayReg(gDPU, 0x1004), displayReg(gDPU, 0x1010),
	    displayReg(gDPU, 0x1014), displayReg(gDPU, 0x1008), displayReg(gDPU, 0x100c), displayReg(gDPU, 0x1414));
	for (unsigned i = 0; i < 6; i++) {
		unsigned c = 0x2000 + i * 0x200;

		displayLog("display: CTL%u top %#x flush %#x start %#x prep %#x intf %#x dsc %#x m3d %#x fetch %#x intf-flush %#x "
		    "layers %#x %#x %#x %#x %#x %#x ext %#x %#x\n", i,
		    displayReg(gDPU, c + 0x14), displayReg(gDPU, c + 0x18), displayReg(gDPU, c + 0x1c), displayReg(gDPU, c + 0xd0),
		    displayReg(gDPU, c + 0xf4), displayReg(gDPU, c + 0xe8), displayReg(gDPU, c + 0xe4), displayReg(gDPU, c + 0xfc),
		    displayReg(gDPU, c + 0x110), displayReg(gDPU, c), displayReg(gDPU, c + 4), displayReg(gDPU, c + 8),
		    displayReg(gDPU, c + 12), displayReg(gDPU, c + 16), displayReg(gDPU, c + 0x24), displayReg(gDPU, c + 0x40),
		    displayReg(gDPU, c + 0x44));
	}
	static const unsigned pipes[] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };
	for (unsigned i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++) {
		unsigned p = pipes[i];

		displayLog("display: SSPP@%#x size %#x img %#x xy %#x out %#x oxy %#x addr %#x %#x stride %#x fmt %#x unpack %#x op %#x "
		    "fetch %#x sw-status %#x\n", p,
		    displayReg(gDPU, p), displayReg(gDPU, p + 4), displayReg(gDPU, p + 8), displayReg(gDPU, p + 0xc),
		    displayReg(gDPU, p + 0x10), displayReg(gDPU, p + 0x14), displayReg(gDPU, p + 0x18), displayReg(gDPU, p + 0x24),
		    displayReg(gDPU, p + 0x30), displayReg(gDPU, p + 0x34), displayReg(gDPU, p + 0x38), displayReg(gDPU, p + 0x48),
		    displayReg(gDPU, p + 0x70));
	}
	for (unsigned i = 0; i < 4; i++) {
		unsigned l = 0x45000 + i * 0x1000, p = 0x71000 + i * 0x800;

		displayLog("display: LM%u op %#x out %#x border %#x; PP%u te %#x dsc %#x autorefresh %#x line %#x\n", i,
		    displayReg(gDPU, l), displayReg(gDPU, l + 4), displayReg(gDPU, l + 8), i, displayReg(gDPU, p),
		    displayReg(gDPU, p + 0xa0), displayReg(gDPU, p + 0x30), displayReg(gDPU, p + 0x2c));
	}
	for (unsigned i = 0; i < 2; i++) {
		unsigned d = 0x81000 + i * 0x400;

		displayLog("display: DSC%u common %#x enc %#x size %#x\n", i, displayReg(gDPU, d), displayReg(gDPU, d + 4),
		    displayReg(gDPU, d + 0x14));
	}
	for (unsigned i = 1; i <= 2; i++) {
		unsigned p = 0x6b000 + i * 0x800, t = 0x6e700 + i * 0x100;

		displayLog("display: INTF%u engine %#x config %#x config2 %#x cmd-trigger %#x mux %#x status %#x intr en %#x st %#x "
		    "tear-intr en %#x st %#x\n", i,
		    displayReg(gDPU, p), displayReg(gDPU, p + 4), displayReg(gDPU, p + 0x60), displayReg(gDPU, p + 0x84),
		    displayReg(gDPU, p + 0x25c), displayReg(gDPU, p + 0x26c), displayReg(gDPU, p + 0x1c0),
		    displayReg(gDPU, p + 0x1c4), displayReg(gDPU, t), displayReg(gDPU, t + 4));
		displayLog("display: INTF%u TE vsync-sel %#x en %#x vsync %#x height %#x wrcount %#x init %#x thresh %#x start %#x "
		    "rd-irq %#x wr-irq %#x out %#x line %#x autorefresh %#x frame %u\n", i,
		    displayReg(gDPU, p + 0x280), displayReg(gDPU, p + 0x284), displayReg(gDPU, p + 0x288), displayReg(gDPU, p + 0x28c),
		    displayReg(gDPU, p + 0x290), displayReg(gDPU, p + 0x294), displayReg(gDPU, p + 0x29c), displayReg(gDPU, p + 0x2a0),
		    displayReg(gDPU, p + 0x2a4), displayReg(gDPU, p + 0x2a8), displayReg(gDPU, p + 0x2ac), displayReg(gDPU, p + 0x2b0),
		    displayReg(gDPU, p + 0x2b4), displayReg(gDPU, p + 0xac));
	}
	/* Is anything still moving on its own? Sample the INTF1 counters over 100 ms. */
	for (unsigned i = 0; i < 5; i++) {
		displayLog("display: t+%ums INTF1 frame %u line %u te-out %#x te-line %#x intr-st %#x tear-st %#x top-st %#x\n", i * 25,
		    displayReg(gDPU, 0x6b8ac), displayReg(gDPU, 0x6b8b0), displayReg(gDPU, 0x6baac), displayReg(gDPU, 0x6bab0),
		    displayReg(gDPU, 0x6b9c4), displayReg(gDPU, 0x6e804), displayReg(gDPU, 0x1014));
		IODelay(25000);
	}
	/* Which address space the MDP fetches in: apps SMMU stream matches for its streams (0x820/0x821, mask 0x402).
	 * Global registers only: reading the display's context bank or its tables sends the phone to CrashDump. */
	vm_offset_t smmu = ml_io_map(0x15000000, 0x2000);
	uint32_t scr0 = displayReg(smmu, 0), matches = displayReg(smmu, 0x20) & 0xff, wide = scr0 & (1u << 3);
	displayLog("display: apps SMMU scr0 %#x, %u stream matches\n", scr0, matches);
	for (uint32_t n = 0; n < matches && n < 256; n++) {
		uint32_t smr = displayReg(smmu, 0x800 + 4 * n), s2cr = displayReg(smmu, 0xc00 + 4 * n);
		uint32_t id = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);
		bool valid = wide ? (s2cr & (1u << 10)) : (smr >> 31);

		if (valid && (((id ^ 0x820) & ~(mask | 0x403)) == 0)) {
			displayLog("display: SMR%u id %#x mask %#x s2cr %#x (type %u cb %u)\n", n, id, mask, s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		}
	}
}

/*
 * SM8250 command-mode presentation (instantnoodle; boot-arg fb_push=0: the old way).
 * The firmware leaves CTL0 -> VIG0/VIG1 -> LM0/LM1 -> dual DSC -> INTF1 in command mode, with INTF1's
 * tear check on and autorefresh fetching the boot framebuffer on every TE (fb_probe=1 shows it), so a
 * CPU copy into that memory is sent to the panel half written. Instead autorefresh goes off and each
 * frame is pushed once, after its copy (CTL0 flush + start): the tear check holds the transfer to the
 * panel's TE, and PINGPONG_0's done interrupt says when the memory may be written again. The panel
 * keeps showing its own RAM in between. Offsets are MDSS-relative (Linux dpu_6_0_sm8250 + 0x1000).
 */
#define MDP_INTR_EN             0x1010
#define MDP_INTR_STATUS         0x1014
#define MDP_INTR_CLEAR          0x1018
#define MDP_INTR_PP0_DONE       (1u << 8)
#define CTL0_TOP                0x2014
#define CTL0_FLUSH              0x2018
#define CTL0_START              0x201c
#define CTL0_INTF_ACTIVE        0x20f4
#define CTL_TOP_CMD_MODE        (1u << 17)
#define CTL_FLUSH_FRAME         (1u << 0 | 1u << 1 | 1u << 17)  /* VIG0, VIG1, the CTL itself */
#define VIG0_SRC0_ADDR          0x5014
#define VIG1_SRC0_ADDR          0x7014
#define INTF1_TE_EN             0x6ba84
#define INTF1_TE_VSYNC          0x6ba88
#define INTF1_TE_EXTERNAL       (1u << 20)
#define INTF1_TE_LINE           0x6bab0
#define INTF1_AUTOREFRESH       0x6bab4
#define MDSS_INTID              (83 + 32)
#define INTF1_TEAR_INTR_EN      0x6e800         /* (MDP_INTF_TEAR_OFF(1)) */
#define INTF1_TEAR_INTR_STATUS  0x6e804
#define INTF1_TEAR_INTR_CLEAR   0x6e808
#define TEAR_RD_PTR             (1u << 2)       /* the panel's read pointer at rd_ptr_irq: once a TE */
#define PUSH_TIMEOUT_MS         100                     /* six refreshes: the panel or the MDP has stopped */

static TUNABLE(uint32_t, gPush, "fb_push", 2);   /* 2: two framebuffers, 1: one, 0: the firmware's autorefresh */
static TUNABLE(uint32_t, gPushIrq, "fb_push_irq", 1);   /* 0: PINGPONG_0's done bit polled, the MDSS interrupt left alone */
static uint32_t gAutorefresh;           /* the firmware's setting, back for the loader (hk_display_quiesce) */
/*
 * fb_push=2: a second framebuffer at DOUBLE_PHYS (in the firmware's display region, below the DFPS data at
 * 0x9e300000; the display SMMU maps it, seen on the phone): a frame is copied into the one the panel is not
 * being sent while the other is, and the pipes are pointed at it with the push.
 */
#define DOUBLE_PHYS             0x9ca00000ull
#define SPLASH_END              0x9e300000ull   /* dt: cont_splash_region@9c000000, 0x2300000 long: the display's memory */
#define VIG0_SRC_YSTRIDE0       0x5024          /* (Linux dpu_hw_sspp.c: SSPP_SRC_YSTRIDE0, plane 0 in 15:0) */
#define VIG1_SRC_YSTRIDE0       0x7024
/*
 * fb_gpu=1 (the default): with two framebuffers, the window server's GPU scales each frame into one of them
 * itself (userland/src/eagl.c) and that one is pushed as it is: nothing is copied here (IOMobileFramebuffer::
 * fPanel). Its rows are then a multiple of 64 bytes, as the GPU's render targets need: 4352 bytes for 1080
 * pixels instead of the firmware's 4320, which the pipes are told; the firmware's pitch goes back with
 * the boot framebuffer (pushStop()).
 */
static TUNABLE(uint32_t, gPanelGpu, "fb_gpu", 1);
static uint32_t gStride[2];             /* VIG0 and VIG1's SRC_YSTRIDE0 as the firmware left them; 0: not changed */
static volatile uint8_t * gPanelBase;   /* ... and the boot framebuffer mapped whole at the GPU's pitch, */
static uint32_t gPanelRowBytes, gPanelRows;     /* ... that pitch, and its rows */
extern "C" uint64_t loader_screen[2];  /* (osfmk/arm64/loader_return.c) what the panel shows, for the loader's screenshots */
static bool gDouble;
static uint32_t gTeVsync;               /* INTF1's TE counter setting from the firmware; 0: not changed */
static bool gPushing;                   /* autorefresh off: frames are pushed */
static uint64_t gPushed;                /* when the last push was started; 0: none in flight */
static uint64_t gPushDone;              /* when PINGPONG_0 finished it (interrupt); 0: not yet */
static uint32_t gPushId;                /* the swap it carries (touch_lat=1 lines) */
static bool gPushIrqLive;               /* the interrupt owns the done bit; false: pushWait() polls and clears it */
static uint32_t gPushIrqEmpty;          /* interrupts in a row with nothing of ours pending */
/*
 * The panel's TE as it happens (the tear check's read-pointer interrupt): the last one and the measured
 * period, for the window server's frame clock to follow the panel instead of a timer of its own, which
 * drifts against it and every so often lets a frame miss its refresh (IOMobileFramebufferUserClient::armVSync()).
 */
static uint64_t gTeTime, gTePeriod;

static void
displayWrite(unsigned offset, uint32_t value)
{
	*(volatile uint32_t *)(gDPU + offset) = value;
}

/* Interrupt context: take PINGPONG_0's done, wake the presenter. Anything else of the MDSS is not ours. */
static void
pushInterrupt(void * context __unused, uint32_t intid)
{
	uint32_t status = displayReg(gDPU, MDP_INTR_STATUS) & displayReg(gDPU, MDP_INTR_EN);
	uint32_t tear = displayReg(gDPU, INTF1_TEAR_INTR_STATUS) & displayReg(gDPU, INTF1_TEAR_INTR_EN);

	if (tear != 0) {
		uint64_t now = mach_absolute_time(), last = gTeTime, period = gTePeriod;

		displayWrite(INTF1_TEAR_INTR_CLEAR, tear);
		if (last != 0 && now - last > period / 2 && now - last < period * 3 / 2) {
			__atomic_store_n(&gTePeriod, (period * 7 + (now - last)) / 8, __ATOMIC_RELAXED);
		}
		__atomic_store_n(&gTeTime, now, __ATOMIC_RELEASE);
		gPushIrqEmpty = 0;
		if (status == 0) {
			return;
		}
	}
	if (status == 0) {
		/* a source of the MDSS that is not ours holds the line: give the interrupt up, the presenter polls */
		if (++gPushIrqEmpty == 64) {
			gic_v3_spi_mask(intid);
			__atomic_store_n(&gPushIrqLive, false, __ATOMIC_RELEASE);
		}
		return;
	}
	gPushIrqEmpty = 0;
	displayWrite(MDP_INTR_CLEAR, status);
	if (status & MDP_INTR_PP0_DONE) {
		__atomic_store_n(&gPushDone, mach_absolute_time(), __ATOMIC_RELEASE);
		thread_wakeup((event_t) &gPushDone);
	}
}

/* The panel shows the boot framebuffer again on its own (to give the display back, or after a failure). */
static void
pushStop(void)
{
	displayWrite(MDP_INTR_EN, displayReg(gDPU, MDP_INTR_EN) & ~MDP_INTR_PP0_DONE);
	displayWrite(INTF1_TEAR_INTR_EN, displayReg(gDPU, INTF1_TEAR_INTR_EN) & ~TEAR_RD_PTR);
	gTeTime = 0;
	if (gTeVsync != 0) {
		displayWrite(INTF1_TE_VSYNC, gTeVsync);         /* (the panel stays at 90 Hz: TE resynchronizes the count) */
	}
	if (gDouble) {
		/* the boot framebuffer again, latched by the first autorefresh frame */
		displayWrite(VIG0_SRC0_ADDR, (uint32_t) vinfo.v_physaddr);
		displayWrite(VIG1_SRC0_ADDR, (uint32_t) vinfo.v_physaddr);
		if (gStride[0] != 0) {
			/* what the boot framebuffer holds, to the firmware's pitch: rows only move down in memory */
			for (uint32_t y = 1; gPanelBase != NULL && y < gPanelRows; y++) {
				for (uint32_t x = 0; x < vinfo.v_rowbytes; x += 4) {
					*(volatile uint32_t *) (gPanelBase + (size_t) y * vinfo.v_rowbytes + x) =
					    *(volatile uint32_t *) (gPanelBase + (size_t) y * gPanelRowBytes + x);
				}
			}
			__asm__ volatile ("dsb sy" ::: "memory");
			displayWrite(VIG0_SRC_YSTRIDE0, gStride[0]);
			displayWrite(VIG1_SRC_YSTRIDE0, gStride[1]);
			gStride[0] = 0;
		}
		displayWrite(CTL0_FLUSH, CTL_FLUSH_FRAME);
		gDouble = false;
	}
	displayWrite(INTF1_AUTOREFRESH, gAutorefresh);
	loader_screen[0] = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	gPushing = false;
}

/*
 * 90 Hz (boot-arg fb_hz=90): the panel's own switch, as Android's device tree sends it for timing@1 of
 * "samsung ana6705 fhd cmd mode dsc dsi panel" (timing@0 is 60 Hz: 60 00 and e6 02). The DSI clock is
 * the same for both; the panel paces TE. DCS long writes through DSI0's command DMA (Linux dsi_host.c,
 * 6G registers 4 bytes in). The DMA reads through the display SMMU, so the packets go in the boot
 * framebuffer's last page past its last pixel, mapped with it.
 */
#define DSI0_PHYS               0x0ae94000
#define DSI_REG(offset)         ((offset) + 4)
#define DSI_CTRL                DSI_REG(0x000)
#define DSI_CMD_DMA_CTRL        DSI_REG(0x038)
#define DSI_CMD_DMA_LOW_POWER   (1u << 26)
#define DSI_DMA_BASE            DSI_REG(0x044)
#define DSI_DMA_LEN             DSI_REG(0x048)
#define DSI_TRIG_CTRL           DSI_REG(0x080)
#define DSI_TRIG_DMA            DSI_REG(0x08c)
#define DSI_INTR_CTRL           DSI_REG(0x10c)
#define DSI_CMD_DMA_DONE        (1u << 0)
#define INTF1_TE_COUNT          0x6ba98         /* [31:16] TEs seen (INTF_TEAR_INT_COUNT_VAL) */

static const uint8_t k90Hz[] = {                /* length, then the bytes: one DCS long write each */
	3, 0xf0, 0x5a, 0x5a, 2, 0x60, 0x10, 3, 0xf0, 0xa5, 0xa5, 3, 0xf0, 0x5a, 0x5a, 2, 0xb0, 0x69,
	2, 0xe6, 0x00, 2, 0xb0, 0x77, 2, 0xe6, 0xb3, 3, 0xf0, 0xa5, 0xa5, 0,
};
static const uint8_t k60Hz[] = {                /* timing@0: back to the firmware's rate (the panel keeps 90 across runs) */
	3, 0xf0, 0x5a, 0x5a, 2, 0x60, 0x00, 3, 0xf0, 0xa5, 0xa5, 3, 0xf0, 0x5a, 0x5a, 2, 0xb0, 0x69,
	2, 0xe6, 0x02, 2, 0xb0, 0x77, 2, 0xe6, 0xb3, 3, 0xf0, 0xa5, 0xa5, 0,
};

/* TE pulses the panel has sent so far (INTF1's read pointer counter, 16 bits). */
static inline uint32_t
teCount(void)
{
	return displayReg(gDPU, INTF1_TE_COUNT) >> 16;
}

/* TEs in 100 ms, from INTF1's read pointer counter (it counts the panel's TE pulses). */
static unsigned
teRate(void)
{
	uint32_t before = teCount();

	IODelay(100000);
	return ((teCount() - before) & 0xffff) * 10;
}

/*
 * Where the packets go: a page of the display's memory (cont_splash_region, which the display SMMU maps as the
 * firmware left it) between the first framebuffer -- at the GPU's pitch it ends at 0x9c9f7000 -- and the second.
 * Past the boot framebuffer's last pixel, as before, would be inside the first framebuffer once its rows are
 * the GPU's 4352 bytes: a speck of packet bytes at the bottom of the screen.
 */
#define DSI_DMA_PHYS            (DOUBLE_PHYS - 0x1000)

static vm_offset_t gDSI, gDsiDma;

static bool
dsiMap(void)
{
	if (gDSI == 0) {
		gDSI = ml_io_map(DSI0_PHYS, 0x1000);
	}
	if (gDsiDma == 0) {
		gDsiDma = ml_io_map(DSI_DMA_PHYS, 0x1000);
	}
	return gDSI != 0 && gDsiDma != 0;
}

/* One DCS packet through DSI0's command DMA: 0x39 a long write, 0x05/0x15 a short one (the command and a parameter). */
static bool
dsiPacket(vm_offset_t dsi, uint8_t type, const uint8_t * bytes, unsigned n)
{
	volatile uint8_t * buffer = (volatile uint8_t *) gDsiDma;
	bool shortPacket = type == 0x05 || type == 0x15;
	unsigned length = shortPacket ? 4 : (4 + n + 3) & ~3u, waited;

	if (n > 255 || (shortPacket && n > 2)) {
		return false;
	}
	if (shortPacket) {
		buffer[0] = n > 0 ? bytes[0] : 0;       /* MSM order: the two data bytes, then the data type; last packet */
		buffer[1] = n > 1 ? bytes[1] : 0;
		buffer[2] = type;
		buffer[3] = 0x80;
	} else {
		buffer[0] = (uint8_t) n;                /* MSM order: word count, then the data type; last packet, long */
		buffer[1] = 0;
		buffer[2] = type;
		buffer[3] = 0xc0;
		for (unsigned i = 0; i < length - 4; i++) {
			buffer[4 + i] = i < n ? bytes[i] : 0xff;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(dsi + DSI_INTR_CTRL) = displayReg(dsi, DSI_INTR_CTRL);      /* pending status written back: cleared */
	*(volatile uint32_t *)(dsi + DSI_DMA_BASE) = (uint32_t) DSI_DMA_PHYS;
	*(volatile uint32_t *)(dsi + DSI_DMA_LEN) = length;
	*(volatile uint32_t *)(dsi + DSI_TRIG_DMA) = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (waited = 0; waited < 2000 && !(displayReg(dsi, DSI_INTR_CTRL) & DSI_CMD_DMA_DONE); waited++) {
		IODelay(10);
	}
	*(volatile uint32_t *)(dsi + DSI_INTR_CTRL) = displayReg(dsi, DSI_INTR_CTRL);
	return waited < 2000;
}

/*
 * No transfer in flight (pushStart()): the panel to `hz` (60 or 90) if it is not there already, INTF1's TE
 * counter to match (the firmware's setting is for 60).
 */
static void
panelRate(unsigned hz)
{
	if (!dsiMap()) {
		return;
	}
	vm_offset_t dsi = gDSI;
	uint32_t ctrl = displayReg(dsi, DSI_CTRL), trig = displayReg(dsi, DSI_TRIG_CTRL), dma = displayReg(dsi, DSI_CMD_DMA_CTRL);
	unsigned before = teRate(), sent = 0;
	uint32_t vsync = displayReg(gDPU, INTF1_TE_VSYNC), count = vsync & 0x7ffff;

	if (hz == 90) {
		gTeVsync = vsync;
		displayWrite(INTF1_TE_VSYNC, (vsync & ~0x7ffffu) | (count * 60 / 90));
	}
	if (before + 10 > hz && before < hz + 10) {
		return;                 /* (already: the panel keeps its rate from the run before) */
	}
	if (!(ctrl & 1) || !(ctrl & (1u << 2)) || (trig & 7) != 4) {
		displayLog("display: DSI0 not in command mode with software-triggered DMA (ctrl %#x trig %#x); the panel stays at %u Hz\n",
		    ctrl, trig, before);
		return;
	}
	*(volatile uint32_t *)(dsi + DSI_CMD_DMA_CTRL) = dma & ~DSI_CMD_DMA_LOW_POWER;     /* "dsi_hs_mode" */
	for (const uint8_t * c = hz == 90 ? k90Hz : k60Hz; *c != 0 && dsiPacket(dsi, 0x39, c + 1, *c); c += 1 + *c) {
		sent++;
	}
	*(volatile uint32_t *)(dsi + DSI_CMD_DMA_CTRL) = dma;
	displayLog("display: %u Hz: %u of 9 DSI commands sent (ctrl %#x trig %#x dma %#x); TE %u Hz before, %u Hz after\n",
	    hz, sent, ctrl, trig, dma, before, teRate());
}

extern "C" int get_preemption_level(void);

static void
panelWait(unsigned ms)
{
	if (ml_get_interrupts_enabled() && get_preemption_level() == 0) {
		IOSleep(ms);
	} else {
		IODelay(ms * 1000);
	}
}

/* DSI0 in command mode with software-triggered DMA, as the firmware leaves it (panelRate()) */
static bool
dsiReady(void)
{
	if (!dsiMap()) {
		return false;
	}
	uint32_t ctrl = displayReg(gDSI, DSI_CTRL), trig = displayReg(gDSI, DSI_TRIG_CTRL);

	return (ctrl & 1) && (ctrl & (1u << 2)) && (trig & 7) == 4;
}

/* A table of packets (type, wait in ms, length, the bytes; 0 ends it), in low power mode as the stock sequences say. */
static unsigned
dsiSequence(const uint8_t * c)
{
	uint32_t dma = displayReg(gDSI, DSI_CMD_DMA_CTRL);
	unsigned sent = 0;

	*(volatile uint32_t *)(gDSI + DSI_CMD_DMA_CTRL) = dma | DSI_CMD_DMA_LOW_POWER;     /* "dsi_lp_mode" */
	for (; *c != 0 && dsiPacket(gDSI, c[0], c + 3, c[2]); c += 3 + c[2]) {
		sent++;
		if (c[1] != 0) {
			panelWait(c[1]);
		}
	}
	*(volatile uint32_t *)(gDSI + DSI_CMD_DMA_CTRL) = dma;
	return sent;
}


/* Autorefresh off, the transfer it may be in the middle of finished (Linux dpu_hw_intf_disable_autorefresh). */
static bool
pushStart(unsigned height)
{
	if (!gPush || !instantnoodle() || !mapDisplay()) {
		return false;
	}
	uint32_t top = displayReg(gDPU, CTL0_TOP), intf = displayReg(gDPU, CTL0_INTF_ACTIVE);
	uint32_t vig0 = displayReg(gDPU, VIG0_SRC0_ADDR), vig1 = displayReg(gDPU, VIG1_SRC0_ADDR), te = displayReg(gDPU, INTF1_TE_EN);

	gAutorefresh = displayReg(gDPU, INTF1_AUTOREFRESH);
	if (!(top & CTL_TOP_CMD_MODE) || intf != 0x2 || vig0 != vinfo.v_physaddr || vig1 != vinfo.v_physaddr || !(te & 1)) {
		displayLog("display: not the command-mode pipeline expected (CTL0 top %#x intf %#x, VIG %#x %#x, TE %#x); "
		    "frames go on being copied under autorefresh\n", top, intf, vig0, vig1, te);
		return false;
	}
	uint32_t vsync = displayReg(gDPU, INTF1_TE_VSYNC);
	unsigned waited;

	displayWrite(INTF1_TE_VSYNC, vsync & ~INTF1_TE_EXTERNAL);
	displayWrite(INTF1_AUTOREFRESH, gAutorefresh & ~(1u << 31));
	__asm__ volatile ("dsb sy" ::: "memory");
	for (waited = 0; waited < PUSH_TIMEOUT_MS * 10; waited++) {
		uint32_t line = displayReg(gDPU, INTF1_TE_LINE) & 0xffff;

		if (line == 0 || line >= height) {
			break;
		}
		IODelay(100);
	}
	displayWrite(INTF1_TE_VSYNC, vsync);
	if (waited == PUSH_TIMEOUT_MS * 10) {
		displayLog("display: the autorefresh frame did not finish; autorefresh back on\n");
		displayWrite(INTF1_AUTOREFRESH, gAutorefresh);
		return false;
	}
	displayWrite(MDP_INTR_CLEAR, MDP_INTR_PP0_DONE);
	if (gPushIrq) {
		displayWrite(MDP_INTR_EN, displayReg(gDPU, MDP_INTR_EN) | MDP_INTR_PP0_DONE);
		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &gTePeriod);
		displayWrite(INTF1_TEAR_INTR_CLEAR, TEAR_RD_PTR);
		displayWrite(INTF1_TEAR_INTR_EN, displayReg(gDPU, INTF1_TEAR_INTR_EN) | TEAR_RD_PTR);
		gPushIrqLive = gic_v3_spi_attach(MDSS_INTID, false, pushInterrupt, NULL) == KERN_SUCCESS;
		if (!gPushIrqLive) {
			displayLog("display: MDSS interrupt %u taken; the done bit is polled\n", MDSS_INTID);
		}
	}
	gPushing = true;
	if (refreshHz() == 90 || refreshHz() == 60) {
		panelRate(refreshHz());
	}
	displayLog("display: autorefresh %#x off after %u us; each frame is pushed through CTL0 on the panel's TE (%s)\n",
	    gAutorefresh, waited * 100, gPushIrqLive ? "interrupt" : "polled");
	return true;
}

/*
 * Pushes in a row that did not finish (pushWait() false). One is a refresh missed; PUSH_FAILURES in a row, and the
 * panel or the MDP has stopped: the boot framebuffer and autorefresh back for the rest of the run (pushStop()).
 */
#define PUSH_FAILURES           3
static uint32_t gPushFailures;

static bool
pushGiveUp(void)
{
	return ++gPushFailures >= PUSH_FAILURES;
}

/* Wait until no push is in flight: the framebuffer is the MDP's while one is. false: it never finished. */
static bool
pushWait(void)
{
	uint64_t pushed = gPushed, deadline;

	if (pushed == 0) {
		return true;
	}
	clock_interval_to_deadline(PUSH_TIMEOUT_MS, kMillisecondScale, &deadline);
	for (;;) {
		uint64_t step;

		clock_interval_to_deadline(2, kMillisecondScale, &step);
		assert_wait_deadline((event_t) &gPushDone, THREAD_UNINT, step < deadline ? step : deadline);
		if (!__atomic_load_n(&gPushIrqLive, __ATOMIC_ACQUIRE) && __atomic_load_n(&gPushDone, __ATOMIC_ACQUIRE) == 0 &&
		    (displayReg(gDPU, MDP_INTR_STATUS) & MDP_INTR_PP0_DONE)) {
			/* (no interrupt: polled; clearing the bit under a live interrupt would take its done away) */
			displayWrite(MDP_INTR_CLEAR, MDP_INTR_PP0_DONE);
			__atomic_store_n(&gPushDone, mach_absolute_time(), __ATOMIC_RELEASE);
		}
		if (__atomic_load_n(&gPushDone, __ATOMIC_ACQUIRE) != 0 || mach_absolute_time() >= deadline) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
	uint64_t done = __atomic_load_n(&gPushDone, __ATOMIC_ACQUIRE);

	if (done == 0 && (displayReg(gDPU, MDP_INTR_STATUS) & MDP_INTR_PP0_DONE)) {
		/* done, and its interrupt did not come: taken as done, and said (it should not happen) */
		static uint32_t told;

		displayWrite(MDP_INTR_CLEAR, MDP_INTR_PP0_DONE);
		done = mach_absolute_time();
		if (told++ < 4) {
			displayLog("display: a push was done but its interrupt (%u) did not come\n", MDSS_INTID);
		}
	}
	gPushed = 0;
	if (done == 0) {
		return false;
	}
	gPushFailures = 0;
	if (gTouchLat) {
		hk_tl("tl: k %u kick %llu done %llu\n", gPushId, micros(pushed), micros(done));
	}
	return true;
}

/*
 * The panel asleep (IOMobileFramebufferRequestPowerChange 0, panelPower()): what the presenter pushes meanwhile is
 * only remembered -- the pipes pointed at it, the push counted as done at once --, and the latest is sent when the
 * panel wakes. gPanelLock: a push and the panel's DCS commands never overlap on the DSI link.
 */
static bool gPanelOff;
static IOLock * gPanelLock;
static IOLock * gClientsLock;           /* (the framebuffer's clients, IOMobileFramebufferUserClient::panelAwake()) */

static void
pushFrameLocked(uint32_t swap, uint64_t buffer)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	if (gDouble) {
		displayWrite(VIG0_SRC0_ADDR, (uint32_t) buffer);
		displayWrite(VIG1_SRC0_ADDR, (uint32_t) buffer);
	}
	loader_screen[1] = gStride[0] != 0 ? gPanelRowBytes : vinfo.v_rowbytes;
	loader_screen[0] = buffer;
	gPushId = swap;
	if (gPanelOff) {
		gPushed = 0;
		__atomic_store_n(&gPushDone, mach_absolute_time(), __ATOMIC_RELEASE);
		return;
	}
	__atomic_store_n(&gPushDone, 0, __ATOMIC_RELEASE);
	gPushed = mach_absolute_time();
	displayWrite(MDP_INTR_CLEAR, MDP_INTR_PP0_DONE);
	displayWrite(CTL0_FLUSH, CTL_FLUSH_FRAME);
	displayWrite(CTL0_START, 1);
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* The frame in the framebuffer to the panel. Every processor that wrote it has drained its stores (drawRows()). */
static void
pushFrame(uint32_t swap, uint64_t buffer)
{
	IOLockLock(gPanelLock);
	if (!gPanelOff && gPushed != 0) {
		pushWait();             /* (the frame the panel's wake sent, panelPower(), may still be on its way) */
	}
	pushFrameLocked(swap, buffer);
	IOLockUnlock(gPanelLock);
}

/* (bsd/dev/hk_power.c: the screen's state, for the memory's bandwidth, the keys and the system's sleep) */
extern "C" void hk_power_display(bool on);


/*
 * The brightness: DCS write_display_brightness (0x51) with 11 bits, 1..0x7ff, high byte first -- the ANA6705's device
 * tree (qcom,mdss-dsi-bl-max-level 0x7ff, bl_ctrl_dcs; its own hbm commands are "51 0c 7c" and "51 0e ec" in that order),
 * in high-speed mode as the panel's brightness commands are. What iOS asks for comes from MiniAppleARMBacklight.cpp
 * as 0..65536 (its "backlight-level"); a slider position is perceptual, the panel's value is linear in light, so the
 * level is squared. gBacklight 0: iOS has not asked yet, the panel stays as the firmware left it (default 0x168).
 * gPanelLock held; sent again when the panel is lit after a dark.
 */
#define BACKLIGHT_FIRMWARE      0x168   /* qcom,mdss-brightness-default-val: what the panel has until iOS asks */
static uint32_t gBacklight;
static uint64_t gBacklightSent;                 /* how many times the panel took one (kern.hackios.backlight_sent) */

static bool
backlightSendLocked(void)
{
	uint8_t bytes[3] = { 0x51, (uint8_t)(gBacklight >> 8), (uint8_t)(gBacklight & 0xff) };
	uint32_t dma;
	bool ok;

	if (gBacklight == 0 || !gPushing || !instantnoodle() || !dsiReady()) {
		return false;
	}
	if (!gPanelOff && gPushed != 0) {
		pushWait();                     /* (the frame on its way finishes first) */
	}
	dma = displayReg(gDSI, DSI_CMD_DMA_CTRL);
	*(volatile uint32_t *)(gDSI + DSI_CMD_DMA_CTRL) = dma & ~DSI_CMD_DMA_LOW_POWER;     /* "dsi_hs_mode" */
	ok = dsiPacket(gDSI, 0x39, bytes, sizeof(bytes));
	*(volatile uint32_t *)(gDSI + DSI_CMD_DMA_CTRL) = dma;
	gBacklightSent += ok;
	return ok;
}

/*
 * The panel dark and lit again, for IOMobileFramebufferRequestPowerChange (backboardd: 0 when the screen locks or dims
 * out, 1 when it wakes): display off (0x28) and display on (0x29), DCS short writes, nothing else. The OLED's pixels
 * are off meanwhile, its TE goes on but nothing is pushed and no VSync is told; the display engine and the memory
 * idle. Not sleep in (0x10), as Android's off-command has it: after it the panel lost its DSC set-up, and neither
 * sleep out with display on nor Android's whole on-command (whose picture parameters are not those the firmware set
 * the display engine up with) brought the picture back -- coloured static over the screen (2026-10-25, on the
 * phone). Android resets and repowers the panel for that; here the firmware's set-up is what has to stay. Thread
 * context; the presenter waits meanwhile.
 */
static void
panelPower(bool on)
{
	static const uint8_t kOff[] = { 0x05, 20, 1, 0x28, 0 };
	uint64_t began = mach_absolute_time();

	if (gPanelLock == NULL || !instantnoodle()) {
		return;
	}
	IOLockLock(gPanelLock);
	if (!gPushing || gPanelOff == !on || !dsiReady()) {
		IOLockUnlock(gPanelLock);
		return;
	}
	if (!on) {
		uint32_t dma = displayReg(gDSI, DSI_CMD_DMA_CTRL);
		unsigned sent = 0;

		pushWait();                     /* (the frame on its way finishes first) */
		*(volatile uint32_t *)(gDSI + DSI_CMD_DMA_CTRL) = dma & ~DSI_CMD_DMA_LOW_POWER;   /* "dsi_hs_mode" */
		for (const uint8_t * c = kOff; *c != 0 && dsiPacket(gDSI, c[0], c + 3, c[2]); c += 3 + c[2]) {
			sent++;
			panelWait(c[1]);
		}
		*(volatile uint32_t *)(gDSI + DSI_CMD_DMA_CTRL) = dma;
		displayWrite(INTF1_TEAR_INTR_EN, displayReg(gDPU, INTF1_TEAR_INTR_EN) & ~TEAR_RD_PTR);
		__atomic_store_n(&gTeTime, 0, __ATOMIC_RELEASE);
		gPanelOff = true;
		IOLockUnlock(gPanelLock);
		displayLog("display: the panel dark (%u command, %llu ms)\n", sent, micros(mach_absolute_time() - began) / 1000);
		hk_power_display(false);
		return;
	}
	hk_power_display(true);         /* (the memory's bandwidth up before the first frame) */
	static const uint8_t kOn[] = { 0x05, 0, 1, 0x29, 0 };
	unsigned sent = dsiSequence(kOn);

	backlightSendLocked();          /* (the brightness iOS had set, before the first lit frame) */
	displayWrite(INTF1_TEAR_INTR_CLEAR, TEAR_RD_PTR);
	if (gPushIrq) {
		displayWrite(INTF1_TEAR_INTR_EN, displayReg(gDPU, INTF1_TEAR_INTR_EN) | TEAR_RD_PTR);
	}
	gPanelOff = false;
	pushFrameLocked(gPushId, loader_screen[0] != 0 ? loader_screen[0] : vinfo.v_physaddr);    /* the latest frame */
	IOLockUnlock(gPanelLock);
	displayLog("display: the panel lit (%u command, %llu ms)\n", sent, micros(mach_absolute_time() - began) / 1000);
}

/* The panel to sleep and back for the system's sleep (bsd/dev/hk_power.c), when iOS had left it lit: thread context */
extern "C" void hk_display_power(bool on);

/* iOS's brightness (MiniAppleARMBacklight.cpp: "backlight-level", 0..65536) to the panel. Thread context. */
extern "C" void
hk_display_set_backlight(uint32_t level)
{
	uint64_t squared = (uint64_t) MIN(level, 65536u) * MIN(level, 65536u) >> 16;
	uint32_t dcs = 1 + (uint32_t)(squared * 2046 >> 16);
	bool sent = false;

	if (gPanelLock != NULL) {
		IOLockLock(gPanelLock);
		gBacklight = dcs;
		sent = backlightSendLocked();
		IOLockUnlock(gPanelLock);
	}
	displayLog("display: brightness level %u of 65536: panel value %#x (%s)\n", level, dcs,
	    sent ? "sent" : gPanelOff ? "kept for when the panel is lit" : "not sent: no DSI command path");
}

/* What the panel has been told (kern.hackios.backlight: its 11-bit value, 0 not yet; backlight_sent: how many writes) */
extern "C" uint32_t
hk_display_backlight(uint64_t * sent)
{
	*sent = gBacklightSent;
	return gBacklight;
}

/* The window server's swaps so far, as the last push carries them (kern.hackios.frames, bsd/dev/hk_power.c: tests wait for a rate) */
extern "C" uint32_t
hk_display_frames(void)
{
	return gPushId;
}

/* Whether the panel is lit (bsd/dev/hk_power.c): asleep only after a power change to 0 */
extern "C" bool
hk_display_on(void)
{
	return !gPanelOff;
}

/* Back to the loader (loader_return.c), from any context: the panel refreshing from the boot framebuffer again. */
extern "C" void
hk_display_quiesce(void)
{
	if (!gPushing) {
		return;
	}
	if (gPanelOff && dsiReady()) {
		/* dark: lit for the loader's text, without the lock (a panic may hold it) */
		static const uint8_t kOn[] = { 0x05, 0, 1, 0x29, 0 };

		dsiSequence(kOn);
		gPanelOff = false;
	}
	for (unsigned i = 0; i < PUSH_TIMEOUT_MS * 10 && gPushed != 0 &&
	    !(displayReg(gDPU, MDP_INTR_STATUS) & MDP_INTR_PP0_DONE) && __atomic_load_n(&gPushDone, __ATOMIC_ACQUIRE) == 0; i++) {
		IODelay(100);
	}
	if (gBacklight != 0 && gBacklight != BACKLIGHT_FIRMWARE) {
		/* the loader's menu is read at the brightness the firmware chose, not at one iOS left (a shutdown sets 0) */
		gBacklight = BACKLIGHT_FIRMWARE;
		backlightSendLocked();
	}
	pushStop();
}

static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}

static void
traceScanout(uint32_t swap, const char *stage)
{
	if (gDPU != 0 && gTouchLat) {
		hk_tl("tl: h %u %s at %llu intf1 frame %u line %u out %#x count %#x\n", swap, stage,
		    micros(mach_absolute_time()), displayReg(gDPU, 0x6b8ac), displayReg(gDPU, 0x6b8b0),
		    displayReg(gDPU, 0x6baac), displayReg(gDPU, 0x6bab0));
	}
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	uint64_t        fSwapsQueued, fSwapsCopied;   /* (fSwapLock) swaps handed over; and done with their surfaces */
	uint32_t        fLastSurface;           /* (fSwapLock) layer 0's surface in the last swap ... */
	bool            fAlternating;           /* ... another than in the swap before, and no other layer */
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fDrawNs;                /* (fb_scan) what putting the last frame into the panel's memory took */
	uint32_t        fStatScanWaits, fStatScanMissed;        /* frames that waited for their place in the scan; that did not get it */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */
	uint32_t        fStatRaced;             /* frames whose surface the GPU was given work on while being copied */
	uint8_t *       fBuffer[2];             /* fb_push=2: the two framebuffers, and which is written next */
	uint64_t        fBufferPhys[2];
	IOMemoryMap *   fBufferMap;             /* ... the second one's mapping, kept */
	unsigned        fBack;
	bool            fPushTried;             /* pushStart() asked, at the first frame (the console draws until then) */
	/*
	 * fb_gpu: the two framebuffers as surfaces (MiniIOSurfaceCreateScanout) for the window server's GPU to
	 * scale a frame into. It asks for one (panelAcquire(): one that is neither on its way to the panel nor
	 * waiting to be), draws its frame of surface S into it and says so (panelReady()); the swap of S that
	 * follows pushes that buffer if S has had no GPU work since (panelFor()). Any other swap is copied here,
	 * as before, into a buffer the window server does not hold.
	 */
	MiniIOSurface * fPanel[2];
	uint8_t         fPanelState[2];         /* (fSwapLock) kPanelFree, kPanelDrawing (the window server's), kPanelReady */
	uint32_t        fPanelSurface[2];       /* (fSwapLock) kPanelReady: the frame of this surface, */
	uint32_t        fPanelDrawn[2];         /* ... after this submission of the GPU's (the surface's `drawn`), */
	uint32_t        fPanelAge[2];           /* ... said at this many swaps shown */
	int             fPanelSent;             /* (fSwapLock) the one pushed last; -1: none yet */
	task_t          fPanelTask;             /* the one task that draws into them (not held: only compared) */
	uint32_t        fShownSurfaces[4];      /* (fSwapLock) layer 0's last different surfaces: what a frame worth scaling is in */
	uint32_t        fStatPanel;             /* frames pushed as the GPU drew them, since the last five-second line */
	IOReturn panelAcquire(task_t task, uint64_t * out);
	IOReturn panelReady(task_t task, uint32_t index, uint32_t sid);
	int panelFor(const uint8_t * swap, MiniIOSurface * surface);
	int panelSpare(void);
	void panelSent(int index);

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	void dequeueSwap(void);
	void waitCopied(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	void drawShare(const struct RowJob * job, uint32_t * blendRow);
	int32_t         fNextRow;               /* (drawShare()) the first row nobody has taken yet */
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	/*
	 * The window server's frame period for the built-in panel comes from here, not from the timings
	 * (QuartzCore windowserver-apple-display.cpp; without it, 1/60 s whatever SyncRate says): the
	 * shortest interval between refreshes, in 2^-32 s (given the mach time of 1/90 s, 213333, it took
	 * 49.7 us), and the step between possible ones, in mach time. One fixed rate: both the period.
	 */
	if (refreshHz() != kFBRefreshHz) {
		OSDictionary * refresh = OSDictionary::withCapacity(2);
		uint64_t period;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		if (refresh != NULL) {
			OSNumber * interval = OSNumber::withNumber((1ULL << 32) / refreshHz(), 64);
			OSNumber * step = OSNumber::withNumber(period, 64);

			if (interval != NULL && step != NULL) {
				refresh->setObject("displayMinRefreshInterval", interval);
				refresh->setObject("displayRefreshStepMachTime", step);
			}
			OSSafeReleaseNULL(interval);
			OSSafeReleaseNULL(step);
			setProperty("IOMFBDisplayRefresh", refresh);
			refresh->release();
		}
	}
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	gPanelLock = IOLockAlloc();
	gClientsLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	probeDisplay();
	uint32_t hz;
	if (instantnoodle() && gPush != 0 && !PE_parse_boot_argn("fb_hz", &hz, sizeof(hz))) {
		gPanelHz = 90;                  /* (fb_hz=60: the firmware's rate) */
	}
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HKWaitsForGPU", kOSBooleanTrue);
	/* the panel's scan, if it can be read: its rate is then the one published, unless fb_hz= is given */
	if (hk_display_scan_start()) {
		uint32_t hz;
		const struct hk_display_scan * scan = hk_display_scan();

		if (!PE_parse_boot_argn("fb_hz", &hz, sizeof(hz)) && scan != NULL) {
			gScanHz = (scan->millihz + 500) / 1000;
		}
		IOLog("MiniIOMobileFramebuffer: frames follow the panel's scan, at %u a second\n", refreshHz());
	}
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
	/* the framebuffer is write-combined: this processor's rows reach memory before the frame is pushed */
	__asm__ volatile ("dsb st" ::: "memory");
}

/*
 * Rows of a job in turns of ROW_SHARE, until there are none: a processor that is slower (a small core) or
 * taken by another thread does fewer, instead of the whole copy waiting for its fixed third.
 */
#define ROW_SHARE               48

void
IOMobileFramebuffer::drawShare(const RowJob * job, uint32_t * blendRow)
{
	for (;;) {
		int32_t y = __atomic_fetch_add(&fNextRow, ROW_SHARE, __ATOMIC_RELAXED);

		if (y >= job->py1) {
			return;
		}
		drawRows(job, y, y + ROW_SHARE < job->py1 ? y + ROW_SHARE : job->py1, blendRow);
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		blend = fb->fHelperBlend[index];
		fb->drawShare(job, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: this thread and the helpers take rows until there are none, then it waits for them */
	IOLockLock(fHelperLock);
	fNextRow = py0;
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawShare(&job, fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hk_tl("tl: s %u at %llu surface %u\n", swapWord(swap, SWAP_ID), micros(now), swapWord(swap, SWAP_SURFACE(0)));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
		if (fSwapSurfaces[slot][layer] != NULL) {
			MiniIOSurfaceDisplayUse(fSwapSurfaces[slot][layer], 1);
		}
	}
	fSwapsWaiting++;
	fSwapsQueued++;
	fAlternating = swapWord(swap, SWAP_SURFACE(0)) != fLastSurface && swapWord(swap, SWAP_SURFACE(1)) == 0 &&
	    swapWord(swap, SWAP_SURFACE(2)) == 0 && swapWord(swap, SWAP_SURFACE(3)) == 0;
	fLastSurface = swapWord(swap, SWAP_SURFACE(0));
	if (fLastSurface != 0 && fLastSurface != fShownSurfaces[0]) {
		memmove(&fShownSurfaces[1], &fShownSurfaces[0], sizeof(fShownSurfaces) - sizeof(fShownSurfaces[0]));
		fShownSurfaces[0] = fLastSurface;
	}
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDisplayUse(surfaces[layer], -1);   /* (copied out, or discarded) */
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		IOLockLock(fSwapLock);
		fSwapsCopied++;
		IOLockWakeup(fSwapLock, &fSwapsCopied, false);
		IOLockUnlock(fSwapLock);
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/*
 * IOMobileFramebufferSwapWait. The window server draws the next frame as soon as this returns, into the
 * surface it handed over least recently: until the presenter has copied that one, the GPU would be writing
 * the very pixels being copied (bands of two frames, half drawn tiles). It alternates two surfaces on the
 * phone (3 and 4), so the last swap may still be copied meanwhile -- which lets drawing and copying overlap;
 * with one surface, or more layers than one, every swap so far. SWAP_GPU_MS at most: a GPU that stopped
 * answering must not stop the window server.
 */
void
IOMobileFramebuffer::waitCopied(void)
{
	AbsoluteTime deadline;

	clock_interval_to_deadline(SWAP_GPU_MS, kMillisecondScale, &deadline);
	IOLockLock(fSwapLock);
	for (uint64_t target = fSwapsQueued - (fAlternating ? 1 : 0); fSwapsCopied < target;) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsCopied, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
			break;
		}
	}
	IOLockUnlock(fSwapLock);
}

enum { kPanelFree, kPanelDrawing, kPanelReady };
#define PANEL_WAIT_MS           20      /* for a push to finish: a TE and a transfer at 60 Hz */
#define PANEL_STALE_SWAPS       3       /* a frame said to be ready that no swap has taken in so many is given up */

/* With fSwapLock held: is framebuffer `index` on its way to the panel right now? */
static bool
panelInFlight(int index, int sent)
{
	return index == sent && __atomic_load_n(&gPushed, __ATOMIC_ACQUIRE) != 0 &&
	       __atomic_load_n(&gPushDone, __ATOMIC_ACQUIRE) == 0 &&
	       (__atomic_load_n(&gPushIrqLive, __ATOMIC_ACQUIRE) || !(displayReg(gDPU, MDP_INTR_STATUS) & MDP_INTR_PP0_DONE));
}

/*
 * The window server's GPU is about to scale a frame: a framebuffer for it, neither waiting to be pushed nor
 * being pushed (a command-mode panel has the frame in its own memory once the push is done, so that is
 * soon). One it held and did not say was ready is taken back first. out: index, surface, width, height,
 * bytes a row.
 */
IOReturn
IOMobileFramebuffer::panelAcquire(task_t task, uint64_t * out)
{
	AbsoluteTime deadline, slice;
	int index = -1;

	if (fPanel[0] == NULL || !gDouble) {
		return kIOReturnNotReady;
	}
	clock_interval_to_deadline(PANEL_WAIT_MS, kMillisecondScale, &deadline);
	IOLockLock(fSwapLock);
	if (fPanelTask == NULL) {
		fPanelTask = task;              /* (the first to ask: the window server, whose GPU alone may draw into them) */
		fPanel[0]->scanoutOwner = fPanel[1]->scanoutOwner = task;
	}
	if (task != fPanelTask) {
		IOLockUnlock(fSwapLock);
		return kIOReturnNotPermitted;
	}
	for (int i = 0; i < 2; i++) {
		if (fPanelState[i] == kPanelDrawing) {
			fPanelState[i] = kPanelFree;
		}
	}
	for (;;) {
		bool pushing = false;

		/* (the one not pushed last first: the other may still be on its way) */
		for (int n = 0; n < 2 && index < 0; n++) {
			int i = fPanelSent == 0 ? 1 - n : n;

			if (fPanelState[i] == kPanelFree && !panelInFlight(i, fPanelSent)) {
				index = i;
			}
			pushing |= fPanelState[i] == kPanelFree;
		}
		/* (only a push is waited for: a frame waiting for its swap is the presenter's to take, later) */
		if (index >= 0 || !pushing || mach_absolute_time() >= deadline) {
			break;
		}
		/* (the end of a push is an interrupt that wakes nobody here: looked at every millisecond) */
		clock_interval_to_deadline(1, kMillisecondScale, &slice);
		IOLockSleepDeadline(fSwapLock, fPanelState, slice < deadline ? slice : deadline, THREAD_UNINT);
	}
	if (index >= 0) {
		fPanelState[index] = kPanelDrawing;
		fPanelAge[index] = fSwapsShown;
	}
	IOLockUnlock(fSwapLock);
	if (index < 0) {
		return kIOReturnBusy;
	}
	out[0] = (uint64_t) index;
	out[1] = fPanel[index]->sid;
	out[2] = fPanelWidth;
	out[3] = fPanelHeight;
	out[4] = fRowBytes;
	return kIOReturnSuccess;
}

/* The window server's GPU has been given the frame of surface `sid`, scaled into framebuffer `index`. */
IOReturn
IOMobileFramebuffer::panelReady(task_t task, uint32_t index, uint32_t sid)
{
	MiniIOSurface * surface = MiniIOSurfaceCopy(sid);
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(fSwapLock);
	if (task != fPanelTask) {
		ret = kIOReturnNotPermitted;
	} else if (index < 2 && fPanelState[index] == kPanelDrawing && surface != NULL &&
	    sid != fShownSurfaces[0] && sid != fShownSurfaces[1] && sid != fShownSurfaces[2] && sid != fShownSurfaces[3]) {
		/* (a picture of the screen's size that is not the screen's -- a snapshot: no swap will come for it) */
		fPanelState[index] = kPanelFree;
		ret = kIOReturnNotFound;
	} else if (index < 2 && fPanelState[index] == kPanelDrawing && surface != NULL) {
		for (int i = 0; i < 2; i++) {
			if (fPanelState[i] == kPanelReady && fPanelSurface[i] == sid) {
				fPanelState[i] = kPanelFree;            /* (an earlier frame of the same surface: never to be shown) */
			}
		}
		fPanelState[index] = kPanelReady;
		fPanelSurface[index] = sid;
		fPanelDrawn[index] = surface->drawn;
		fPanelAge[index] = fSwapsShown;
		ret = kIOReturnSuccess;
	}
	IOLockWakeup(fSwapLock, fPanelState, false);
	IOLockUnlock(fSwapLock);
	if (surface != NULL) {
		MiniIOSurfaceDrop(surface);
	}
	return ret;
}

/*
 * The presenter, for a swap: the framebuffer the GPU has scaled this very frame into, once the GPU is done
 * with it; -1: none (copied here). Only a swap of one full-screen layer qualifies, and only if its surface has
 * had no GPU work since the window server said the frame was ready. Ready ones no swap took are given up.
 */
int
IOMobileFramebuffer::panelFor(const uint8_t * swap, MiniIOSurface * surface)
{
	uint32_t sid = swapWord(swap, SWAP_SURFACE(0));
	int32_t src[4], dst[4];
	int index = -1;
	bool whole;

	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
	whole = surface != NULL && surface->info.width == fWidth && surface->info.height == fHeight &&
	    swapWord(swap, SWAP_SURFACE(1)) == 0 && swapWord(swap, SWAP_SURFACE(2)) == 0 && swapWord(swap, SWAP_SURFACE(3)) == 0 &&
	    src[0] == 0 && src[1] == 0 && (src[2] <= 0 || (uint32_t) src[2] == fWidth) && (src[3] <= 0 || (uint32_t) src[3] == fHeight) &&
	    dst[0] == 0 && dst[1] == 0 && (dst[2] <= 0 || (uint32_t) dst[2] == fWidth) && (dst[3] <= 0 || (uint32_t) dst[3] == fHeight);
	IOLockLock(fSwapLock);
	static uint32_t told;
	if (told < 8 && fPanelTask != NULL && !(whole && ((fPanelState[0] == kPanelReady && fPanelSurface[0] == sid &&
	    fPanelDrawn[0] == surface->drawn) || (fPanelState[1] == kPanelReady && fPanelSurface[1] == sid &&
	    fPanelDrawn[1] == surface->drawn)))) {
		told++;
		IOLog("MiniIOMobileFramebuffer: swap %u of surface %u (%s, GPU work %u) copied: framebuffers %u/%u/%u and %u/%u/%u "
		    "(state/surface/GPU work)\n", swapWord(swap, SWAP_ID), sid, whole ? "whole" : "not one whole layer",
		    surface != NULL ? surface->drawn : 0, fPanelState[0], fPanelSurface[0], fPanelDrawn[0], fPanelState[1],
		    fPanelSurface[1], fPanelDrawn[1]);
	}
	for (int i = 0; i < 2; i++) {
		if (fPanelState[i] == kPanelDrawing && fSwapsShown - fPanelAge[i] > PANEL_STALE_SWAPS) {
			fPanelState[i] = kPanelFree;            /* (taken and never said to be ready: the window server gave up on it) */
		}
		if (fPanelState[i] != kPanelReady) {
			continue;
		}
		if (whole && fPanelSurface[i] == sid && fPanelDrawn[i] == surface->drawn) {
			index = i;
		} else if (fPanelSurface[i] == sid || fSwapsShown - fPanelAge[i] > PANEL_STALE_SWAPS) {
			fPanelState[i] = kPanelFree;
		}
	}
	IOLockWakeup(fSwapLock, fPanelState, false);
	IOLockUnlock(fSwapLock);
	if (index >= 0 && !MiniIOSurfaceDrawn(fPanel[index], SWAP_GPU_MS)) {
		IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with framebuffer %d after %u ms; copied instead\n",
		    swapWord(swap, SWAP_ID), index, SWAP_GPU_MS);
		panelSent(-1);
		IOLockLock(fSwapLock);
		fPanelState[index] = kPanelFree;
		IOLockUnlock(fSwapLock);
		index = -1;
	}
	return index;
}

/*
 * The presenter, for a frame copied here: a framebuffer the window server does not hold (the one not pushed last
 * first). None free -- one waits for a swap that has not come, the other is being drawn: the waiting one is taken
 * back, its frame given up (the swap it was for is copied in its turn), once the GPU is done with it.
 */
int
IOMobileFramebuffer::panelSpare(void)
{
	int index = -1, taken = -1;

	IOLockLock(fSwapLock);
	for (int n = 0; n < 2 && index < 0; n++) {
		int i = fPanelSent == 0 ? 1 - n : n;

		if (fPanelState[i] == kPanelFree) {
			index = i;
		} else if (fPanelState[i] == kPanelReady && taken < 0) {
			taken = i;
		}
	}
	if (index < 0 && taken >= 0) {
		fPanelState[taken] = kPanelFree;
		index = taken;
	}
	IOLockUnlock(fSwapLock);
	if (taken == index && index >= 0 && !MiniIOSurfaceDrawn(fPanel[index], SWAP_GPU_MS)) {
		index = -1;
	}
	return index;
}

/* The presenter has pushed framebuffer `index` (-1: nothing new): free for the next frame once the push is done. */
void
IOMobileFramebuffer::panelSent(int index)
{
	IOLockLock(fSwapLock);
	if (index >= 0) {
		fPanelState[index] = kPanelFree;
		fPanelSent = index;
	}
	IOLockWakeup(fSwapLock, fPanelState, false);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::dequeueSwap(void)
{
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;
	int panel;                              /* fb_gpu: the framebuffer the GPU has drawn this frame into; -1: none */

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; discarded\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
			dequeueSwap();
			return;
		}
	}
	if (gTouchLat) {
		hk_tl("tl: d %u began %llu ready %llu\n", swapWord(swap, SWAP_ID), micros(began), micros(mach_absolute_time()));
	}
	/* the GPU work each surface was waited for: new work naming it during the copy is a race (see waitCopied()) */
	uint32_t fences[SWAP_LAYERS];
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		fences[layer] = surfaces[layer] != NULL ? surfaces[layer]->drawn : 0;
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (!fPushTried) {
		fPushTried = true;
		uint32_t rowBytes = (fPanelWidth * 4 + 63) & ~63u, stride0 = 0, stride1 = 0;
		uint64_t panelSize = round_page_64((uint64_t) rowBytes * fPanelHeight);

		if (pushStart(fPanelHeight) && gPush == 2 && gPanelGpu) {
			/* both inside the display's memory, apart, and the pipes reading the boot framebuffer's pitch now */
			stride0 = displayReg(gDPU, VIG0_SRC_YSTRIDE0);
			stride1 = displayReg(gDPU, VIG1_SRC_YSTRIDE0);
			if ((stride0 & 0xffff) == fRowBytes && (stride1 & 0xffff) == fRowBytes &&
			    vinfo.v_physaddr + panelSize <= DOUBLE_PHYS && DOUBLE_PHYS + panelSize <= SPLASH_END) {
				fPanel[0] = MiniIOSurfaceCreateScanout(vinfo.v_physaddr, fPanelWidth, fPanelHeight, rowBytes);
				fPanel[1] = MiniIOSurfaceCreateScanout(DOUBLE_PHYS, fPanelWidth, fPanelHeight, rowBytes);
			}
			if (fPanel[0] != NULL && fPanel[1] != NULL) {
				gStride[0] = stride0;
				gStride[1] = stride1;
				displayWrite(VIG0_SRC_YSTRIDE0, (stride0 & ~0xffffu) | rowBytes);    /* (latched by the first push) */
				displayWrite(VIG1_SRC_YSTRIDE0, (stride1 & ~0xffffu) | rowBytes);
				fRowBytes = rowBytes;
				gPanelBase = fPanel[0]->pixels();
				gPanelRowBytes = rowBytes;
				gPanelRows = fPanelHeight;
				for (unsigned i = 0; i < 2; i++) {
					fBuffer[i] = fPanel[i]->pixels();
				}
				fBufferPhys[0] = vinfo.v_physaddr;
				fBufferPhys[1] = DOUBLE_PHYS;
				fPanelSent = -1;
				fBack = 1;
				gDouble = true;
			} else {
				MiniIOSurfaceDrop(fPanel[0]);
				MiniIOSurfaceDrop(fPanel[1]);
				fPanel[0] = fPanel[1] = NULL;
			}
			displayLog("display: framebuffers %#llx and %#llx as surfaces %u and %u for the GPU (rows of %u bytes; the "
			    "pipes had %#x %#x)%s\n", (uint64_t) vinfo.v_physaddr, DOUBLE_PHYS, fPanel[0] ? fPanel[0]->sid : 0,
			    fPanel[1] ? fPanel[1]->sid : 0, rowBytes, stride0, stride1, gDouble ? "" : ": not taken, copied here");
		}
		if (gPushing && gPush == 2 && !gDouble) {
			size_t frame = round_page((size_t) fRowBytes * fPanelHeight);

			fBuffer[0] = fBase;
			fBufferPhys[0] = vinfo.v_physaddr;
			fBufferPhys[1] = DOUBLE_PHYS;
			/* (in the kernel map: ml_io_map's range has no room for 10 MB) */
			IOMemoryDescriptor * memory = vinfo.v_physaddr + frame <= DOUBLE_PHYS && DOUBLE_PHYS + frame <= 0x9e300000ull ?
			    IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) DOUBLE_PHYS, frame, kIODirectionInOut) : NULL;

			fBufferMap = memory != NULL ? memory->map(kIOMapWriteCombineCache) : NULL;
			OSSafeReleaseNULL(memory);
			fBuffer[1] = fBufferMap != NULL ? (uint8_t *) fBufferMap->getVirtualAddress() : NULL;
			gDouble = fBuffer[1] != NULL;
			fBack = 1;
			displayLog("display: second framebuffer at %#llx: %s\n", DOUBLE_PHYS, gDouble ? "frames alternate" : "not mapped");
		}
	}
	if (!gPushing && fPanel[0] != NULL && fRowBytes != vinfo.v_rowbytes) {
		fRowBytes = vinfo.v_rowbytes;   /* (given back, pushStop(): the boot framebuffer at the firmware's pitch) */
		fBuffer[0] = (uint8_t *) vinfo.v_baseaddr;
	}
	if (!gPushing && fBuffer[0] != NULL && fBase != fBuffer[0]) {
		fBase = fBuffer[0];             /* (given back: pushStop()) */
	}
	panel = gDouble && fPanel[0] != NULL ? panelFor(swap, surfaces[0]) : -1;
	if (gDouble && fPanel[0] != NULL && panel < 0) {
		/* copied here, then: into one the window server does not hold, out of the panel's way */
		int spare = panelSpare();

		if (spare < 0) {
			displayLog("display: swap %u: both framebuffers are the GPU's; not shown\n", swapWord(swap, SWAP_ID));
			fSwapsShown++;                  /* (what waits for a swap ages all the same) */
			dequeueSwap();
			return;
		}
		if (spare == fPanelSent && !pushWait() && pushGiveUp()) {
			displayLog("display: %u pushed frames in a row did not finish in %u ms; the boot framebuffer and autorefresh "
			    "back\n", PUSH_FAILURES, PUSH_TIMEOUT_MS);
			pushStop();
		}
		fBack = (unsigned) spare;
	}
	if (gDouble) {
		fBase = fBuffer[fBack];         /* the one not being sent: written now, waited for at the push */
	} else if (gPushing) {
		/* paced by the panel: the frame before has to be out of the framebuffer first */
		if (!pushWait() && pushGiveUp()) {
			displayLog("display: %u pushed frames in a row did not finish in %u ms; autorefresh back on\n", PUSH_FAILURES,
			    PUSH_TIMEOUT_MS);
			pushStop();
		}
		fStatBeatWait += mach_absolute_time() - began;
		fStatOnBeat++;
		began = mach_absolute_time();
	} else if (hk_display_scan() != NULL) {
		/*
		 * Where the copy may start (fb_scan): every band of rows (fThreads of them, drawn side by side, each top
		 * to bottom in about fDrawNs) has to stay behind the scan in this refresh -- written after the scan has
		 * passed it, so it is first shown in the next -- and to be done before the next refresh's scan gets to
		 * it. The last band starts below the scan and must not catch it up; the first must be done before the
		 * scan comes round to its last row. That is a window of lines for the scan to be in when the copy starts:
		 *   from  max(shown (n-1)/n, shown - D/line) and   up to  lines + shown/n - D/line.
		 * Waited for, a frame and a half at most: a frame is never held longer than that for it.
		 */
		const struct hk_display_scan * scan = hk_display_scan();
		uint64_t d = fDrawNs != 0 ? fDrawNs : scan->frame_ns / 3, dl = d / scan->line_ns;
		uint32_t n = fThreads != 0 ? fThreads : 1, S = scan->shown, T = scan->lines;
		uint64_t lo = (uint64_t) S * (n - 1) / n, hi = (uint64_t) T + S / n;

		lo = S > dl && S - dl > lo ? S - dl : lo;
		lo += 16;
		hi = hi > dl + 16 ? hi - dl - 16 : 0;
		if (hi > T) {
			hi = T;
		}
		if (lo >= hi) {
			lo = S;                         /* (a copy too slow for any window: at least start in the blanking) */
			hi = T;
		}
		fStatScanWaits++;
		if (!hk_display_wait_lines((uint32_t) lo, (uint32_t) hi, scan->frame_ns * 3 / 2)) {
			fStatScanMissed++;
		}
		fBeat = began = mach_absolute_time();
	} else if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	traceScanout(swapWord(swap, SWAP_ID), "copy-begin");
	/* its turn has come: the swap after it may be handed over */
	dequeueSwap();
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			if (panel < 0) {
				drawLayer(s, src, dst);
			}
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && surfaces[layer]->drawn != fences[layer] && fStatRaced++ < 20) {
			displayLog("display: swap %u: surface %u got GPU work %u while it was copied (waited for %u)\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), surfaces[layer]->drawn, fences[layer]);
		}
	}
	if (gDouble && !pushWait() && pushGiveUp()) {
		displayLog("display: %u pushed frames in a row did not finish in %u ms; the boot framebuffer and autorefresh back\n",
		    PUSH_FAILURES, PUSH_TIMEOUT_MS);
		pushStop();
	} else if (gPushing) {
		unsigned sent = panel >= 0 ? (unsigned) panel : fBack;

		pushFrame(swapWord(swap, SWAP_ID), fBufferPhys[sent]);
		if (fPanel[0] != NULL && gDouble) {
			panelSent((int) sent);
		} else {
			fBack ^= gDouble;
		}
	}
	drawn = mach_absolute_time();
	traceScanout(swapWord(swap, SWAP_ID), "copy-end");
	absolutetime_to_nanoseconds(drawn - began, &nanos);
	fDrawNs = fDrawNs == 0 ? nanos : (fDrawNs * 7 + nanos) / 8;    /* (an average: one slow frame does not move the window) */
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		hk_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatPanel += panel >= 0;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves; %u frames scaled by the GPU\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate, fStatPanel);
		fStatSwapsLate = 0;
		fStatPanel = 0;
		if (fStatScanWaits != 0) {
			IOLog("MiniIOMobileFramebuffer: the scan (fb_scan): %u frames started where it let them land whole, %u did not "
			    "get there in time; %llu us to put a frame into the panel's memory\n", fStatScanWaits - fStatScanMissed,
			    fStatScanMissed, fDrawNs / NSEC_PER_USEC);
			fStatScanWaits = fStatScanMissed = 0;
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fVSyncParked;           /* stopped because the panel sleeps: started again when it wakes */
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);
	void unparkVSync(void);

public:
	static void panelAwake(void);
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

/* The clients (backboardd's, and any other), for their refreshes to start again when the panel wakes */
#define kFBClientsMost          8
static IOMobileFramebufferUserClient * gClients[kFBClientsMost];

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	if (fLock == NULL || fVSyncCall == NULL) {
		return false;
	}
	IOLockLock(gClientsLock);
	for (unsigned i = 0; i < kFBClientsMost; i++) {
		if (gClients[i] == NULL) {
			gClients[i] = this;
			break;
		}
	}
	IOLockUnlock(gClientsLock);
	return true;
}

/* The panel is awake again: every client whose refreshes stopped while it slept gets them again. */
void
IOMobileFramebufferUserClient::panelAwake(void)
{
	if (gClientsLock == NULL) {
		return;                 /* (no framebuffer started: no clients) */
	}
	IOLockLock(gClientsLock);
	for (unsigned i = 0; i < kFBClientsMost; i++) {
		if (gClients[i] != NULL) {
			gClients[i]->unparkVSync();
		}
	}
	IOLockUnlock(gClientsLock);
}

void
IOMobileFramebufferUserClient::unparkVSync(void)
{
	IOLockLock(fLock);
	if (fVSyncParked && !fVSyncRunning && !fClosed && fCallbacks[kFBNotifyVSync] != 0) {
		fVSyncRunning = true;
		fVSyncTime = mach_absolute_time();
		armVSync();
	}
	fVSyncParked = false;
	IOLockUnlock(fLock);
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	IOLockLock(gClientsLock);
	for (unsigned i = 0; i < kFBClientsMost; i++) {
		if (gClients[i] == this) {
			gClients[i] = NULL;
		}
	}
	IOLockUnlock(gClientsLock);
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	uint64_t te = __atomic_load_n(&gTeTime, __ATOMIC_ACQUIRE), te_period = gTePeriod;

	/* the panel's next TE, where its interrupt says (fb_push) */
	if (te != 0 && te_period != 0 && now - te < 4 * te_period) {
		static bool said;

		for (fVSyncTime = te + te_period; fVSyncTime <= now + te_period / 8; fVSyncTime += te_period) {
		}
		if (!said) {
			said = true;
			displayLog("display: the frame clock follows the panel's TE, measured %llu us apart\n", micros(te_period));
		}
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}
	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	if (hk_display_scan() != NULL) {
		/* fb_scan: when the panel's next frame begins, by its scan -- locked to it, never drifting */
		uint64_t ahead;

		nanoseconds_to_absolutetime(hk_display_next_frame_ns(), &ahead);
		fVSyncTime = now + (ahead != 0 ? ahead : period);
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 *
 * The times are mach_continuous_time(), which goes on while the system sleeps: QuartzCore takes them in that
 * clock (CA_FORCE_TIMER, inject.c; the second, the presented time, is its frame clock). Given mach_absolute_time(),
 * which stands still in a sleep (bsd/dev/hk_power.c), the window server's frame clock fell behind its clients' by
 * all the time slept, and whatever came on the screen with an animation after a wake -- Control Center's modules,
 * an application starting, the status bar's text -- stayed invisible until the clock caught up (the owner's report
 * 2026-10-26; ca_vbl=0 or sleep_freeze=0 cured it).
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime + (mach_continuous_time() - mach_absolute_time());
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else if (gPanelOff) {
		/* the panel sleeps: it has no refreshes to tell of, as an iPhone's display has none when it is off */
		fVSyncRunning = false;
		fVSyncParked = true;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodRedwoodPanelAcquire:
	{
		uint64_t panel[5];
		IOReturn ret;

		if (args->scalarOutputCount < 5) {
			return kIOReturnBadArgument;
		}
		ret = fOwner->panelAcquire(fTask, panel);
		if (ret == kIOReturnSuccess) {
			memcpy(out, panel, sizeof(panel));
			args->scalarOutputCount = 5;
		}
		return ret;
	}
	case kFBMethodRedwoodPanelReady:
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		return fOwner->panelReady(fTask, (uint32_t) in[0], (uint32_t) in[1]);

	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* the swaps so far copied out of their surfaces */
	{
		uint64_t began = mach_absolute_time();

		fOwner->waitCopied();
		if (gTouchLat) {
			hk_tl("tl: w %llu %#llx %#llx at %llu waited %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(began),
			    micros(mach_absolute_time() - began));
		}
		return kIOReturnSuccess;
	}
	case kFBMethodRequestPowerChange:               /* in: 0 the display off, 1 on */
		if (args->scalarInputCount >= 1) {
			panelPower(in[0] != 0);
			if (in[0] != 0) {
				panelAwake();
			}
		}
		return kIOReturnSuccess;

	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
extern "C" void
hk_display_power(bool on)
{
	panelPower(on);
	if (on) {
		IOMobileFramebufferUserClient::panelAwake();
	}
}

static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		/*
		 * metal=2: the service carries the two properties from the start, as Apple's own
		 * accelerators do, so that every process -- the first daemons included -- finds the
		 * Metal device (userland/src/hkmetal.m) without bin/metal_props having run.
		 */
		if (metal == 2) {
			accelerator->setProperty(kAcceleratorPluginNameKey, "HKMetal");
			accelerator->setProperty(kAcceleratorPluginClassNameKey, "HKMetalDevice");
		}
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space%s\n", metal,
			    metal == 2 ? ", and carries HKMetal from the start" : "");
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
