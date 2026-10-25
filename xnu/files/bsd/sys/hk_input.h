/*
 * /dev/hk-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_HK_INPUT_H_
#define _SYS_HK_INPUT_H_

#include <stdint.h>

#define HK_TOUCH_DEVICE    "/dev/hk-touch"

#define HK_TOUCH_DOWN      1       /* a finger has landed */
#define HK_TOUCH_MOVE      2
#define HK_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define HK_BUTTON_DOWN     4       /* a hardware button; `finger` says which (HK_BUTTON_*), x and y are 0 */
#define HK_BUTTON_UP       5

#define HK_BUTTON_LOCK     1       /* the side button */
#define HK_BUTTON_VOLUME_UP 2
#define HK_BUTTON_VOLUME_DOWN 3
#define HK_BUTTON_HOME     4
#define HK_SLIDER          6       /* a three-position switch (the OnePlus 8's alert slider) is now at `finger` (HK_SLIDER_*) */

#define HK_SLIDER_DOWN     1       /* ring */
#define HK_SLIDER_MIDDLE   2       /* silent */
#define HK_SLIDER_UP       3       /* Do Not Disturb */

struct hk_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* HK_TOUCH_* or HK_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void hk_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void hk_input_init(void);
void hk_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void hk_goodix_init(void);         /* boot-arg touchscreen=1: the phone's Goodix or FocalTech touch screen */
void instantnoodle_touch_init(void);   /* board=instantnoodle: the OnePlus 8's S6SY761 touch screen */
int hk_spmi_read(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count); /* id: slave << 8 | peripheral */
int hk_spmi_write(uint16_t id, uint8_t reg, uint8_t value);  /* through the channel this system owns, read back */
void hk_buttons_init(void);        /* boot-arg buttons=1: the phone's power and volume keys */
void hk_ufs_init(void);           /* ufs=2 to xnuloader: the phone's storage, read only, through the loader (ufs_shared_pa=) */
void hk_ufs_report(const char *when); /* where reading the storage has spent its time so far */
bool hk_ufs_dev(dev_t dev);           /* one of the storage's devices */
void hk_battery_init(void);        /* boot-arg battery=1: Qualcomm UEFI charger state shared by xnuloader */
int hk_battery_temperature(void);  /* the battery's temperature in C, -273 while it is not known */
/* one transfer at a time through the PMIC arbiter for this system (bsd/dev/hk_buttons.c) */
void hk_spmi_lock(void);
void hk_spmi_unlock(void);
/*
 * How the battery is, as the audio DSP's battery manager says (bsd/dev/hk_adsp.c, with adsp=1):
 * handed to bsd/dev/hk_battery.c, which keeps it for the sysctls kern.hackios.battery_* and
 * passes it on to iokit/Kernel/HKBattery.cpp, the power source that iOS reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct hk_battery_state {
	uint32_t size;                  /* sizeof(struct hk_battery_state) */
	int32_t percent_hundredths;     /* the charge, 0..10000 */
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if a charger or a computer is on the USB port */
	int32_t status;                 /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	int32_t charge_uah, full_uah, design_uah, cycles;      /* 0 when not known (yet) */
	int32_t usb_type;               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 a computer's port, 2 a charger, ... */
	int32_t usb_voltage_uv, usb_current_ua;                 /* what the port gives */
};
void hk_battery_report(const struct hk_battery_state *state);
void hk_net_init(void);            /* en0, if there is something to carry its frames: sys/hk_net.h */
void hk_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
int hk_cpufreq_top(uint32_t domain);                 /* a group's highest row, -1 if not raised */
uint32_t hk_cpufreq_khz(uint32_t domain, int row);   /* how fast that row is, 0 if there is none */
void hk_cpufreq_cap(uint32_t domain, int row);       /* the group at that row at most (the thermal service) */
void hk_cpufreq_boost(void);       /* a touch or a key: every group at its top for a moment (the governor) */
void hk_cpufreq_display(bool on);  /* the screen off or on: the memory's bandwidth follows (bsd/dev/hk_power.c) */
void hk_thermal_init(void);        /* the thermal service: on with cpufreq=1 unless thermal=0 (bsd/dev/hk_thermal.c) */
void hk_adreno_init(void);         /* boot-arg adreno=N: the first N steps of waking the phone's GPU */
void hk_adreno_quiesce(void);      /* on the way back to the loader: the GPU block stopped, out of our memory */
void hk_adreno_stop(void);         /* before that, where the ending is no panic's: the GPU given back in order */
void hk_adreno_minute(void);       /* once a minute: a line about how warm the GPU is, if it is in use */
void hk_wifi_init(void);           /* boot-arg wifi=1: the phone's Wi-Fi; wifi_step=N: the first N steps of it */
void hk_adsp_init(void);           /* boot-arg adsp=1: the phone's audio DSP started and linked; adsp_step=N: the first N steps */
void hk_wifi_stop(void);           /* where a run ends in order: the Wi-Fi DSP, if it was started, shut down */
int hk_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi steps' log */
int hk_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one that is said even with wifi=1 */
uint32_t hk_wifi_option(const char *name);                         /* a boot-arg's number, or what wifi=1 stands for */
const uint8_t *hk_wifi_file(const char *name, uint64_t *size);     /* a file of fw/ in the small image, in place */
bool hk_wlfw_exchange(uint32_t step);                              /* step 15 of wifi_step=N: hk_wlfw.c */
bool hk_wlfw_registers(uint64_t *address, uint32_t *size, bool *mission); /* ... and what it learnt for step 16 */
void hk_wlfw_stop(uint32_t step);                                  /* ... and its end, before the DSP is stopped */
void hk_wifi_quiesce(void);        /* on the way back to the loader: the Wi-Fi DSP, if it still runs, shut down */
/* ath11k's object and the kernel's side of it (bsd/dev/hk_lx.c), and what that asks of the phone's radio */
int hk_lx_start(void);             /* the driver brought up on the radio as far as it is built: 0 or an errno */
void hk_lx_stop(void);             /* ... and down again, if it stayed up (a network joined) */
int hk_lx_prepare(void);           /* before all that: the memory the radio will be shown, taken; 0 or an errno */
void hk_lx_net_early(void);        /* ... and en0 made on the radio now, if the kernel was given its address */
bool hk_wifi_dsp_is_up(void);      /* the DSP that runs the radio's firmware has been started and has a link */
const uint8_t *hk_wifi_driver_file(const char *name, uint64_t *size);     /* a file by the name Linux has for it */
bool hk_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address); /* memory shown to the radio */
bool hk_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi); /* where the radio raises its interrupts */
bool hk_wifi_radio_address(uint8_t *address);      /* the radio's own address (boot-arg wifi_mac), if one was given */
/* the network to join (fw/network of the small image): name and passphrase, buffer sizes in and lengths out */
bool hk_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/*
 * The GPU kept on (boot-arg adreno_keep=1), for iokit/Kernel/HKGPU.cpp: see "the GPU kept on"
 * in bsd/dev/hk_adreno.c. One caller at a time.
 */
bool hk_gpu_ready(void);
bool hk_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t hk_gpu_address(uint64_t size);
void hk_gpu_address_free(uint64_t address, uint64_t size);
void hk_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most,
    uint64_t *lost, uint32_t *refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
struct gpu_space;                       /* a program's page tables (gpu_pt); NULL: the one space for everybody */
extern uint32_t gpu_pt;
struct gpu_space *hk_gpu_space_new(void);
uint32_t hk_gpu_space_leave(struct gpu_space *space);
bool hk_gpu_space_free(struct gpu_space *space);
uint32_t hk_gpu_space_faults(struct gpu_space *space);
void hk_gpu_faults(void);
bool hk_gpu_map(struct gpu_space *space, uint64_t address, uint64_t phys, bool read_only);
void hk_gpu_mapped(struct gpu_space *space);
void hk_gpu_unmap(struct gpu_space *space, uint64_t address, uint64_t size);
void hk_gpu_to_memory(void *start, uint64_t size);
uint32_t hk_gpu_submit(struct gpu_space *space, const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool hk_gpu_done(uint32_t fence);
uint32_t hk_gpu_last(void);
void hk_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void hk_gpu_report(const char *why);
bool hk_gpu_recover(void);
void hk_gpu_stop(void);
#endif

#endif /* _SYS_HK_INPUT_H_ */
