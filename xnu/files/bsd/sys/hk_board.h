/*
 * The board: where the blocks of the phone's SoC are (bsd/dev/hk_board.c). The drivers of the port were
 * brought up on one phone (the POCO X5 Pro, SM7325), and every address of a block they talk to is that
 * phone's -- unless the kernel's command line names another: board_<block>=0x... A device profile says so
 * (common/devices/<device>.conf, BOARD_ARGS; common/new-device.sh reads them out of the phone's own device
 * tree), so a phone with the same blocks at other places needs no other kernel.
 */
#ifndef _SYS_HK_BOARD_H_
#define _SYS_HK_BOARD_H_

#include <mach/vm_types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the physical address of a block: board_<name>= on the command line, else `dflt` (the brought-up phone's) */
vm_offset_t hk_board_addr(const char *name, vm_offset_t dflt);
/* a number of the board (counts, sizes): board_<name>=, else `dflt` */
uint32_t hk_board_number(const char *name, uint32_t dflt);
/* every board_ address that was given, once, to the log (hk_input.c, at the drivers' start) */
void hk_board_report(void);
/*
 * The application processor's RPMh command slot set 0 (DRV 2, TCS 0), which several drivers write: claimed for one
 * request at a time (bsd/dev/hk_board.c). `wait` false: one try (interrupts off). true when it is ours.
 */
bool hk_rpmh_claim(bool wait);
void hk_rpmh_release(void);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_HK_BOARD_H_ */
