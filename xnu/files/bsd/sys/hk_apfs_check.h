/*
 * Mount the APFS volume on /dev/rvblk0 and walk it, to compare with what the host reader
 * sees in the same image.  Only with boot-arg apfs_check.
 * See bsd/dev/hk_apfs_check.c.
 */
#ifndef _SYS_HK_APFS_CHECK_H_
#define _SYS_HK_APFS_CHECK_H_

void hk_apfs_check_init(void);

#endif /* _SYS_HK_APFS_CHECK_H_ */
