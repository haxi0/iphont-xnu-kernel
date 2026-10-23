/* ------------------------------------------------------------------------ */
/* darwin as a file of userdata                                             */
/*
 * Included by loader.c. On a phone whose darwin is not a partition of its own but a file in userdata
 * (common/flash/darwinid.py, kind "file"): a file f2fs never moves -- pinned and allocated in full, as
 * Android keeps its swap files and Dynamic System Update images -- whose blocks lie where the map in
 * darwin_identity.h says (FIEMAP, read when the file was made). Out of that map this makes a block device
 * of darwin's own 4 KB blocks, an efi_block_io like any other: the boot menu (flashboot.c), the block
 * service for the kernel (block_start, block_poll) and everything after them read and write darwin through
 * it and never know it is not a partition. Nothing of the partition table or of the filesystem changes.
 *
 * What keeps a write from landing in Android's data:
 *   - every request is cut along the map; a block outside darwin is refused, never wrapped or clamped;
 *   - every extent is checked to lie inside the host partition before anything is read;
 *   - the device exists only once the marker block in darwin (darwinid.py mark, MARK_BLOCK) has been read
 *     through the map and says this map: the file's random mark, its size, its number of extents and the
 *     sum of the map. A file deleted, moved or made again since the loader was built does not have it --
 *     its blocks now belong to anything -- and then there is no darwin at all: no boot from it, no write.
 */

#if DARWIN_FILE

#define DF_BLOCK        4096ULL
#define DF_MARK_BLOCK   2091521ULL              /* darwinid.py MARK_BLOCK: right after the boot state */

static efi_block_io *df_host;                   /* the partition the file is in (userdata) */
static efi_block_io df_bio;
static efi_block_media df_media;
static uint64_t df_start[DARWIN_FILE_EXTENTS];  /* darwin's first block in each extent */
static int df_state;                            /* 0 not tried, 1 there, -1 not there */

/*
 * The map as the kernel reads it (bsd/dev/hk_ufs.c, darwin_map_pa= on its command line): with
 * ufs_native=1 the kernel reads the storage through its own driver, LUN by LUN, and darwin -- on no unit of
 * its own -- through this, along the extents of the host partition (part[host] of the block service's
 * table). Written once before the kernel starts; one page, in the loader's memory, which stays.
 */
#define DF_MAP_MAGIC    0x3130504d4444484bULL   /* "HKDDMP01" */
struct df_map {
	uint64_t magic;
	uint32_t count, host;                   /* extents; the host partition's index in the block service's table */
	uint64_t blocks;                        /* darwin's, of 4 KB */
	uint64_t sum;                           /* DARWIN_FILE_SUM: the kernel checks it against the extents below */
	struct { uint64_t at, count; } ext[DARWIN_FILE_EXTENTS];  /* in the host's 4 KB blocks, in darwin's order */
};
static struct df_map df_map __attribute__((aligned(0x2000)));     /* (8 KB: as far as this target aligns; the kernel maps the 16 KB pages around it) */

static uint64_t
df_sum(void)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	for (int i = 0; i < DARWIN_FILE_EXTENTS; i++) {
		uint64_t v[2] = { darwin_file_extents[i].at, darwin_file_extents[i].count };
		const uint8_t *p = (const uint8_t *)v;

		for (int k = 0; k < 16; k++) {
			h ^= p[k];
			h *= 0x100000001b3ULL;
		}
	}
	return h;
}

/* darwin's blocks lba.. (size bytes) through the map, one piece of an extent at a time */
static efi_status
df_io(int write, uint64_t lba, uint64_t size, void *buf)
{
	uint64_t hbs = df_host->media->block_size, per = DF_BLOCK / hbs, n = size / DF_BLOCK;
	uint8_t *p = buf;
	int e = DARWIN_FILE_EXTENTS - 1;

	if (size % DF_BLOCK != 0 || lba >= DARWIN_FILE_BLOCKS || n > DARWIN_FILE_BLOCKS - lba) {
		return 0x8000000000000002ULL;           /* EFI_INVALID_PARAMETER: not inside darwin */
	}
	while (n > 0) {
		uint64_t off, take;
		efi_status s;

		/* the extent holding lba (they are few: a step back from the last is quick enough) */
		while (e > 0 && df_start[e] > lba) {
			e--;
		}
		while (e < DARWIN_FILE_EXTENTS - 1 && df_start[e + 1] <= lba) {
			e++;
		}
		off = lba - df_start[e];
		take = darwin_file_extents[e].count - off;
		if (take > n) {
			take = n;
		}
		if (write) {
			s = df_host->write_blocks(df_host, df_host->media->media_id, (darwin_file_extents[e].at + off) * per,
			    take * DF_BLOCK, p);
		} else {
			s = df_host->read_blocks(df_host, df_host->media->media_id, (darwin_file_extents[e].at + off) * per,
			    take * DF_BLOCK, p);
		}
		if (EFI_ERROR(s)) {
			return s;
		}
		lba += take;
		n -= take;
		p += take * DF_BLOCK;
	}
	return 0;
}

static efi_status EFIAPI
df_read(efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buf)
{
	return df_io(0, lba, size, buf);
}

static efi_status EFIAPI
df_write(efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buf)
{
	return df_io(1, lba, size, buf);
}

static efi_status EFIAPI
df_flush(efi_block_io *self)
{
	return df_host->flush_blocks ? df_host->flush_blocks(df_host) : 0;
}

/* darwin's block device, or NULL when the file is not (or no longer) the one the map describes */
static efi_block_io *
darwin_file_bio(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static uint8_t mark[DF_BLOCK] __attribute__((aligned(4096)));
	efi_handle *handles;
	uint64_t count, total = 0, hbs;

	if (df_state != 0) {
		return df_state > 0 ? &df_bio : NULL;
	}
	df_state = -1;
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count && df_host == NULL; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		if (str_eq(name, DARWIN_FILE_HOST)) {
			df_host = bio;
		}
	}
	if (df_host == NULL) {
		print("darwin: no partition %s to hold its file\n", DARWIN_FILE_HOST);
		return NULL;
	}
	hbs = df_host->media->block_size;
	if (hbs == 0 || hbs > DF_BLOCK || DF_BLOCK % hbs != 0 || df_host->media->read_only) {
		print("darwin: %s has blocks of %d bytes%s: not used\n", DARWIN_FILE_HOST, hbs,
		    df_host->media->read_only ? " and is read only" : "");
		return NULL;
	}
	for (int i = 0; i < DARWIN_FILE_EXTENTS; i++) {
		uint64_t end = (darwin_file_extents[i].at + darwin_file_extents[i].count) * (DF_BLOCK / hbs);

		if (darwin_file_extents[i].count == 0 || end > df_host->media->last_block + 1) {
			print("darwin: extent %d of the map is not inside %s: not used\n", (uint64_t)i, DARWIN_FILE_HOST);
			return NULL;
		}
		df_start[i] = total;
		total += darwin_file_extents[i].count;
	}
	if (total != DARWIN_FILE_BLOCKS || df_sum() != DARWIN_FILE_SUM) {
		print("darwin: the map does not add up: not used\n");
		return NULL;
	}
	/* the marker, through the map: the file is still the one mapped, or there is no darwin */
	if (EFI_ERROR(df_io(0, DF_MARK_BLOCK, DF_BLOCK, mark)) || memcmp(mark, "HKDARWIN", 8) != 0 ||
	    *(uint64_t *)(mark + 8) != DARWIN_FILE_MARK || *(uint64_t *)(mark + 16) != DARWIN_FILE_BLOCKS ||
	    *(uint64_t *)(mark + 24) != DARWIN_FILE_EXTENTS || *(uint64_t *)(mark + 32) != DARWIN_FILE_SUM) {
		print("darwin: the file in %s no longer carries its marker (deleted or made again since this loader was "
		    "built): darwin is not used, nothing is written\n", DARWIN_FILE_HOST);
		return NULL;
	}
	df_media.media_id = 0;
	df_media.present = 1;
	df_media.logical = 1;
	df_media.block_size = DF_BLOCK;
	df_media.last_block = DARWIN_FILE_BLOCKS - 1;
	df_bio.revision = 0x00010000;
	df_bio.media = &df_media;
	df_bio.read_blocks = df_read;
	df_bio.write_blocks = df_write;
	df_bio.flush_blocks = df_flush;
	df_state = 1;
	print("darwin: a file of %s, %d blocks in %d extents, marker right\n", DARWIN_FILE_HOST, DARWIN_FILE_BLOCKS,
	    (uint64_t)DARWIN_FILE_EXTENTS);
	return &df_bio;
}

/* fill the kernel's copy of the map (block_start, once the host's index in the table is known) */
static void
darwin_file_publish(uint32_t host)
{
	df_map.magic = DF_MAP_MAGIC;
	df_map.count = DARWIN_FILE_EXTENTS;
	df_map.host = host;
	df_map.blocks = DARWIN_FILE_BLOCKS;
	df_map.sum = DARWIN_FILE_SUM;
	for (int i = 0; i < DARWIN_FILE_EXTENTS; i++) {
		df_map.ext[i].at = darwin_file_extents[i].at;
		df_map.ext[i].count = darwin_file_extents[i].count;
	}
	clean_dcache((uint64_t)(uintptr_t)&df_map, (uint64_t)(uintptr_t)&df_map + sizeof(df_map));
}

#else

static int df_state;
static struct { uint64_t magic; } df_map;
static void darwin_file_publish(uint32_t host) { (void)host; }
#define DARWIN_FILE_BLOCKS 0ULL                 /* (darwin is a partition: block_start never asks) */
#define DARWIN_FILE_HOST ""

static efi_block_io *
darwin_file_bio(void)
{
	return NULL;
}

#endif /* DARWIN_FILE */
