/*
 * HKGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/hk_adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/hackios-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Every client has page tables of its own (boot-arg gpu_pt, the default on the OnePlus 8; see
 * hk_gpu_space_new() in bsd/dev/hk_adreno.c): the GPU reaches, while it runs a program's
 * commands, that program's buffers and nothing else -- an IOSurface is shared by being entered
 * into each program's tables that asks for it (surfaceBuffer()). A request the SMMU refuses is
 * counted against the program (MSM_PARAM_FAULTS) and logged. Without gpu_pt (the redwood) every
 * client's buffers are in the one address space.
 *
 * Not here yet: interrupts on the redwood (there, waiting is asking again five times a millisecond;
 * the OnePlus 8's fences raise one, gpu_irq), fences and buffers as file descriptors. A GPU that has stopped
 * answering is reported (hk_gpu_report()) and, with boot-arg gpu_recover=1 (the default on
 * the OnePlus 8), reset and started again, so that the submission that hung is all that is lost: what that does and
 * which source says so is at hk_gpu_recover() in bsd/dev/hk_adreno.c. Without the
 * boot-arg it stays stopped and nothing is waited for again.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void hk_tl(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/hk_adreno.c ("the GPU kept on"; declared in bsd/sys/hk_input.h, which is not seen from here) */
bool hk_gpu_ready(void);
bool hk_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hk_gpu_address(uint64_t size);
void hk_gpu_address_free(uint64_t address, uint64_t size);
void hk_gpu_space(uint64_t * used, uint32_t * live, uint64_t * largest, unsigned * holes, unsigned * most,
    uint64_t * lost, uint32_t * refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
struct gpu_space;                       /* a program's page tables (gpu_pt); NULL: the one space for everybody */
extern uint32_t gpu_pt;
struct gpu_space * hk_gpu_space_new(void);
uint32_t hk_gpu_space_leave(struct gpu_space * space);
bool hk_gpu_space_free(struct gpu_space * space);
uint32_t hk_gpu_space_faults(struct gpu_space * space);
void hk_gpu_faults(void);
bool hk_gpu_map(struct gpu_space * space, uint64_t address, uint64_t phys, bool read_only);
void hk_gpu_mapped(struct gpu_space * space);
void hk_gpu_unmap(struct gpu_space * space, uint64_t address, uint64_t size);
void hk_gpu_to_memory(void * start, uint64_t size);
uint32_t hk_gpu_submit(struct gpu_space * space, const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool hk_gpu_done(uint32_t fence);
bool hk_gpu_interrupts(void);
uint32_t hk_gpu_level_count(void);
uint32_t hk_gpu_level_khz(uint32_t level);
bool hk_gpu_set_level(uint32_t level);
uint64_t hk_gpu_load(void);
int32_t hk_gpu_temperature(void);
extern uint32_t gpu_dcvs, gpu_hot;
void hk_gpu_sleep(uint32_t fence, uint64_t until);
uint32_t hk_gpu_last(void);
void hk_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hk_gpu_report(const char * why);
bool hk_gpu_recover(void);
void hk_gpu_stop(void);

void HKGPUStart(void);
void HKGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHKGPUWorked       = 0x7d,         /* ours: how long the 3D core itself has worked, and on how many */
	kHKSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kHKShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hk_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHKDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hk_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};
/*
 * kHKGPUWorked: the GPU's own side of a measurement a program makes with its own clock.
 *
 * A program timing this driver (userland/src/metal_drv_test.m --time) can measure only wall
 * time: the whole of a draw, which is its own work, this ioctl, the submission, the core's
 * execution and the wait. Nothing it can reach says how much of that was the core -- and the
 * kernel has known all along, because bsd/dev/hk_adreno.c times every submission off
 * CP_ALWAYS_ON_COUNTER (19.2 ticks a microsecond) and until now only ever printed it. This
 * request hands the same three numbers to the program, so the subtraction happens where the
 * two halves are both in hand instead of in whoever reads the log afterwards.
 *
 * It RESETS what it reports, which is hk_gpu_worked()'s own contract ("since this was
 * last asked") and is what makes it useful: a caller reads once to zero the counters, does the
 * work it wants to price, and reads again. Two consequences, and they are the whole of the
 * sharp edge: two programs asking at once each get a part of the truth, and the kernel's own
 * five-second report in submit() below shares the same counters -- so a boot that reads this
 * will see gaps in those lines, and that is expected rather than a fault.
 */
struct hk_worked {
	uint64_t microseconds;                  /* the core's own time on the submissions counted here */
	uint32_t submissions;                   /* how many of them it timed; 0 means nothing finished since */
	uint32_t by[7];                         /* ... of which under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
};

#define kHKDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_SLEEP_MS            10              /* ... or, woken by its interrupt (gpu_irq), the longest sleep between two */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (hk_gpu_...) */
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */
static bool gGPUSaidSetIova;                    /* MSM_INFO_SET_IOVA has been refused out loud once */

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
} gGPUStat;

/*
 * gpu_dcvs: every GPU_SPEED_MS the core's speed follows what it has worked in that time (the core's own clock
 * at each end of every submission, hk_gpu_load()): straight to the top above GPU_SPEED_UP percent, and
 * one level down only after GPU_SPEED_CALM periods in a row under GPU_SPEED_DOWN -- a frame that waits for a
 * slow GPU is seen at once, a GPU kept fast a moment too long is not (one step at a time each way, every 100
 * ms, cost 9% of the frames of a busy second their refresh, log 192913). Never above the lowest level while
 * the hotter of its sensors reads gpu_hot or more, and until it is 5 degrees under it again. Only the levels
 * the GMU was given at its start (hk_adreno.c: 305, 400, 525 MHz on the OnePlus 8).
 */
#define GPU_SPEED_MS            50
#define GPU_SPEED_UP            50
#define GPU_SPEED_DOWN          25
#define GPU_SPEED_CALM          4
static thread_call_t gSpeedCall;
static uint32_t gSpeedLevel;            /* (gGPULock) the level asked for last */
static bool gSpeedHot;                  /* (gGPULock) held at the lowest for the heat */
static uint32_t gSpeedTime[4];          /* (gGPULock) GPU_SPEED_MS periods at each level, since the last five-second line */
static uint32_t gSpeedChanges;
static uint32_t gSpeedCalm;             /* (gGPULock) periods in a row under GPU_SPEED_DOWN */
static uint64_t gSpeedSlowest;          /* (gGPULock) the longest a change took, since the last five-second line */
static bool gGPUReported;

/* The level asked for last (bsd/dev/hk_cpufreq.c: the memory's bandwidth stays up while the GPU is above its lowest) */
extern "C" uint32_t
hk_gpu_speed_level(void)
{
	return gSpeedLevel;
}

extern "C" bool hk_display_on(void);   /* (MiniIOMobileFramebuffer.cpp) */

/* The screen lit again (bsd/dev/hk_power.c): the speed looked at now, not up to a second later */
extern "C" void
hk_gpu_speed_kick(void)
{
	if (gSpeedCall != NULL) {
		thread_call_enter(gSpeedCall);
	}
}

static void
gpuSpeed(thread_call_param_t, thread_call_param_t)
{
	uint64_t deadline;
	bool again = true;

	IOLockLock(gGPULock);
	if (!hk_gpu_ready()) {
		again = false;
	} else if (!gGPUReported) {
		uint32_t busy = (uint32_t) (hk_gpu_load() * 100 / (19200ull * GPU_SPEED_MS)), want = gSpeedLevel;
		int32_t tenths = hk_gpu_temperature();
		uint32_t top = hk_gpu_level_count() - 1;

		if (!gSpeedHot && tenths >= (int32_t) gpu_hot) {
			gSpeedHot = true;
			IOLog("HKGPU: the GPU reads %d.%d degrees: held at %u MHz\n", tenths / 10, tenths % 10,
			    hk_gpu_level_khz(1) / 1000);
		} else if (gSpeedHot && tenths < (int32_t) gpu_hot - 50) {
			gSpeedHot = false;
			IOLog("HKGPU: the GPU reads %d.%d degrees: its speed follows its load again\n", tenths / 10, tenths % 10);
		}
		gSpeedCalm = busy < GPU_SPEED_DOWN ? gSpeedCalm + 1 : 0;
		if (busy > GPU_SPEED_UP) {
			want = top;
		} else if (gSpeedCalm >= GPU_SPEED_CALM && want > 1) {
			want--;
			gSpeedCalm = 0;
		}
		if (gSpeedHot) {
			want = 1;
		}
		if (want != gSpeedLevel) {
			uint64_t began = mach_absolute_time();

			if (hk_gpu_set_level(want)) {
				gSpeedLevel = want;
				gSpeedChanges++;
				gSpeedSlowest = mach_absolute_time() - began > gSpeedSlowest ? mach_absolute_time() - began : gSpeedSlowest;
			} else {
				IOLog("HKGPU: the GMU does not take speed %u (%u MHz): the speed stays where it is\n", want,
				    hk_gpu_level_khz(want) / 1000);
				again = false;
			}
		}
		gSpeedTime[gSpeedLevel < 4 ? gSpeedLevel : 0]++;
	}
	IOLockUnlock(gGPULock);
	if (again) {
		/* (with the screen off and the GPU at its lowest, a look a second: nothing is drawn) */
		clock_interval_to_deadline(!hk_display_on() && gSpeedLevel <= 1 ? 1000 : GPU_SPEED_MS, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gSpeedCall, deadline);
	}
}

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hk_gpu_done(fence)) {
			hk_gpu_faults();
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		IOLockUnlock(gGPULock);
		if (hk_gpu_interrupts()) {
			uint64_t until;

			clock_interval_to_deadline(GPU_SLEEP_MS, kMillisecondScale, &until);
			hk_gpu_sleep(fence, until < began + patience ? until : began + patience);
		} else {
			delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		}
		IOLockLock(gGPULock);
	}
	/*
	 * A wait of a second or more that came to nothing: the core has stopped answering. What it
	 * and its SMMU context say is put down, and then, with boot-arg gpu_recover=1, the core is
	 * reset and started again, which gives up the submission that hung and leaves later ones
	 * the core's own. Without that -- or if it does not work -- nothing is waited for again in
	 * this run: a GPU that may yet write memory of ours is given nothing more. Either way this
	 * wait has failed, and says so: the submission did not finish.
	 */
	if (milliseconds >= 1000 && !gGPUReported) {
		hk_gpu_faults();                /* (a fault is often why: said, and put down to its program, first) */
		hk_gpu_report("no answer");
		gGPUReported = !hk_gpu_recover();
	}
	return false;
}

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint32_t fence = surface->drawn;
	bool done = true;

	if (fence != 0 && gGPULock != NULL) {
		IOLockLock(gGPULock);
		done = !hk_gpu_ready() || gpuWait(fence, milliseconds);
		IOLockUnlock(gGPULock);
	}
	return done;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hk_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HKGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HKGPU : public IOService
{
	OSDeclareDefaultStructors(HKGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HKGPU, IOService);

bool
HKGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HKGPUUserClient");
	setName("HKGPU");
	registerService();
	return true;
}

void
HKGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HKGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(HKGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HKGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HKGPU: the service did not start\n");
	}
	gpu->release();
	if (gpu_dcvs && (gSpeedCall = thread_call_allocate(gpuSpeed, NULL)) != NULL) {
		gSpeedLevel = hk_gpu_level_count() - 1;    /* (where step 17 left it) */
		gpuSpeed(NULL, NULL);
	}
}

/* The run is ending (hk_adreno.c): nobody is in the middle of anything when the GPU goes. */
void
HKGPUStop(void)
{
	if (gSpeedCall != NULL) {
		thread_call_cancel_wait(gSpeedCall);   /* (before the lock: it takes it) */
	}
	IOLockLock(gGPULock);
	hk_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
};
#define GPU_LEFT                64

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */

class HKGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HKGPUUserClient);

	task_t                  fTask;
	struct gpu_space *      fSpace;                 /* its page tables (gpu_pt); NULL: the one space */
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hk_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hk_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HKGPUUserClient, IOUserClient);

bool
HKGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
HKGPUUserClient::start(IOService * provider)
{
	if (!hk_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	if (gpu_pt) {
		IOLockLock(gGPULock);
		fSpace = hk_gpu_space_new();
		IOLockUnlock(gGPULock);
		if (fSpace == NULL) {
			IOLog("HKGPU: no page tables for a program\n");
			return false;
		}
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

/*
 * With the lock held, and the GPU done with everything it has been given (`idle`). If it is
 * not -- it has stopped answering -- the buffer's memory stays the GPU's for good: it may yet be
 * written, and must not be somebody else's by then.
 */
void
HKGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	bool busy = hk_gpu_ready() && b->fence != 0 && !hk_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (busy) {
			static uint32_t said;

			if (b->surface == NULL) {
				b->memory->retain();
			} else {
				b->surface->memory->retain();
			}
			if (said++ < 8) {
				IOLog("HKGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && hk_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* (now the kept one's) */
		} else if (hk_gpu_ready()) {
			hk_gpu_unmap(fSpace, b->address, b->size);
			hk_gpu_address_free(b->address, b->size);
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
HKGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (hk_gpu_ready() && !hk_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && hk_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (hk_gpu_ready()) {
			hk_gpu_unmap(fSpace, left->address, left->size);
			hk_gpu_address_free(left->address, left->size);
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
HKGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (hk_gpu_ready()) {
			gpuWait(hk_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("HKGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		/* its page tables, once the core has been switched out of them (and with nothing of the GPU's left in them) */
		if (fSpace != NULL) {
			uint32_t left = hk_gpu_space_leave(fSpace);

			if (left != 0) {
				gpuWait(left, 1000);
			}
			if (fLeftCount == 0 && hk_gpu_space_free(fSpace)) {
				fSpace = NULL;
			} else {
				IOLog("HKGPU: the GPU may still be in the page tables of a program that is gone: they stay\n");
			}
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HKGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HKGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HKGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
HKGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hk_gpu_address(size)) == 0) {
		IOLog("HKGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hk_gpu_map(fSpace, b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hk_gpu_mapped(fSpace);
	if (!mapped) {
		hk_gpu_unmap(fSpace, b->address, size);
		hk_gpu_address_free(b->address, size);
		IOLog("HKGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
HKGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hk_gpu_ready()) {
		hk_gpu_unmap(fSpace, k->address, k->size);
		hk_gpu_address_free(k->address, k->size);
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
HKGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HKGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HKGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	if (gGPUCached) {
		hk_gpu_to_memory(memory->getBytesNoCopy(), size);
	}

	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kHKDescribe), no buffer is made.
 */
IOReturn
HKGPUUserClient::surfaceBuffer(struct hk_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL || (s->scanout != NULL && s->scanoutOwner != fTask)) {
		/*
		 * (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows
		 * the GPU takes; or a panel buffer, which only the window server's GPU draws into)
		 */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = s->scanout != NULL ? IOSubMemoryDescriptor::withSubRange(s->scanout, 0, size, kIODirectionInOut) :
	    IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHKDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = s->scanout != NULL ? newBuffer(s->scanout, 0, size, 0, &request->handle) :
	    newBuffer(s->memory, PAGE_SIZE, size, 0, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kpixels;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HKGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		/*
		 * The address the buffer was given when it was made, and the buffer's until it is
		 * given back. Nothing here ever moves a live buffer, so a caller may ask once and
		 * keep the answer -- which is what freedreno does (fd_bo_init_common).
		 */
		request->value = b->address;
	} else if (request->info == MSM_INFO_SET_IOVA) {
		/*
		 * Where a buffer goes is not the program's to choose here: addresses come out of one
		 * list for every client (bsd/dev/hk_adreno.c, hk_gpu_address), with page tables
		 * of its own (gpu_pt) or without, where every client is in the one space and an
		 * address one program picked would be a place in every other program's as well.
		 * Said once, by name, rather than returned as a bare code nobody can look up.
		 */
		if (!gGPUSaidSetIova) {
			gGPUSaidSetIova = true;
			IOLog("HKGPU: a program asked to place buffer %u at %#llx itself: refused. Addresses "
			    "come out of one list for every client, so they are the kernel's to give and "
			    "MSM_INFO_GET_IOVA is how to learn them\n", request->handle, request->value);
		}
		ret = kIOReturnUnsupported;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HKGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("HKGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface != NULL && b->surface->scanout != NULL) {
			continue;                       /* (write-combined: never in this processor's cache) */
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (seed == b->seed && !b->seen && began - b->pushed < again) {
				continue;
			}
			b->seed = seed;
			b->pushed = began;
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		hk_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	hk_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("HKGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = hk_gpu_submit(fSpace, addresses, words, count);
	/*
	 * The ring is full and the core is not reading it: it has stopped. With gpu_recover=1 it is
	 * reset and given this submission again -- after a reset the ring is empty, so there is
	 * room for it. (A wait of a second or more notices a stopped core first, as a rule; this is
	 * for a program that submits without waiting.)
	 */
	if (request->fence == 0 && !gGPUReported) {
		hk_gpu_report("the ring is full");
		gGPUReported = !hk_gpu_recover();
		if (!gGPUReported) {
			request->fence = hk_gpu_submit(fSpace, addresses, words, count);
		}
	}
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hk_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
			}
		}
	}
	reap();
	hk_gpu_faults();
	if (fSubmits++ < 4) {
		IOLog("HKGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hk_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		IOLog("HKGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		IOLog("HKGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		if (gSpeedCall != NULL) {
			uint32_t periods = gSpeedTime[1] + gSpeedTime[2] + gSpeedTime[3];

			uint64_t slowest;

			absolutetime_to_nanoseconds(gSpeedSlowest, &slowest);
			IOLog("HKGPU: speed (gpu_dcvs): %u%% of the time at %u MHz, %u%% at %u, %u%% at %u; %u changes, the "
			    "longest %llu us%s\n", periods ? gSpeedTime[1] * 100 / periods : 0, hk_gpu_level_khz(1) / 1000,
			    periods ? gSpeedTime[2] * 100 / periods : 0, hk_gpu_level_khz(2) / 1000,
			    periods ? gSpeedTime[3] * 100 / periods : 0, hk_gpu_level_khz(3) / 1000, gSpeedChanges,
			    slowest / 1000, gSpeedHot ? "; held low for the heat" : "");
			bzero(gSpeedTime, sizeof(gSpeedTime));
			gSpeedChanges = 0;
			gSpeedSlowest = 0;
		}
		if (gpu_va != 0) {
			/*
			 * The address space itself, because nothing else said anything about it: what
			 * argument buffers and heaps are built on is that this allocator holds, and
			 * "lost" and "refused" are the two ways it stops holding (bsd/dev/
			 * hk_adreno.c, hk_gpu_address_free).
			 */
			uint64_t used, largest, lost;
			uint32_t live, refused;
			unsigned holes, most;

			hk_gpu_space(&used, &live, &largest, &holes, &most, &lost, &refused);
			IOLog("HKGPU: addresses: %llu MB in %u buffers, longest run free %llu MB, %u holes "
			    "(%u at most, of 1024), %llu KB lost, %u allocations refused\n", used >> 20, live,
			    largest >> 20, holes, most, lost >> 10, refused);
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
HKGPUUserClient::show(const struct hk_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HKGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HKGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hk_surface surface;
		struct hk_show show;
		struct hk_worked worked;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hk_gpu_ready() && selector != kHKShow) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		if (r.param.param == 0x09) {            /* FAULTS: requests of this program's the SMMU refused (Linux: per address space) */
			IOLockLock(gGPULock);
			hk_gpu_faults();
			r.param.value = hk_gpu_space_faults(fSpace);
			IOLockUnlock(gGPULock);
			break;
		}
		ret = hk_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(hk_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, hk_gpu_last());
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHKGPUWorked:
		/*
		 * Under gGPULock because hk_gpu_worked() calls gpu_times(), which reads the
		 * scratch page and advances the same counters the submit path advances -- and
		 * submit() holds this lock while it does exactly that. The output goes back
		 * through the common copy at the end of this method, so a caller that passes a
		 * struct of this size in gets it filled on the way out; one that passes fewer
		 * bytes gets as many as it asked for, and the rest of the numbers are lost with
		 * the reset, which is the one way to misuse this request.
		 */
		IOLockLock(gGPULock);
		hk_gpu_worked(&r.worked.microseconds, &r.worked.submissions, r.worked.by);
		IOLockUnlock(gGPULock);
		break;
	case kHKSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHKShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("HKGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
