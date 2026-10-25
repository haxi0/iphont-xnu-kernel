/* ------------------------------------------------------------------------ */
/* Standalone boot from the flash, and the dual-boot menu                   */
/*
 * Included by loader.c. An installed phone boots without a computer: the UEFI in the spare A/B slot's
 * boot partition starts this loader with no payload, and the loader
 *
 *   1. shows a menu -- HackiOS (the default, after a few seconds), the previous HackiOS when there is
 *      one, Android, or developer mode (the USB link that `xl` talks to, as `xl enter` gives);
 *   2. for HackiOS reads the boot bundle -- the kernel, the boot volume and the command line, the same
 *      "XNUP" container `xl` sends -- from the boot area of darwin into the payload window, checks it and
 *      runs it with no time limit: a session lasts until the phone is switched off or restarted;
 *   3. for Android makes the other slot active again and resets.
 *
 * Every power-on shows the menu, like a PC's: after Android has come up, the HackiOS module in it
 * (common/flash/rearm, for KernelSU or Magisk) makes this slot active again, so the next start comes
 * here first; and the countdown takes what was chosen last time (FB_LAST_ANDROID in the boot state).
 *
 * The boot area is the free stretch of darwin between the cryptexes and the var store's slots (README of
 * the userland repository: "Data that survives a reboot"): two bundle slots used in turn, each a header
 * block and the container behind it, and one block of boot state (formats: common/flash/bundlefmt.py).
 * An update writes the slot that does not hold the newest good bundle -- zeroed header first, container,
 * header last -- so an update that is cut off loses the update and not the phone: the other slot still
 * boots.
 *
 * What is checked before a bundle runs: the header's own sum (a torn header is no header), the SHA-256 of
 * the container, and -- when the loader was built with an owner key pinned (common/flash/ownerkey.py) --
 * the Ed25519 signature of the header by that key. A bundle that fails any of them is not run.
 *
 * The boot guard: before it starts a bundle the loader counts the start in the boot state, and iOS says
 * it came up (userland: var_store bootok, from the var store's job a minute into the run). A generation
 * that was started FB_MAX_TRIES times in a row without coming up is skipped by the countdown: the
 * previous one boots instead, and when that has failed too, Android. A choice made with the keys is
 * always honoured -- the person holding the phone decides -- and still counted.
 *
 * Android's A/B slots are switched the way Qualcomm's boot control does it: the A/B byte of the GPT
 * attributes of boot_a and boot_b (bits 48..55: priority, active, retry count, successful, unbootable).
 * The loader never invents those bits: going back to Android it exchanges the two entries' A/B bytes,
 * which is exactly the state the phone had before `bootctl set-active-boot-slot` sent it here. It checks
 * the state first (one of the two active, and the other one), writes the backup GPT before the primary
 * one, and reads both back. Outside darwin's boot area nothing else is ever written.
 */

#include "crypto.c"
#ifdef OWNER_KEY_FILE
#include OWNER_KEY_FILE             /* a key for one build (common/flash/qemu-test.sh) */
#else
#include "owner_key.h"              /* xnuloader/build.sh makes an unpinned one when there is none */
#endif

#define FB_BLOCK          4096ULL                 /* the boot area is laid out in darwin's 4 KiB blocks */
#define FB_AREA_FIRST     2042368ULL              /* = BLOCK_WRITE_FIRST: right after both cryptexes */
#define FB_SLOT_BLOCKS    24576ULL                /* 96 MiB a slot; two slots end at 2091520 < 2097152 (var store) */
#define FB_SLOTS          2
#define FB_STATE_BLOCK    (FB_AREA_FIRST + FB_SLOTS * FB_SLOT_BLOCKS)
#define FB_MENU_SECONDS   5
#define FB_MAX_TRIES      3
#define FB_SIGNED         1ULL
#define FB_HOLD           1u
#define FB_LAST_ANDROID   2u                      /* the last choice was Android: the countdown offers it again */
#define FB_OFF_CHARGING   4u                      /* switched off with the cable in: see fb_offmode() */

static const char fb_magic[8] = { 'H', 'K', 'B', 'O', 'O', 'T', '0', '2' };
static const char fb_state_magic[8] = { 'H', 'K', 'S', 'T', 'A', 'T', 'E', '1' };

struct fb_header {
	char magic[8];
	uint64_t generation;                    /* the newest good slot wins */
	uint64_t size;                          /* bytes of container behind the header block */
	uint64_t flags;                         /* FB_SIGNED */
	char label[32];                         /* for the menu */
	uint8_t sha256[32];                     /* of the container */
	uint8_t signature[64];                  /* Ed25519 over the 96 bytes above */
	uint64_t header_sum;                    /* FNV-1a 64 of the 160 bytes above */
};

struct fb_state {
	char magic[8];
	uint64_t trying;                        /* the generation started last */
	uint32_t tries;                         /* started that many times in a row, not come up */
	uint32_t flags;                         /* FB_HOLD: the previous version, asked for (flashctl rollback);
	                                         * FB_LAST_ANDROID: the last choice, for the countdown's default;
	                                         * FB_OFF_CHARGING: switched off with the cable in */
	uint64_t good;                          /* the last generation that came up */
	uint64_t boots;                         /* starts of HackiOS */
	uint64_t bad;                           /* a generation given up on: the countdown skips it */
	uint64_t sum;                           /* FNV-1a 64 of the 48 bytes above */
};

typedef struct { uint16_t scan, unicode; } efi_input_key;
typedef struct efi_simple_text_in {
	void *reset;
	efi_status (EFIAPI *read_key_stroke)(struct efi_simple_text_in *self, efi_input_key *key);
	void *wait_for_key;
} efi_simple_text_in;

/* what the boot area holds, read once before the menu */
static struct {
	efi_block_io *bio;
	struct fb_header hdr[FB_SLOTS];
	int ok[FB_SLOTS];                       /* the header is sound (and signed, when a key is pinned) */
	int newest, older;                      /* slot indices, -1 when there is none */
	struct fb_state st;
	int have_state;
} fb;

static uint64_t
fb_fnv(const uint8_t *p, uint64_t n, uint64_t h)
{
	for (uint64_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int
fb_name_is(const efi_partition_info *info, const char *want)
{
	char name[40];

	ufs_name(info, name, sizeof(name));
	return str_eq(name, want);
}

/*
 * darwin's BlockIo, wherever the firmware lists it (a phone's UFS unit, a QEMU disk). On a device whose
 * port has a store of its own (struct platform .store: redwood's darwin, the OnePlus 8's whole userdata)
 * that partition, and only when its GUIDs and LBAs are the ones it was set up with -- a name alone, like
 * "userdata", says too little to write into it.
 */
static efi_block_io *
fb_find_darwin(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	const char *want = PLAT->store != NULL ? PLAT->store : "darwin";
	efi_handle *handles;
	uint64_t count;

	if (DARWIN_FILE) {
		return darwin_file_bio();               /* a file of userdata, through its map (darwinfile.c) */
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || !fb_name_is(info, want)) {
			continue;
		}
		if (PLAT->store != NULL && (memcmp(&info->type_guid, &PLAT->store_type, sizeof(efi_guid)) != 0 ||
		    memcmp(&info->unique_guid, &PLAT->store_uniq, sizeof(efi_guid)) != 0 ||
		    info->start_lba != PLAT->store_first || info->end_lba != PLAT->store_last)) {
			print("flash: %s is not the partition the port was set up with (GUIDs, LBAs): not used\n", want);
			continue;
		}
		return bio;
	}
	return NULL;
}

/* read `bytes` from darwin at 4 KiB block `blk` into `dst` (its own LBA size may be 512 or 4096) */
static int
fb_read(efi_block_io *bio, uint64_t blk, void *dst, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, lba = blk * FB_BLOCK / bs, done = 0;
	const uint64_t chunk = 4ULL << 20;

	bytes = (bytes + bs - 1) / bs * bs;
	while (done < bytes) {
		uint64_t n = bytes - done < chunk ? bytes - done : chunk;
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, lba + done / bs, n, (uint8_t *)dst + done))) {
			return 0;
		}
		done += n;
	}
	return 1;
}

static int
fb_header_sound(const struct fb_header *h)
{
	return memcmp(h->magic, fb_magic, sizeof(fb_magic)) == 0 && h->size > 16 &&
	    h->size <= PAYLOAD_MAX_SIZE - 0x100000ULL && h->size <= (FB_SLOT_BLOCKS - 1) * FB_BLOCK &&
	    fb_fnv((const uint8_t *)h, 160, 0xcbf29ce484222325ULL) == h->header_sum;
}

/* the owner's signature: required when a key is pinned, otherwise only reported */
static int
fb_header_trusted(int s, const struct fb_header *h)
{
	if (!OWNER_KEY_PINNED) {
		return 1;
	}
	if (!(h->flags & FB_SIGNED)) {
		print("flash: boot slot %d: not signed, and this loader takes only bundles signed by key %s\n",
		    (uint64_t)s, owner_key_fp);
		return 0;
	}
	if (!ed25519_verify(h->signature, (const uint8_t *)h, 96, owner_key)) {
		print("flash: boot slot %d: the signature is not by key %s: REFUSED\n", (uint64_t)s, owner_key_fp);
		return 0;
	}
	return 1;
}

static int
fb_state_sound(const struct fb_state *st)
{
	return memcmp(st->magic, fb_state_magic, sizeof(fb_state_magic)) == 0 &&
	    fb_fnv((const uint8_t *)st, 48, 0xcbf29ce484222325ULL) == st->sum;
}

/* the boot state back into darwin, and read back; a guard that cannot write is said so and boots anyway */
static int
fb_state_write(void)
{
	uint64_t bs = fb.bio->media->block_size, lba = FB_STATE_BLOCK * FB_BLOCK / bs;

	memcpy(fb.st.magic, fb_state_magic, sizeof(fb_state_magic));
	fb.st.sum = fb_fnv((const uint8_t *)&fb.st, 48, 0xcbf29ce484222325ULL);
	memset(ufs_buffer, 0, FB_BLOCK);
	memcpy(ufs_buffer, &fb.st, sizeof(fb.st));
	if (EFI_ERROR(fb.bio->write_blocks(fb.bio, fb.bio->media->media_id, lba, FB_BLOCK, ufs_buffer))) {
		print("flash: the boot state could not be written: the boot guard is off for this start\n");
		return 0;
	}
	if (fb.bio->flush_blocks) {
		fb.bio->flush_blocks(fb.bio);
	}
	if (!fb_read(fb.bio, FB_STATE_BLOCK, ufs_buffer, FB_BLOCK) || memcmp(ufs_buffer, &fb.st, sizeof(fb.st)) != 0) {
		print("flash: the boot state did not read back the same: the boot guard is off for this start\n");
		return 0;
	}
	return 1;
}

/* Read both headers and the boot state. 0 when there is no darwin. */
static int
fb_scan(void)
{
	memset(&fb, 0, sizeof(fb));
	fb.newest = fb.older = -1;
	fb.bio = fb_find_darwin();
	if (fb.bio == NULL) {
		print("flash: no darwin partition: HackiOS is not installed on this device\n");
		return 0;
	}
	print("flash: loader %s\n", OWNER_KEY_PINNED ? "with an owner key pinned: signed bundles only" :
	    "without an owner key: unsigned bundles are taken (common/flash/ownerkey.py)");
	if (OWNER_KEY_PINNED) {
		print("flash: owner key %s\n", owner_key_fp);
	}
	for (int s = 0; s < FB_SLOTS; s++) {
		struct fb_header *h = &fb.hdr[s];

		if (fb_read(fb.bio, FB_AREA_FIRST + s * FB_SLOT_BLOCKS, ufs_buffer, FB_BLOCK)) {
			memcpy(h, ufs_buffer, sizeof(*h));
		}
		h->label[sizeof(h->label) - 1] = 0;
		fb.ok[s] = fb_header_sound(h) && fb_header_trusted(s, h);
		if (fb.ok[s]) {
			print("flash: boot slot %d: generation %d \"%s\", %d bytes, %s\n", (uint64_t)s, h->generation,
			    h->label, h->size, (h->flags & FB_SIGNED) ? "signed" : "unsigned");
		} else {
			print("flash: boot slot %d: empty, torn or refused\n", (uint64_t)s);
		}
	}
	for (int s = 0; s < FB_SLOTS; s++) {
		if (!fb.ok[s]) {
			continue;
		}
		if (fb.newest < 0 || fb.hdr[s].generation > fb.hdr[fb.newest].generation) {
			fb.older = fb.newest;
			fb.newest = s;
		} else {
			fb.older = s;
		}
	}
	if (fb_read(fb.bio, FB_STATE_BLOCK, ufs_buffer, FB_BLOCK)) {
		memcpy(&fb.st, ufs_buffer, sizeof(fb.st));
	}
	fb.have_state = fb_state_sound(&fb.st);
	if (!fb.have_state) {
		memset(&fb.st, 0, sizeof(fb.st));
	}
	print("flash: boot state: %s, last started %d (%d times), last up %d, given up on %d, %d starts%s\n",
	    fb.have_state ? "kept" : "new", fb.st.trying, (uint64_t)fb.st.tries, fb.st.good, fb.st.bad,
	    fb.st.boots, (fb.st.flags & FB_HOLD) ? ", holding to the previous version" : "");
	return 1;
}

/* would the countdown boot slot s? (a generation that keeps failing to come up: no) */
static int
fb_guard_allows(int s)
{
	uint64_t g = fb.hdr[s].generation;

	if (g == fb.st.bad) {
		return 0;
	}
	return !(g == fb.st.trying && fb.st.tries >= FB_MAX_TRIES && fb.st.good != g);
}

/* count the start of slot s in the boot state */
static void
fb_count_start(int s)
{
	uint64_t g = fb.hdr[s].generation;

	if (fb.st.trying != g && fb.st.tries >= FB_MAX_TRIES && fb.st.good != fb.st.trying) {
		fb.st.bad = fb.st.trying;               /* the one being left: remembered as given up on */
	}
	if (fb.st.trying == g) {
		fb.st.tries++;
	} else {
		fb.st.trying = g;
		fb.st.tries = 1;
	}
	fb.st.boots++;
	fb.st.flags &= ~FB_LAST_ANDROID;
	if (fb_state_write()) {
		print("flash: start %d of generation %d counted (it is given up on after %d without coming up)\n",
		    (uint64_t)fb.st.tries, g, (uint64_t)FB_MAX_TRIES);
	}
}

/*
 * The slot the countdown takes: the newest the guard allows (the older first when held), -1 when the
 * guard allows none -- every installed generation has failed to come up.
 */
static int
fb_auto_slot(void)
{
	int first = fb.newest, second = fb.older;

	if ((fb.st.flags & FB_HOLD) && second >= 0) {
		first = fb.older;
		second = fb.newest;
	}
	for (int k = 0; k < 2; k++) {
		int s = k == 0 ? first : second;
		if (s < 0 || !fb.ok[s]) {               /* none there, or refused at load (its SHA-256) */
			continue;
		}
		if (fb_guard_allows(s)) {
			return s;
		}
		print("flash: generation %d did not come up %d times: skipped\n", fb.hdr[s].generation,
		    (uint64_t)FB_MAX_TRIES);
	}
	return -1;
}

/*
 * Put slot s's bundle in the payload window and check it. Returns 1 with the container at PAYLOAD_BASE,
 * 0 when it cannot be had (a read error, contents that do not match their hash).
 */
static int
fb_load_slot(int s)
{
	const struct fb_header *h = &fb.hdr[s];
	uint64_t addr = PAYLOAD_BASE;
	uint8_t sum[32];

	/* the payload window: reserved in the phone's memory map, ordinary RAM in QEMU -- claim it either way */
	if (EFI_ERROR(BS->allocate_pages(2 /* AllocateAddress */, 2 /* EfiLoaderData */, PAYLOAD_MAX_SIZE >> 12, &addr))) {
		print("flash: the payload window is the firmware's (reserved): reading into it as on a USB boot\n");
	}
	if (!fb_read(fb.bio, FB_AREA_FIRST + s * FB_SLOT_BLOCKS + 1, (void *)PAYLOAD_BASE, h->size)) {
		print("flash: boot slot %d: read error\n", (uint64_t)s);
		return 0;
	}
	sha256((const void *)PAYLOAD_BASE, h->size, sum);
	if (memcmp(sum, h->sha256, 32) != 0) {
		print("flash: boot slot %d: contents do not match their SHA-256\n", (uint64_t)s);
		return 0;
	}
	clean_dcache(PAYLOAD_BASE, PAYLOAD_BASE + h->size);
	print("flash: booting slot %d, generation %d \"%s\" (SHA-256 checked%s)\n", (uint64_t)s, h->generation,
	    h->label, OWNER_KEY_PINNED ? ", signature checked" : "");
	return 1;
}

/* ---- the date: the PMIC's clock and an offset kept in the boot area ------- */

/*
 * The UEFI here has no clock with the date, and Android never set the PMIC's (it counts from wherever it
 * started). So the date is that count plus an offset, kept in darwin's block FB_CLOCK_BLOCK (in the boot
 * area, past the boot state and a file-backed darwin's marker): "HKCLOCK1", the offset (seconds, signed),
 * the PMIC count it was set at, and an FNV-1a 64 over those 24 bytes. xl settime writes it from the
 * computer's clock; every boot after that has the date, the phone off in between or not.
 */
#define FB_CLOCK_BLOCK    (FB_STATE_BLOCK + 2)
static int fb_write(efi_block_io *bio, uint64_t blk, const void *src, uint64_t bytes);
static const char fb_clock_magic[8] = { 'H', 'K', 'C', 'L', 'O', 'C', 'K', '1' };

struct fb_clock {
	char magic[8];
	int64_t offset;
	uint64_t at_rtc;
	uint64_t sum;
};

static uint64_t
clock_epoch(void)
{
	efi_block_io *bio = fb.bio != NULL ? fb.bio : fb_find_darwin();
	uint64_t rtc = pmic_rtc_seconds();
	struct fb_clock c;

	if (bio == NULL || rtc == 0 || !fb_read(bio, FB_CLOCK_BLOCK, ufs_buffer, FB_BLOCK)) {
		return 0;
	}
	memcpy(&c, ufs_buffer, sizeof(c));
	if (memcmp(c.magic, fb_clock_magic, 8) != 0 || fb_fnv((const uint8_t *)&c, 24, 0xcbf29ce484222325ULL) != c.sum ||
	    rtc < c.at_rtc) {
		return 0;                               /* never set, torn, or the PMIC's clock was reset since */
	}
	print("clock: %d by the PMIC's clock and the kept offset\n", (uint64_t)((int64_t)rtc + c.offset));
	return (uint64_t)((int64_t)rtc + c.offset);
}

static const char *
clock_set(uint64_t now)
{
	efi_block_io *bio = fb.bio != NULL ? fb.bio : fb_find_darwin();
	uint64_t rtc = pmic_rtc_seconds();
	struct fb_clock c;

	if (now < 1704067200ULL || now > 4102444800ULL) {
		return "not a date between 2024 and 2100";
	}
	if (bio == NULL || rtc == 0) {
		return "no darwin, or no PMIC clock";
	}
	memset(&c, 0, sizeof(c));
	memcpy(c.magic, fb_clock_magic, 8);
	c.offset = (int64_t)now - (int64_t)rtc;
	c.at_rtc = rtc;
	c.sum = fb_fnv((const uint8_t *)&c, 24, 0xcbf29ce484222325ULL);
	memset(ufs_buffer, 0, FB_BLOCK);
	memcpy(ufs_buffer, &c, sizeof(c));
	if (!fb_write(bio, FB_CLOCK_BLOCK, ufs_buffer, FB_BLOCK) || !fb_read(bio, FB_CLOCK_BLOCK, ufs_buffer, FB_BLOCK) ||
	    memcmp(ufs_buffer, &c, sizeof(c)) != 0) {
		return "the clock block did not read back";
	}
	print("clock: set: %d now, the PMIC's clock at %d\n", now, rtc);
	return NULL;
}

/* ---- install over USB: a bundle into the boot area (xl install) ----------- */

/* write `bytes` (whole 4 KiB blocks) at darwin's block `blk`, flushed */
static int
fb_write(efi_block_io *bio, uint64_t blk, const void *src, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, lba = blk * FB_BLOCK / bs, done = 0;
	const uint64_t chunk = 4ULL << 20;

	while (done < bytes) {
		uint64_t n = bytes - done < chunk ? bytes - done : chunk;
		if (EFI_ERROR(bio->write_blocks(bio, bio->media->media_id, lba + done / bs, n, (void *)((const uint8_t *)src + done)))) {
			return 0;
		}
		done += n;
	}
	if (bio->flush_blocks) {
		bio->flush_blocks(bio);
	}
	return 1;
}

/*
 * A bundle from mkbundle.py (header block, container), downloaded over USB, into the slot that does not
 * hold the newest good bundle -- the header zeroed first, the container, the header last, each read back --
 * as flashctl.py write-bundle does from Android: a phone without one (the OnePlus 8) is installed and
 * updated this way, from developer mode. Nothing but that slot of darwin's boot area is written; an
 * install cut off loses itself, the other slot still boots. NULL when done, else why not.
 */
static const char *
fb_install(const uint8_t *data, uint64_t len)
{
	const struct fb_header *h = (const struct fb_header *)data;
	uint8_t sum[32];
	uint64_t first;
	int slot;

	if (len < 2 * FB_BLOCK || len % FB_BLOCK != 0 || len > FB_SLOT_BLOCKS * FB_BLOCK || !fb_header_sound(h) ||
	    h->size > len - FB_BLOCK) {
		return "not a bundle from mkbundle.py";
	}
	sha256(data + FB_BLOCK, h->size, sum);
	if (memcmp(sum, h->sha256, 32) != 0) {
		return "the container does not match its SHA-256";
	}
	if (!fb_scan()) {
		return "no darwin on this device";
	}
	slot = fb.newest < 0 ? 0 : 1 - fb.newest;
	if (!fb_header_trusted(slot, h)) {
		return "refused: not signed by the pinned owner key";
	}
	if (fb.newest >= 0 && h->generation <= fb.hdr[fb.newest].generation) {
		return "not newer than the installed generation";
	}
	first = FB_AREA_FIRST + (uint64_t)slot * FB_SLOT_BLOCKS;
	print("flash: installing generation %d \"%s\" into boot slot %d (blocks %d..%d)\n", h->generation, h->label,
	    (uint64_t)slot, first, first + len / FB_BLOCK - 1);
	memset(ufs_buffer, 0, FB_BLOCK);
	if (!fb_write(fb.bio, first, ufs_buffer, FB_BLOCK) || !fb_write(fb.bio, first + 1, data + FB_BLOCK, len - FB_BLOCK)) {
		return "write error (the other slot is untouched)";
	}
	for (uint64_t at = FB_BLOCK; at < len; at += sizeof(ufs_buffer)) {
		uint64_t n = len - at < sizeof(ufs_buffer) ? len - at : sizeof(ufs_buffer);
		if (!fb_read(fb.bio, first + at / FB_BLOCK, ufs_buffer, n) || memcmp(ufs_buffer, data + at, n) != 0) {
			return "read back differs (the other slot is untouched)";
		}
	}
	if (!fb_write(fb.bio, first, data, FB_BLOCK) || !fb_read(fb.bio, first, ufs_buffer, FB_BLOCK) ||
	    memcmp(ufs_buffer, data, FB_BLOCK) != 0) {
		return "the header did not read back (the other slot is untouched)";
	}
	print("flash: boot slot %d written and read back\n", (uint64_t)slot);
	fb_scan();
	return NULL;
}

/*
 * A boot image into boot_a or boot_b, from the loader -- the stock bootloader's fastboot does the same, but
 * reaching it means a reset, after which it does not come back on USB with the cable in. Only a boot image
 * ("ANDROID!") that fits; only when the partition's SHA-256 now begins with want_hex (16 digits: the host
 * says what it expects to overwrite -- the backup, or the image when putting the backup back); written,
 * flushed and read back. Nothing else is written. NULL when done, else why not.
 */
static const char *
fb_flash_boot(const char *name, const char *want_hex, const uint8_t *img, uint64_t len)
{
	efi_block_io *bio = part_bio(name);
	struct sha256 h;
	uint8_t sum[32];
	char hex[17];
	uint64_t bs, size;

	if (bio == NULL) {
		return "no such partition";
	}
	bs = bio->media->block_size;
	size = (bio->media->last_block + 1) * bs;
	if (len < 4096 || memcmp(img, "ANDROID!", 8) != 0 || len > size) {
		return "not a boot image that fits";
	}
	sha256_init(&h);
	for (uint64_t at = 0; at < size; at += sizeof(ufs_buffer)) {
		uint64_t n = size - at < sizeof(ufs_buffer) ? size - at : sizeof(ufs_buffer);
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, at / bs, n, ufs_buffer))) {
			return "read error";
		}
		sha256_add(&h, ufs_buffer, n);
	}
	sha256_end(&h, sum);
	for (int i = 0; i < 8; i++) {
		hex[2 * i] = "0123456789abcdef"[sum[i] >> 4];
		hex[2 * i + 1] = "0123456789abcdef"[sum[i] & 15];
	}
	hex[16] = 0;
	print("flash: %s now: sha256 %s...\n", name, hex);
	if (str_len(want_hex) < 16 || memcmp(hex, want_hex, 16) != 0) {
		return "the partition is not what the host expects: nothing written";
	}
	memset((uint8_t *)img + len, 0, (len + bs - 1) / bs * bs - len);    /* the last block's tail: zeros, not an old payload's */
	len = (len + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->write_blocks(bio, bio->media->media_id, 0, len, (void *)img))) {
		return "write error";
	}
	if (bio->flush_blocks) {
		bio->flush_blocks(bio);
	}
	for (uint64_t at = 0; at < len; at += sizeof(ufs_buffer)) {
		uint64_t n = len - at < sizeof(ufs_buffer) ? len - at : sizeof(ufs_buffer);
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, at / bs, n, ufs_buffer)) || memcmp(ufs_buffer, img + at, n) != 0) {
			return "READ BACK DIFFERS: put the backup back";
		}
	}
	print("flash: %s written (%d bytes) and read back\n", name, len);
	return NULL;
}

/* ---- A/B: back to Android ------------------------------------------------ */

#define AB_ACTIVE 0x04                          /* in the A/B byte (attribute bits 48..55): the slot is active */

struct ab_gpt {
	efi_block_io *bio;
	uint64_t bs, entries_lba, alt_lba, count, esize, bytes, read;
	int a, b;                               /* entry indices of boot_a, boot_b */
};

static int
ab_entry_is(const uint8_t *e, const char *want)
{
	for (int i = 0; i < 36; i++) {
		uint16_t c = e[56 + 2 * i] | e[57 + 2 * i] << 8;
		if (c != (uint8_t)want[i]) {
			return 0;
		}
		if (c == 0) {
			return 1;
		}
	}
	return 0;
}

/* read and check one GPT (header at `lba`) into ufs_buffer; returns 1 and fills g on a sound table */
static int
ab_read_gpt(struct ab_gpt *g, uint64_t lba, uint8_t *header)
{
	uint32_t hsize, hcrc;

	if (EFI_ERROR(g->bio->read_blocks(g->bio, g->bio->media->media_id, lba, g->bs, ufs_buffer)) ||
	    memcmp(ufs_buffer, "EFI PART", 8) != 0) {
		return 0;
	}
	memcpy(header, ufs_buffer, 512);
	hsize = *(uint32_t *)(header + 12); hcrc = *(uint32_t *)(header + 16);
	if (hsize < 92 || hsize > 512) {
		return 0;
	}
	*(uint32_t *)(header + 16) = 0;
	if (ufs_crc32(header, hsize) != hcrc) {
		return 0;
	}
	*(uint32_t *)(header + 16) = hcrc;
	g->entries_lba = *(uint64_t *)(header + 72);
	g->alt_lba = *(uint64_t *)(header + 32);
	g->count = *(uint32_t *)(header + 80);
	g->esize = *(uint32_t *)(header + 84);
	g->bytes = g->count * g->esize;
	g->read = (g->bytes + g->bs - 1) / g->bs * g->bs;
	if (g->esize < 128 || g->bytes == 0 || g->read > sizeof(ufs_buffer) / 2 ||
	    EFI_ERROR(g->bio->read_blocks(g->bio, g->bio->media->media_id, g->entries_lba, g->read, ufs_buffer)) ||
	    ufs_crc32(ufs_buffer, g->bytes) != *(uint32_t *)(header + 88)) {
		return 0;
	}
	return 1;
}

/* find the unit whose GPT holds boot_a and boot_b; the primary table is left in ufs_buffer */
static int
ab_find(struct ab_gpt *g, uint8_t *header)
{
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		return 0;
	}
	for (uint64_t i = 0; i < count; i++) {
		memset(g, 0, sizeof(*g));
		if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&g->bio)) || g->bio->media->logical) {
			continue;
		}
		g->bs = g->bio->media->block_size;
		if (g->bs < 512 || !ab_read_gpt(g, 1, header)) {
			continue;
		}
		g->a = g->b = -1;
		for (uint64_t k = 0; k < g->count; k++) {
			const uint8_t *e = ufs_buffer + k * g->esize;
			if (ab_entry_is(e, "boot_a")) g->a = g->a == -1 ? (int)k : -2;
			if (ab_entry_is(e, "boot_b")) g->b = g->b == -1 ? (int)k : -2;
		}
		if (g->a >= 0 && g->b >= 0) {
			return 1;
		}
	}
	return 0;
}

static uint8_t ab_primary[512], ab_backup[512];

/*
 * Does entry k's partition hold HackiOS's own boot image -- the UEFI with this loader, which mkxnuimg.py
 * makes with board name "HACKiOS" (an Android boot image of header version 0..2 has its name at byte 48;
 * one of version 3 or 4 has no name there at all)? Read through the same unit as the GPT.
 */
static int
ab_is_ours(const struct ab_gpt *g, const uint8_t *table, int k)
{
	static uint8_t head[4096] __attribute__((aligned(4096)));
	uint64_t first = *(const uint64_t *)(table + k * g->esize + 32);
	static const char want[8] = { 'H', 'A', 'C', 'K', 'i', 'O', 'S', 0 };

	if (g->bs > sizeof(head) || EFI_ERROR(g->bio->read_blocks(g->bio, g->bio->media->media_id, first, g->bs, head))) {
		return 0;
	}
	return memcmp(head, "ANDROID!", 8) == 0 && *(const uint32_t *)(head + 40) <= 2 && memcmp(head + 48, want, 8) == 0;
}

/*
 * Make the other slot active: exchange the A/B bytes of boot_a and boot_b, in the backup GPT and then
 * the primary one. Returns 1 when both tables were written and read back right.
 */
static int
ab_switch_back(void)
{
	struct ab_gpt g, gb;
	uint8_t *tab = ufs_buffer, *tab2 = ufs_buffer + sizeof(ufs_buffer) / 2;
	uint8_t ba, bb;

	if (!ab_find(&g, ab_primary)) {
		print("ab: no unit with boot_a and boot_b in a sound GPT: switch with `fastboot --set-active` instead\n");
		return 0;
	}
	ba = tab[g.a * g.esize + 54]; bb = tab[g.b * g.esize + 54];
	print("ab: boot_a A/B byte %x, boot_b %x\n", (uint64_t)ba, (uint64_t)bb);
	if (((ba & AB_ACTIVE) != 0) == ((bb & AB_ACTIVE) != 0)) {
		print("ab: not exactly one slot is active: nothing written\n");
		return 0;
	}
	/*
	 * Only the slot that holds HackiOS's image is given away. Started any other way -- `fastboot boot` of the
	 * same image, with Android's slot the active one -- exchanging the bytes would make the wrong slot active;
	 * then Android's slot already is, and a reset is all it takes.
	 */
	{
		int ours_a = ab_is_ours(&g, tab, g.a), ours_b = ab_is_ours(&g, tab, g.b);
		int ours_active = (ours_a && (ba & AB_ACTIVE)) || (ours_b && (bb & AB_ACTIVE));

		if (ours_a == ours_b) {
			print("ab: %s HackiOS's image: nothing written (`fastboot --set-active` instead)\n",
			    ours_a ? "both boot partitions hold" : "neither boot partition holds");
			return 0;
		}
		if (!ours_active) {
			print("ab: Android's slot is the active one already (HackiOS was not started from its own slot): "
			    "nothing written, a reset starts Android\n");
			return 1;
		}
	}
	memcpy(tab2, tab, g.read);                       /* the primary table, kept while the backup is read */
	gb = g;
	if (!ab_read_gpt(&gb, g.alt_lba, ab_backup) || gb.bytes != g.bytes || memcmp(ufs_buffer, tab2, g.bytes) != 0) {
		print("ab: the backup GPT is missing or differs from the primary one: nothing written\n");
		return 0;
	}
	/* the new table: the two bytes exchanged, nothing else */
	tab2[g.a * g.esize + 54] = bb; tab2[g.b * g.esize + 54] = ba;
	uint32_t crc = ufs_crc32(tab2, g.bytes);
	for (int pass = 0; pass < 2; pass++) {               /* 0: backup, 1: primary */
		uint8_t *h = pass ? ab_primary : ab_backup;
		uint64_t hlba = pass ? 1 : g.alt_lba, elba = *(uint64_t *)(h + 72);
		uint32_t hsize = *(uint32_t *)(h + 12);
		*(uint32_t *)(h + 88) = crc;
		*(uint32_t *)(h + 16) = 0;
		*(uint32_t *)(h + 16) = ufs_crc32(h, hsize);
		memset(ufs_buffer, 0, g.bs);
		memcpy(ufs_buffer, h, 512);
		if (EFI_ERROR(g.bio->write_blocks(g.bio, g.bio->media->media_id, elba, g.read, tab2)) ||
		    EFI_ERROR(g.bio->write_blocks(g.bio, g.bio->media->media_id, hlba, g.bs, ufs_buffer))) {
			print("ab: write of the %s GPT failed\n", pass ? "primary" : "backup");
			return 0;
		}
	}
	if (g.bio->flush_blocks) {
		g.bio->flush_blocks(g.bio);
	}
	/* read both back: sound, and the bytes where they should be */
	struct ab_gpt chk = g;
	uint8_t hb[512];
	if (!ab_read_gpt(&chk, 1, hb) || ufs_buffer[g.a * g.esize + 54] != bb || ufs_buffer[g.b * g.esize + 54] != ba ||
	    !ab_read_gpt(&chk, g.alt_lba, hb) || ufs_buffer[g.a * g.esize + 54] != bb || ufs_buffer[g.b * g.esize + 54] != ba) {
		print("ab: read back does not match: the phone may need `fastboot --set-active=b`\n");
		return 0;
	}
	print("ab: slots exchanged: boot_a %x, boot_b %x\n", (uint64_t)bb, (uint64_t)ba);
	return 1;
}

/* is there an A/B pair to go back to (without writing anything) */
static int
ab_available(void)
{
	struct ab_gpt g;
	return ab_find(&g, ab_primary);
}

/* ---- the menu ------------------------------------------------------------ */

enum { MENU_HACKIOS, MENU_PREVIOUS, MENU_ANDROID, MENU_DEVELOPER, MENU_RECOVERY, MENU_BOOTLOADER, MENU_POWER_OFF,
	MENU_ITEMS };
static const char *const menu_names[MENU_ITEMS] = { "HackiOS", "HackiOS, previous version", "Android",
	"Developer mode (USB, xl)", "Recovery (TWRP)", "Bootloader (fastboot)", "Power off" };

static const char *
menu_detail(int i)
{
	if (i == MENU_HACKIOS && fb.newest >= 0) return fb.hdr[fb.newest].label;
	if (i == MENU_PREVIOUS && fb.older >= 0) return fb.hdr[fb.older].label;
	return "";
}

static void
menu_draw(int sel, int have[MENU_ITEMS], int secs)
{
	print("\n  HACKiOS boot menu        volume: move   power: select\n");
	for (int i = 0; i < MENU_ITEMS; i++) {
		if (!have[i]) {
			continue;
		}
		print("   %s %s%s%s%s\n", i == sel ? ">" : " ", menu_names[i], *menu_detail(i) ? "  (" : "",
		    menu_detail(i), *menu_detail(i) ? ")" : "");
	}
	if (secs >= 0) {
		print("  %s in %d s\n", menu_names[sel], (uint64_t)secs);
	}
}

/*
 * The choice: volume up/down move, any other key (power) takes the highlighted entry; with no key for
 * FB_MENU_SECONDS `dflt` is taken. Returns a MENU_* value; *by_hand says whether a key chose it.
 */
static int
fb_menu(int have[MENU_ITEMS], int dflt, int *by_hand)
{
	efi_simple_text_in *in = (efi_simple_text_in *)ST->con_in;
	int sel = dflt, touched = 0;
	uint64_t ticks = 0;

	*by_hand = 0;
	menu_draw(sel, have, FB_MENU_SECONDS);
	flush_log();
	for (;;) {
		efi_input_key key;
		if (in != NULL && !EFI_ERROR(in->read_key_stroke(in, &key))) {
			if (key.scan == 1 || key.scan == 2) {               /* up, down */
				touched = 1;
				do {
					sel = (sel + (key.scan == 2 ? 1 : MENU_ITEMS - 1)) % MENU_ITEMS;
				} while (!have[sel]);
				menu_draw(sel, have, -1);
			} else {
				*by_hand = 1;
				return sel;
			}
		}
		BS->stall(50000);
		if (!touched && ++ticks % 20 == 0) {
			int left = FB_MENU_SECONDS - (int)(ticks / 20);
			if (left <= 0) {
				return sel;
			}
			print("  %s in %d s\n", menu_names[sel], (uint64_t)left);
		}
	}
}

/*
 * Off-mode charging (the OnePlus 8). Switched off with the cable in, the PMIC starts the phone again by the cable
 * (docs/INSTALL.md: startupmode=dc_charger), and the countdown would start HackiOS: a phone that cannot be switched
 * off on its charger. An iPhone stays off and charges. So a switch-off with the cable in leaves FB_OFF_CHARGING in
 * the boot state, and the next start -- by the cable, not the power key -- waits here, the screen dark: the PMIC
 * charges by itself (its own JEITA limits), at the wall charger's input limit (charger_icl_raise); the power key
 * starts the menu, as it would start an iPhone; the cable out switches the phone off. A cable put into a phone that
 * was switched off without one still starts it (as an iPhone does). The flag counts once: it is cleared here.
 */
#define FB_OFF_SHOW_SECONDS 5

static void
fb_power_off(void)
{
	if (cable_in() == 1 && fb.bio != NULL) {
		hc_take_back("before the boot state");          /* (the kernel's transfer list may still be in) */
		fb.st.flags |= FB_OFF_CHARGING;
		fb_state_write();
	}
	power_off();
}

static void
fb_offmode_show(void)
{
	static const char *const state[8] = { "inhibited", "trickle", "pre-charge", "charging", "charging (taper)",
		"charged", "paused", "disabled" };
	int ch = spmi_channel(0x210, 0);
	uint8_t st = 0;

	print("\n  Switched off, on the charger: %s.\n  Power key: switch on.\n",
	    ch >= 0 && pon_reg(ch, 0x06, 0, &st) ? state[st & 7] : "state not known");
	flush_log();
}

static void
fb_offmode(void)
{
	efi_simple_text_in *in = (efi_simple_text_in *)ST->con_in;
	uint64_t cntkctl = READ_SYSREG(CNTKCTL_EL1), shown = READ_SYSREG(CNTVCT_EL0), tick = READ_SYSREG(CNTFRQ_EL0) / 4;
	int dark = 0;

	if (!(fb.st.flags & FB_OFF_CHARGING)) {
		return;
	}
	fb.st.flags &= ~FB_OFF_CHARGING;
	fb_state_write();
	if (pon_reason == 0xffffffffu || (pon_reason & PON_REASON_KPDPWR) || !(pon_reason & PON_REASON_CHARGER) ||
	    cable_in() != 1) {
		print("flash: switched off on the charger last time; this start is not the cable's (%x): the menu\n",
		    (uint64_t)pon_reason);
		return;
	}
	print("flash: off-mode charging (started by the cable, %x)\n", (uint64_t)pon_reason);
	charger_icl_raise();
	fb_offmode_show();
	/* WFE with the timer's event stream (as run_on_other_cpu): a look at the keys and the cable every 250 ms */
	__asm__ volatile ("msr CNTKCTL_EL1, %0\nisb" : : "r" ((cntkctl & ~0xfcULL) | 1ULL << 2 | 10ULL << 4));
	for (;;) {
		efi_input_key key;

		for (uint64_t t = READ_SYSREG(CNTVCT_EL0); READ_SYSREG(CNTVCT_EL0) - t < tick;) {
			__asm__ volatile ("dsb sy\n\twfe" ::: "memory");
		}
		if (cable_in() == 0) {
			print("flash: the cable is out: switching off\n");
			power_off();
		}
		if (in != NULL && !EFI_ERROR(in->read_key_stroke(in, &key))) {
			if (key.scan != 1 && key.scan != 2) {          /* not volume: the power key */
				break;
			}
			dark = 0;                               /* volume: the state again */
			shown = READ_SYSREG(CNTVCT_EL0);
			fb_offmode_show();
		}
		if (!dark && seconds_since(shown) >= FB_OFF_SHOW_SECONDS && SCREEN.base != 0) {
			memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			dark = 1;
		}
	}
	__asm__ volatile ("msr CNTKCTL_EL1, %0\nisb" : : "r" (cntkctl));
	charger_icl_restore();                          /* the kernel sets its own (instantnoodle_battery.c) */
	print("flash: the power key: switching on\n");
}

/* run slot s; returns only when it could not be loaded */
static void
fb_boot(int s)
{
	if (fb_load_slot(s)) {
		struct payload_info pi;
		int timed = 0;

		/* a test bundle (loader_return_after= on its command line): after it, the USB link, not a reset */
		parse_payload(&pi, 0);
		for (const char *c = pi.cmdline; *c; c++) {
			timed |= starts_with(c, "loader_return_after=") && (c == pi.cmdline || c[-1] == ' ');
		}
		fb_count_start(s);
		flush_log();
		run_kernel(0, 0);                       /* until the phone is switched off; if it comes back, reset */
		if (loader_ctx[CTX_REASON] == 3) {
			fb_power_off();                 /* iOS shut down (halt_all_cpus): off, not round again */
		}
		if (timed && (USB != NULL || usb_start())) {          /* (USB: started over it, xl flashboot) */
			/* the host fetches the run's log (fastboot get_staged) and the boot state (xl health) */
			usb_command_loop();
		}
		reboot();
	}
	fb.ok[s] = 0;
}

/* The boot of an installed device: the menu, then what was chosen. Does not return. */
static void
fb_standalone(void)
{
	int have[MENU_ITEMS] = { 0, 0, 0, 1 };

	fb_scan();
	fb_offmode();
	have[MENU_ANDROID] = !PLAT->no_android && ab_available();
	/* the phone's own modes, by the reason in its PMIC (reboot_to()): never the countdown's choice */
	have[MENU_RECOVERY] = have[MENU_BOOTLOADER] = have[MENU_POWER_OFF] = PLAT == &platform_instantnoodle;
	for (;;) {
		int auto_slot = fb.bio ? fb_auto_slot() : -1, by_hand, choice, dflt;

		have[MENU_HACKIOS] = fb.newest >= 0 && fb.ok[fb.newest];
		have[MENU_PREVIOUS] = fb.older >= 0 && fb.ok[fb.older];
		if (auto_slot >= 0 && (fb.st.flags & FB_LAST_ANDROID) && have[MENU_ANDROID]) {
			/* like a PC's boot menu: what was chosen last time is what the countdown takes */
			dflt = MENU_ANDROID;
		} else if (auto_slot >= 0) {
			dflt = auto_slot == fb.newest ? MENU_HACKIOS : MENU_PREVIOUS;
		} else if (have[MENU_HACKIOS]) {
			/* installed, and nothing installed comes up: the countdown goes back to Android -- or, on a
			 * phone with none, waits on the USB link for a computer (the keys still start HackiOS) */
			dflt = have[MENU_ANDROID] ? MENU_ANDROID : have[MENU_DEVELOPER] ? MENU_DEVELOPER : MENU_HACKIOS;
			print("flash: no installed HackiOS has come up in %d tries: %s is the default\n",
			    (uint64_t)FB_MAX_TRIES, dflt == MENU_ANDROID ? "Android" : dflt == MENU_DEVELOPER ? "developer mode" : "HackiOS");
		} else {
			dflt = have[MENU_HACKIOS] ? MENU_HACKIOS : have[MENU_ANDROID] ? MENU_ANDROID : MENU_DEVELOPER;
		}
		choice = fb_menu(have, dflt, &by_hand);
		print("menu: %s%s\n", menu_names[choice], by_hand ? "" : " (countdown)");
		if (choice == MENU_HACKIOS || choice == MENU_PREVIOUS) {
			fb_boot(choice == MENU_HACKIOS ? fb.newest : fb.older);
		} else if (choice == MENU_ANDROID) {
			if (fb.bio != NULL && !(fb.st.flags & FB_LAST_ANDROID)) {
				fb.st.flags |= FB_LAST_ANDROID;         /* (before the switch: a write to darwin after it is never needed) */
				fb_state_write();
			}
			if (ab_switch_back()) {
				reboot();
			}
			have[MENU_ANDROID] = 0;
		} else if (choice == MENU_RECOVERY) {
			reboot_to(1);
		} else if (choice == MENU_BOOTLOADER) {
			reboot_to_fastboot();
		} else if (choice == MENU_POWER_OFF) {
			fb_power_off();
		} else {
			if (usb_start()) {
				flush_log();
				usb_command_loop();
			}
			print("menu: no USB link here\n");
			have[MENU_DEVELOPER] = 0;
			if (!have[MENU_HACKIOS] && !have[MENU_PREVIOUS] && !have[MENU_ANDROID]) {
				print("menu: nothing left to start: resetting\n");
				reboot();
			}
		}
	}
}
