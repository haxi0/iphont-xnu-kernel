/*
 * The OnePlus 8's speakers (instantnoodle, SM8250): its two NXP TFA9874 amplifiers, set up and run as
 * Android's driver runs them, behind the hk_speaker_*() calls of sys/hk_audio.h (hk_speaker.c,
 * redwood's Awinic pair, hands every call here with board=instantnoodle).
 *
 * Android's device tree has them on the I2C bus of serial engine 15 (i2c@884000: GPIO 44 data, 45
 * clock; on the same bus are the SMB1390 charge pump and the FSA4480 USB-C switch, which nothing
 * here addresses): "tfa98xx_right" at 0x34 with its reset on GPIO 91, "tfa98xx_left" at 0x35 with
 * its reset on GPIO 99, both resets active high. They are fed by the tertiary MI2S interface, which
 * the audio DSP drives (hk_sound.c): two 32-bit slots at 48 kHz. OxygenOS 11's driver (NXP's
 * tfa98xx v6.5.3, audio_tfa9894.ko, which takes a TFA9874 or a TFA9894 by the revision register)
 * sets them up from its container file, /vendor/firmware/tfa98xx.cnt: device "P" (0x34; it alone has
 * a "receiver" profile: the earpiece) takes slot 0, device "Q" (0x35, the bottom speaker) slot 1,
 * and the mixer paths select the profile "speaker" for both. The file is the phone's own
 * (userland/build.sh puts fw-audio/ into /fw); it is not in this repository.
 *
 * The TFA9874 has no DSP of its own (NXP's driver: a "probus" device; the container's patch is a
 * dummy). Under Android its speaker protection runs in the audio DSP, and so does the volume: the
 * amplifier has neither a mute nor a volume that the driver uses. So here, as there, the level is
 * set on the samples -- the speakers' guard does it (hk_guard_digital(), hk_guard.c) -- and
 * hk_speaker_volume() only notes it.
 *
 * What NXP's driver does for a cold start of a TFA9874 (tfa_dsp_v6.c: tfa_dev_start,
 * tfaRunSpeakerBoost_v6, tfaRunStartup_v6, tfa98xx_init_v6, tfa_dev_set_state; tfa_init_v6.c:
 * tfa9874_specific; tfa_container_v6.c: tfaContWriteRegsDev_v6_v6, tfaContWriteRegsProf_v6), and
 * this does the same way:
 *
 *   prepare  the reset pin high for 5 ms, then low for 5 ms; the revision (register 3) must be a
 *            TFA9874's; AUDFS and FRACTDEL read; an I2C reset (I2CR); the DSP's reset bit (RST)
 *            set; keys 1 and 2 unlocked; the "optimal settings" of the revision; AUDFS and FRACTDEL
 *            back; the register and bit field items of the container's device list (up to its
 *            patch), then those of the profile "speaker" (up to its defaults). The chip stays in
 *            powerdown (PWDN, state 0): nothing can be heard, whatever the samples.
 *   start    only while the bit clock runs: MANSCONF, PWDN off, the clocks stable (CLKS); then the
 *            amplifier on (AMPE): state 9, "operating"
 *   stop     PWDN on and AMPE off, the state machine back in powerdown (state 0)
 *
 * There is no mute of the amplifier's own that NXP's driver uses for it (tfa_set_mute_nodsp): what is muted is the
 * samples (the guard: silence when iOS's volume is 0 or the amplifiers are off, a level that rises climbs 6 dB a
 * block), and the amplifiers are powered down after three seconds of silence (hk_sound.c). They are started while
 * what reaches them is still silence: a write of sound reaches them behind the periods queued at the DSP (160 ms).
 *
 * Every write is said in the log and read back (but those of the keys, of I2CR and of RST, which do
 * not read back as written); one that does not read back, a byte nobody acknowledges, or an alarm
 * in the status (MANALARM) has both amplifiers held in reset at once and nothing more done; on a
 * panic's way out both reset pins go high too (no I2C, no lock). Not done, compared with Android:
 * the DSP's speaker protection and the calibration (the container's "calibrate.cal" profile,
 * OnePlus's impedance check), which the speakers' guard stands in for; the amplifiers' current and
 * voltage sense, which they send back on the interface's other data line, is not read.
 *
 * Layouts: tfa9874_tfafieldnames.h and tfa98xx_parameters.h (the container) of the same driver, in
 * LineageOS's android_kernel_oneplus_sm8250, techpack/audio/asoc/codecs/tfa98xx-v6.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/hk_audio.h>
#include <sys/instantnoodle.h>

extern void loader_log_putc(char c);

static bool leaving;                    /* on a panic's way out: nothing sleeps, nothing is said but to the loader */
static bool broken;                     /* held in reset after a failure: not started again in this run */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving) {
		printf("%s", line);
	}
	return length;
}

static void
wait_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- the chips ----------------------------------------------------------------------------------- */

static struct instantnoodle_i2c bus = {
	.name = "se15",
	.sda = { INSTANTNOODLE_TLMM_SOUTH, 44 },
	.scl = { INSTANTNOODLE_TLMM_SOUTH, 45 },
	.pull = INSTANTNOODLE_PULL_UP,          /* (weak, beside whatever the board has: harmless either way) */
};

static struct amplifier {
	const char *where;
	uint8_t address;
	struct instantnoodle_pin reset;
	uint16_t rev;
	bool prepared, started;
	uint8_t away;                           /* looks in a row that found it out of its operating state (but no alarm) */
	uint16_t status[4];                     /* registers 0x10, 0x11, 0x13, 0x14 as last said */
} amplifiers[2] = {
	{ .where = "0x34 (the earpiece, slot 0)", .address = 0x34, .reset = { INSTANTNOODLE_TLMM_SOUTH, 91 } },
	{ .where = "0x35 (the bottom speaker, slot 1)", .address = 0x35, .reset = { INSTANTNOODLE_TLMM_NORTH, 99 } },
};

/* bit fields: [15:8] register, [7:4] the lowest bit, [3:0] the width less one (NXP's encoding) */
#define BF_PWDN                 0x0000
#define BF_I2CR                 0x0010
#define BF_AMPE                 0x0030
#define BF_MANSCONF             0x0120
#define BF_AUDFS                0x0203
#define BF_FRACTDEL             0x0255
#define BF_MANALARM             0x1050
#define BF_NOCLK                0x1070
#define BF_CLKS                 0x1170
#define BF_MANSTATE             0x1433
#define BF_RST                  0x9000          /* NXP's TFA2_BF_RST: the DSP's reset, set by tfa98xx_init_v6 for every TFA2 */
#define REG_REV                 0x03
#define REG_KEY_HIDDEN          0x0f
#define REG_KEY1                0xa0
#define REG_KEY2                0xa1
#define REG_KEY1_XOR            0xfb
#define STATE_POWERDOWN         0
#define STATE_ALARM             8               /* NXP's names: 10 mute, 11 the clocks being switched off */
#define STATE_OPERATING         9
#define AWAY_LOOKS              5               /* a second of hk_sound.c's looks (200 ms) */

/* Both chips held in reset, at once: without I2C (a chip that does not answer is silenced as well) and without a lock. */
static void
hold_in_reset(const char *why)
{
	for (unsigned i = 0; i < 2; i++) {
		instantnoodle_gpio_output(amplifiers[i].reset, true, INSTANTNOODLE_PULL_NONE, 2);
		amplifiers[i].prepared = amplifiers[i].started = false;
	}
	broken = true;
	say("speaker: BOTH AMPLIFIERS ARE HELD IN RESET: %s\n", why);
}

static bool
get(struct amplifier *a, uint8_t reg, uint16_t *value)
{
	uint8_t bytes[2];

	if (instantnoodle_i2c_read(&bus, a->address, reg, bytes, 2) != 0) {
		say("speaker: %s: register %#04x: NO ANSWER\n", a->where, reg);
		return false;
	}
	*value = (uint16_t)(bytes[0] << 8 | bytes[1]);
	return true;
}

/* Written and said, and read back if `check'; on any failure both are held in reset. */
static bool
put(struct amplifier *a, uint8_t reg, uint16_t value, bool check)
{
	uint8_t bytes[3] = { reg, (uint8_t)(value >> 8), (uint8_t)value };
	uint16_t back = 0;

	say("speaker: %s: register %#04x = %#06x\n", a->where, reg, value);
	if (instantnoodle_i2c_write(&bus, a->address, bytes, sizeof(bytes)) != 0) {
		hold_in_reset("a write was not acknowledged");
		return false;
	}
	if (check && (!get(a, reg, &back) || back != value)) {
		say("speaker: %s: register %#04x reads back %#06x\n", a->where, reg, back);
		hold_in_reset("a register did not read back as written");
		return false;
	}
	return true;
}

/* what reads back as written: all but the keys, and RST (reserved on a chip without a DSP) */
static bool
readable(uint8_t reg)
{
	return reg != REG_KEY_HIDDEN && reg != REG_KEY1 && reg != REG_KEY2 && reg != (BF_RST >> 8);
}

static uint16_t
field_mask(uint16_t field)
{
	return (uint16_t)(((1u << ((field & 15) + 1)) - 1) << (field >> 4 & 15));
}

static bool
get_field(struct amplifier *a, uint16_t field, uint16_t *value)
{
	uint16_t reg;

	if (!get(a, (uint8_t)(field >> 8), &reg)) {
		return false;
	}
	*value = (uint16_t)((reg & field_mask(field)) >> (field >> 4 & 15));
	return true;
}

/* as NXP's tfa_set_bf_v6: read, change the field, written only if that changes the register */
static bool
set_field(struct amplifier *a, uint16_t field, uint16_t value)
{
	uint8_t reg = (uint8_t)(field >> 8);
	uint16_t old, mask = field_mask(field), new;

	if (!get(a, reg, &old)) {
		hold_in_reset("a register could not be read");
		return false;
	}
	new = (uint16_t)((old & ~mask) | ((value << (field >> 4 & 15)) & mask));
	return new == old || put(a, reg, new, readable(reg));
}

/* as NXP's tfaRunWriteRegister: the masked bits of the register */
static bool
set_masked(struct amplifier *a, uint8_t reg, uint16_t value, uint16_t mask)
{
	uint16_t old;

	if (!get(a, reg, &old)) {
		hold_in_reset("a register could not be read");
		return false;
	}
	return put(a, reg, (uint16_t)((old & ~mask) | (value & mask)), readable(reg));
}

/* ---- the container (tfa98xx.cnt) ------------------------------------------------------------------- */

#define DSC_PROFILE             1
#define DSC_REGISTER            2
#define DSC_FILE                4
#define DSC_PATCH               5
#define DSC_BITFIELD            16
#define DSC_DEFAULT             17
#define DSC_NO_INIT             24
#define CONTAINER_INDEX         46              /* after the header: id, versions, size, CRC, rev, customer, application, type, counts */

static const uint8_t *cnt;
static uint64_t cnt_size;

static uint32_t
le32(uint64_t at)
{
	return (uint32_t)cnt[at] | (uint32_t)cnt[at + 1] << 8 | (uint32_t)cnt[at + 2] << 16 | (uint32_t)cnt[at + 3] << 24;
}

static uint16_t
le16(uint64_t at)
{
	return (uint16_t)(cnt[at] | cnt[at + 1] << 8);
}

/* a descriptor at `at': its item's offset in the file and its type; false if either is out of the file */
static bool
descriptor(uint64_t at, uint32_t *offset, uint8_t *type)
{
	if (at + 4 > cnt_size) {
		return false;
	}
	*offset = le32(at) & 0xffffff;
	*type = (uint8_t)(le32(at) >> 24);
	return *offset < cnt_size;
}

/* the string a descriptor points at, if it ends within the file */
static const char *
string_at(uint64_t at)
{
	uint32_t offset;
	uint8_t type;

	if (!descriptor(at, &offset, &type) || memchr(cnt + offset, 0, cnt_size - offset) == NULL) {
		return NULL;
	}
	return (const char *)cnt + offset;
}

/* The device list of an amplifier: at *list, its *length items' descriptors from *list + 12. */
static bool
device_list(uint8_t address, uint64_t *list, uint32_t *length)
{
	uint16_t devices;

	if (cnt_size < CONTAINER_INDEX || cnt[0] != 'P' || cnt[1] != 'M' || le32(6) > cnt_size) {
		say("speaker: the container is not one of NXP's (\"PM\", its size within the file)\n");
		return false;
	}
	devices = le16(40);
	for (uint32_t d = 0; d < devices; d++) {
		uint32_t offset;
		uint8_t type;

		if (!descriptor(CONTAINER_INDEX + 4 * d, &offset, &type) || type != 0 || offset + 12 > cnt_size) {
			say("speaker: the container's device %u is not where its index says\n", d);
			return false;
		}
		if (cnt[offset + 1] == 0 && cnt[offset + 2] == address) {
			*list = offset;
			*length = cnt[offset];
			if (offset + 12 + 4ull * *length > cnt_size) {
				say("speaker: the container's device %#x runs past the file\n", address);
				return false;
			}
			return true;
		}
	}
	say("speaker: the container has no device at %#x\n", address);
	return false;
}

/* A register (address, value, mask) or bit field (value, field) item: written; any other type is not one. */
static bool
write_item(struct amplifier *a, uint32_t offset, uint8_t type, bool *was)
{
	*was = true;
	if (type == DSC_REGISTER && offset + 5 <= cnt_size) {
		return set_masked(a, cnt[offset], le16(offset + 1), le16(offset + 3));
	}
	if (type == DSC_BITFIELD && offset + 4 <= cnt_size) {
		return set_field(a, le16(offset + 2), le16(offset));
	}
	*was = false;
	return true;
}

/* The device's items up to its patch, file or profile (tfaContWriteRegsDev_v6_v6), then the profile's up to its defaults. */
static bool
write_container(struct amplifier *a, uint64_t list, uint32_t length, const char *profile_name)
{
	uint32_t written = 0, profiles = 0, offset;
	uint8_t type;
	bool was, in_device = true;

	for (uint32_t i = 0; i < length; i++) {
		if (!descriptor(list + 12 + 4 * i, &offset, &type)) {
			say("speaker: %s: item %u of the device is not within the container\n", a->where, i);
			return false;
		}
		if (type == DSC_PATCH || type == DSC_FILE || type == DSC_PROFILE) {
			in_device = false;
		}
		if (in_device) {
			if (!write_item(a, offset, type, &was)) {
				return false;
			}
			written += was;
			continue;
		}
		if (type != DSC_PROFILE) {
			continue;
		}
		/* a profile: { length:8, group:8, id:16 }, its name, then length - 1 items */
		{
			uint32_t items = offset + 8 <= cnt_size ? cnt[offset] : 0, profile_written = 0, item_offset;
			const char *name = string_at(offset + 4);

			profiles++;
			if (name == NULL || strcmp(name, profile_name) != 0) {
				continue;
			}
			if (items == 0 || offset + 8 + 4ull * (items - 1) > cnt_size) {
				say("speaker: %s: the profile \"%s\" runs past the container\n", a->where, name);
				return false;
			}
			say("speaker: %s: %u items of the device's list written; now its profile %u, \"%s\"\n", a->where, written,
			    profiles - 1, name);
			for (uint32_t k = 0; k + 1 < items; k++) {
				if (!descriptor(offset + 8 + 4 * k, &item_offset, &type)) {
					return false;
				}
				if (type == DSC_DEFAULT) {
					break;          /* (what restores the bit fields when another profile is chosen) */
				}
				if (!write_item(a, item_offset, type, &was)) {
					return false;
				}
				profile_written += was;
			}
			say("speaker: %s: %u items of the profile written\n", a->where, profile_written);
			return true;
		}
	}
	say("speaker: %s: the container has no profile \"%s\" for it\n", a->where, profile_name);
	return false;
}

/* ---- prepare, start, stop -------------------------------------------------------------------------------- */

/* NXP's "optimal settings" per revision of the TFA9874 (tfa_init_v6.c: tfa9874_specific) */
static const struct {
	uint16_t rev;
	uint8_t count;
	struct {
		uint8_t reg;
		uint16_t value;
	} writes[16];
} optimal[] = {
	{ 0x0a74, 16, { { 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 }, { 0x61, 0x0110 }, { 0x66, 0x0701 },
	                { 0x6f, 0x00a3 }, { 0x70, 0x07f8 }, { 0x73, 0x0007 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0594 },
	                { 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 } } },
	{ 0x0b74, 16, { { 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 }, { 0x61, 0x0110 }, { 0x66, 0x0701 },
	                { 0x6f, 0x00a3 }, { 0x70, 0x07f8 }, { 0x73, 0x0047 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0595 },
	                { 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 } } },
	{ 0x0c74, 13, { { 0x02, 0x22c8 }, { 0x52, 0x57dc }, { 0x53, 0x003e }, { 0x56, 0x0400 }, { 0x61, 0x0110 }, { 0x6f, 0x00a5 },
	                { 0x70, 0x07f8 }, { 0x73, 0x0047 }, { 0x74, 0x5098 }, { 0x75, 0x8d28 }, { 0x80, 0x0000 }, { 0x83, 0x0799 },
	                { 0x84, 0x0081 } } },
};

/* The reset pin pulsed (as tfa98xx_ext_reset), and the revision: true for a TFA9874. */
static bool
reset_and_identify(struct amplifier *a)
{
	instantnoodle_gpio_output(a->reset, true, INSTANTNOODLE_PULL_NONE, 2);
	wait_ms(5);
	instantnoodle_gpio_output(a->reset, false, INSTANTNOODLE_PULL_NONE, 2);
	wait_ms(5);
	if (!get(a, REG_REV, &a->rev)) {
		instantnoodle_i2c_recover(&bus);
		if (!get(a, REG_REV, &a->rev)) {
			return false;
		}
	}
	say("speaker: %s: revision %#06x (%s)\n", a->where, a->rev, (a->rev & 0xff) == 0x74 ? "a TFA9874" :
	    (a->rev & 0xff) == 0x94 ? "a TFA9894" : "not a chip of this family");
	return (a->rev & 0xff) == 0x74;
}

static bool
prepare_one(struct amplifier *a)
{
	uint64_t list = 0;
	uint32_t length = 0, offset;
	uint16_t audfs, fractdel, value, state;
	uint8_t type;
	bool init = true;

	if (!device_list(a->address, &list, &length)) {
		return false;
	}
	for (uint32_t i = 0; i < length; i++) {
		if (descriptor(list + 12 + 4 * i, &offset, &type) && type == DSC_NO_INIT) {
			init = false;           /* (as tfaRunStartup_v6) */
		}
	}
	if (!reset_and_identify(a)) {
		say("speaker: %s: the container is for a TFA9874; nothing more is done\n", a->where);
		return false;
	}
	if (!get_field(a, BF_AUDFS, &audfs) || !get_field(a, BF_FRACTDEL, &fractdel)) {
		return false;
	}
	if (init) {
		unsigned r;

		/* tfa98xx_init_v6: the I2C registers to their defaults, the DSP's reset; then tfa9874_specific */
		if (!put(a, BF_I2CR >> 8, 1u << (BF_I2CR >> 4 & 15), false) || !set_field(a, BF_RST, 1) ||
		    !put(a, REG_KEY_HIDDEN, 0x5a6b, false) || !get(a, REG_KEY1_XOR, &value) ||
		    !put(a, REG_KEY1, (uint16_t)(value ^ 0x005a), false) ||
		    /* tfa98xx_key2_v6(tfa, 0) */
		    !put(a, REG_KEY_HIDDEN, 0x5a6b, false) || !put(a, REG_KEY2, 0x005a, false) || !put(a, REG_KEY_HIDDEN, 0, false)) {
			return false;
		}
		for (r = 0; r < sizeof(optimal) / sizeof(optimal[0]) && optimal[r].rev != a->rev; r++) {
		}
		if (r == sizeof(optimal) / sizeof(optimal[0])) {
			say("speaker: %s: NXP's driver has no optimal settings for revision %#x either (it goes on without them)\n",
			    a->where, a->rev);
		}
		for (unsigned w = 0; r < sizeof(optimal) / sizeof(optimal[0]) && w < optimal[r].count; w++) {
			if (!put(a, optimal[r].writes[w].reg, optimal[r].writes[w].value, true)) {
				return false;
			}
		}
		if (!set_field(a, BF_AUDFS, audfs) || !set_field(a, BF_FRACTDEL, fractdel)) {
			return false;
		}
	}
	if (!write_container(a, list, length, "speaker")) {
		hold_in_reset("the container's items could not be written");
		return false;
	}
	/* nothing so far leaves powerdown: make sure of it */
	if (!get_field(a, BF_PWDN, &value) || !get_field(a, BF_MANSTATE, &state) || value != 1 || state != STATE_POWERDOWN) {
		say("speaker: %s: PWDN %u, state %u after the set-up\n", a->where, value, state);
		hold_in_reset("an amplifier is not in powerdown after its set-up");
		return false;
	}
	get_field(a, BF_AUDFS, &audfs);
	say("speaker: %s: set up from the container, in powerdown (state 0), AUDFS %u\n", a->where, audfs);
	a->prepared = true;
	return true;
}

bool
instantnoodle_speaker_prepare(const uint8_t *container, uint64_t size)
{
	if (broken) {
		return false;
	}
	cnt = container;
	cnt_size = size;
	for (unsigned i = 0; i < 2; i++) {
		if (!prepare_one(&amplifiers[i])) {
			cnt = NULL;
			if (!broken) {
				hold_in_reset("an amplifier could not be set up");
			}
			return false;
		}
	}
	cnt = NULL;                             /* (the caller gives the file back) */
	say("speaker: BOTH AMPLIFIERS ARE SET UP AND POWERED DOWN (TFA9874, the profile \"speaker\" of Android's container)\n");
	return true;
}

/* The state machine at `want' within `ms'. */
static bool
reach(struct amplifier *a, uint16_t want, uint32_t ms, uint16_t *state)
{
	for (uint32_t waited = 0;; waited += 2) {
		if (!get_field(a, BF_MANSTATE, state)) {
			return false;
		}
		if (*state == want || waited >= ms) {
			return *state == want;
		}
		wait_ms(2);
	}
}

/* Only while the bit clock runs (tfa_dev_set_state: INIT_CF, then OPERATING, as for a probus device). */
bool
instantnoodle_speaker_start(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t clocks = 0, state = 0;
		unsigned tries;

		if (broken || !a->prepared) {
			say("speaker: %s is not set up: not started\n", a->where);
			return false;
		}
		if (a->started) {
			continue;
		}
		say("speaker: the amplifier %s is started as Android's driver starts it\n", a->where);
		if (!set_field(a, BF_MANSCONF, 1) || !set_field(a, BF_PWDN, 0)) {
			return false;
		}
		for (tries = 0; tries < 50 && get_field(a, BF_CLKS, &clocks) && !clocks; tries++) {
			wait_ms(1);
		}
		if (!clocks) {
			hold_in_reset("the clocks do not become stable (no bit clock?)");
			return false;
		}
		if (!set_field(a, BF_AMPE, 1)) {
			return false;
		}
		a->started = true;
		a->away = 0;
		if (!reach(a, STATE_OPERATING, 200, &state)) {
			say("speaker: %s: state %u, not operating, 200 ms after AMPE\n", a->where, state);
			hold_in_reset("an amplifier did not reach its operating state");
			return false;
		}
		say("speaker: THE AMPLIFIER %s IS OPERATING (state 9): what reaches it is heard\n", a->where);
	}
	return true;
}

/* Nothing to set on the chips: the level is the samples' (hk_guard_digital()). */
bool
instantnoodle_speaker_volume(uint32_t below, __unused uint32_t step_ms)
{
	static uint32_t said = UINT32_MAX;

	if (below != said) {
		say("speaker: the level, %u.%03u dB under Android's, is set on the samples: the TFA9874 has no volume of its own\n",
		    below / 8, below % 8 * 125);
		said = below;
	}
	return !broken;
}

/* The status words, said when they change; false (and both held in reset) on an alarm or a state not operating. */
bool
instantnoodle_speaker_healthy(void)
{
	static const uint8_t regs[4] = { 0x10, 0x11, 0x13, 0x14 };

	if (broken) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t now[4];
		unsigned alarm, state;

		if (!a->started) {
			continue;
		}
		for (unsigned r = 0; r < 4; r++) {
			if (!get(a, regs[r], &now[r])) {
				hold_in_reset("an amplifier does not answer while it plays");
				return false;
			}
		}
		if (memcmp(now, a->status, sizeof(now)) != 0) {
			say("speaker: %s: status 0x10 %#06x, 0x11 %#06x, 0x13 %#06x, 0x14 %#06x\n", a->where, now[0], now[1], now[2],
			    now[3]);
			memcpy(a->status, now, sizeof(now));
		}
		alarm = now[0] >> (BF_MANALARM >> 4 & 15) & 1;
		state = (now[3] & field_mask(BF_MANSTATE)) >> (BF_MANSTATE >> 4 & 15);
		if (alarm || state == STATE_ALARM) {
			say("speaker: %s: alarm %u, state %u\n", a->where, alarm, state);
			hold_in_reset("an amplifier reports an alarm");
			return false;
		}
		a->away = state == STATE_OPERATING ? 0 : a->away + 1;
		if (a->away >= AWAY_LOOKS) {
			say("speaker: %s: state %u for %u looks\n", a->where, state, a->away);
			hold_in_reset("an amplifier stays out of its operating state");
			return false;
		}
	}
	return true;
}

/* The TFA9874's own watch of temperature and battery is not read: no reduction is asked for. */
uint32_t
instantnoodle_speaker_monitor(__unused bool tell)
{
	return 0;
}

/* tfa_dev_stop: powerdown, AMPE off (NXP's PLMA5337), and the state machine back in powerdown. */
void
instantnoodle_speaker_stop(void)
{
	for (unsigned i = 0; i < 2 && !broken; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t state = 0;

		if (!a->started) {
			continue;
		}
		say("speaker: the amplifier %s is stopped as Android's driver stops it\n", a->where);
		if (!set_field(a, BF_PWDN, 1) || !set_field(a, BF_AMPE, 0)) {
			return;
		}
		a->started = false;
		if (!reach(a, STATE_POWERDOWN, 200, &state)) {
			say("speaker: %s: state %u, 200 ms after powerdown\n", a->where, state);
			hold_in_reset("an amplifier did not power down");
			return;
		}
		say("speaker: THE AMPLIFIER %s IS POWERED DOWN\n", a->where);
	}
}

void
instantnoodle_speaker_quiesce(void)
{
	if (!amplifiers[0].prepared && !amplifiers[1].prepared) {
		return;
	}
	leaving = true;
	hold_in_reset("the kernel is on its way out");
}

/*
 * boot-arg speaker_probe=1: the pins, as the boot chain left them; =2: ... and each chip reset and asked
 * its revision, state and a few registers; =3: ... and both set up from /fw/tfa98xx.cnt as for sound=4,
 * early, while the log keeps everything. None starts anything: the chips stay in powerdown.
 */
void
instantnoodle_speaker_probe(uint32_t level)
{
	static const struct {
		struct instantnoodle_pin pin;
		const char *what;
	} pins[] = {
		{ { INSTANTNOODLE_TLMM_SOUTH, 44 }, "I2C data" }, { { INSTANTNOODLE_TLMM_SOUTH, 45 }, "I2C clock" },
		{ { INSTANTNOODLE_TLMM_SOUTH, 91 }, "reset of 0x34" }, { { INSTANTNOODLE_TLMM_NORTH, 99 }, "reset of 0x35" },
	};

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = instantnoodle_gpio_cfg(pins[i].pin);

		say("speaker: gpio%u (%s): function %u, pull %u, %s, reads %u\n", pins[i].pin.pin, pins[i].what, cfg >> 2 & 15, cfg & 3,
		    cfg & (1u << 9) ? "output" : "input", instantnoodle_gpio_level(pins[i].pin));
	}
	if (level < 2) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t r00 = 0, r02 = 0, r10 = 0, r14 = 0;

		if (reset_and_identify(a) && get(a, 0x00, &r00) && get(a, 0x02, &r02) && get(a, 0x10, &r10) && get(a, 0x14, &r14)) {
			say("speaker: %s: 0x00 %#06x (PWDN %u, AMPE %u), 0x02 %#06x, 0x10 %#06x, 0x14 %#06x (state %u)\n", a->where, r00,
			    r00 & 1, r00 >> 3 & 1, r02, r10, r14, (r14 & field_mask(BF_MANSTATE)) >> 3);
		}
	}
	if (level >= 3) {
		const uint8_t *file;
		uint64_t size = 0;

		for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
			wait_ms(200);           /* (the file is on the root, as for adsp=1) */
		}
		file = hk_adsp_fw_file("tfa98xx.cnt", &size);
		if (file == NULL) {
			say("speaker: no /fw/tfa98xx.cnt: put the phone's /vendor/firmware/tfa98xx.cnt into fw-audio/\n");
		} else {
			instantnoodle_speaker_prepare(file, size);
			hk_adsp_fw_free(file, size);
		}
	}
	say("speaker: probe done (bus %s: %u transfers not acknowledged)\n", bus.name, bus.errors);
}
