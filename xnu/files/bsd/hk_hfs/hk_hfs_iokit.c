/*
 * Built-in replacement for HFS's IOKit glue (core/hfs_iokit.cpp and hfs_encodings/hfs_encodings_kext.cpp).
 *
 * On Apple's systems HFS is a kext: IOKit matches its IOService on IOResources and start() registers the
 * filesystem. Here HFS is linked into the kernel for one job, an installed phone's data volume (/private/var
 * on the store's data area, userland/src/init.c), so bsd_init calls hk_hfs_register() right after vfsinit().
 * The registration is the kext's start() sequence. It is not a root filesystem here (no VFS_TBLCANMOUNTROOT):
 * the root stays the iOS volume, and mountroot never tries this one.
 *
 * The helpers that ask IOKit about storage keep their answers for a system with no IOStorageFamily and so no
 * IOMedia objects: nothing is ejectable, no medium carries an external journal, the platform expert publishes
 * no serial number. Content protection (AppleKeyStore) is not built, nor (as on iOS) the macOS-only
 * text-encoding converters (hk_hfs_compat.h).
 * After ios-instantnoodle's 0026 patch (sweet_hfs_iokit.c), which ran iOS 16 over this HFS.
 */
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>

#include "hfs.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_iokit.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif

#if DEBUG
bool hfs_corruption_panics = true;
#endif

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
extern struct vfsops hfs_vfsops;

void hk_hfs_register(void);

static vfstable_t hk_hfs_vfs_handle;

void
hk_hfs_register(void)
{
#ifndef panic_on_assert
	panic_on_assert = PE_i_can_has_kernel_configuration() & kPEICanHasAssertions;
#endif

#if DEBUG
	PE_parse_boot_argn("hfs_corruption_panics", &hfs_corruption_panics, sizeof(hfs_corruption_panics));
#endif

	struct vnodeopv_desc *op_descs[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};

#ifndef VFS_TBLVNOP_SECLUDE_RENAME
#define VFS_TBLVNOP_SECLUDE_RENAME 0
#endif

	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(op_descs) / sizeof(op_descs[0]),
		.vfe_opvdescs = op_descs,
		.vfe_fsname = "hfs",
		.vfe_flags = (VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED
		    | VFS_TBL64BITREADY | VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2
		    | VFS_TBLTHREADSAFE | VFS_TBLVNOP_SECLUDE_RENAME | VFS_TBLNATIVEXATTR)
	};

	int ret = vfs_fsadd(&vfe, &hk_hfs_vfs_handle);
	if (ret) {
		printf("hk_hfs: vfs_fsadd failed: %d: no HFS+ this boot\n", ret);
		return;
	}

	hfs_init_zones();
	hfs_sysctl_register();

	uint32_t num_cpus;
	size_t sz = sizeof(num_cpus);

	if (sysctlbyname("hw.physicalcpu", &num_cpus, &sz, NULL, 0) == 0) {
		if ((2 * num_cpus) > MAX_CACHED_ORIGINS_DEFAULT) {
			_hfs_max_origins = 2 * num_cpus;
			_hfs_max_file_origins = 2 * num_cpus;
		} else if ((2 * num_cpus) > MAX_CACHED_FILE_ORIGINS_DEFAULT) {
			_hfs_max_file_origins = 2 * num_cpus;
		}
	}

	printf("hk_hfs: hfs-627.100.6 registered\n");
}

/* built in, not a loadable kext: there is no kext to retain per mount */
OSKextLoadTag
hk_hfs_current_load_tag(void)
{
	return kOSKextInvalidLoadTag;
}

int
hfs_is_ejectable(__unused const char *cdev_name)
{
	return 0;
}

void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg),
    __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(char *serial_number_str, uint32_t len)
{
	if (len < 1) {
		return 0;
	}
	serial_number_str[0] = '\0';
	return KERN_NOT_SUPPORTED;
}
