/*
 * Compile Apple's HFS kext sources inside the kernel the way the kext is built: with the kext (KPI) view
 * of the kernel headers, and with HFS's own idea of its features rather than the kernel's -DCONFIG_*
 * options, which a kext build does not see. This is HFS's iOS build (core/kext-config.h, iOS branch:
 * no quotas, secluded rename) without content protection, for there is no AppleKeyStore here; and as on
 * iOS no text-encoding converters (hfs_encodings is a macOS kext: names are MacRoman, hfs_catalog.c).
 * kext-config.h itself is kept out (its guard defined), so that it cannot turn protection back on.
 * (After ios-instantnoodle's 0026 patch, which built the macOS variant.)
 */
#ifndef HK_HFS_COMPAT_H
#define HK_HFS_COMPAT_H
#undef BSD_KERNEL_PRIVATE
#undef XNU_KERNEL_PRIVATE
#undef CONFIG_PROTECT
#undef CONFIG_SEARCHFS
#undef CONFIG_HFS_DIRLINK
#undef CONFIG_SECLUDED_RENAME
#undef CONFIG_MACF
#undef CONFIG_HFS_RBTREE
#undef QUOTA
#undef NAMEDSTREAMS
#undef FIFO
#undef HFS_COMPRESSION

#include <TargetConditionals.h>
#define _hfs_config_            /* core/kext-config.h: decided here instead */
#define HFS_COMPRESSION 1
#define FIFO 1
#define QUOTA 0
#define CONFIG_SECLUDED_RENAME 1

/* built in, not a loadable kext (hk_hfs_iokit.c) */
#define OSKextGetCurrentLoadTag hk_hfs_current_load_tag
#endif
