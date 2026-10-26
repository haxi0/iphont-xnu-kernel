/*
 * What ath11k uses of Linux and LinuxKPI's headers (FreeBSD 15.1) do not declare. Small things
 * each; where one needs a function, it is in src/.
 */
#ifndef _LX_MORE_H_
#define _LX_MORE_H_

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/pm.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

/* --- interrupts: how the line is triggered is the kernel's knowledge, not the driver's */
#define IRQF_TRIGGER_RISING	0x0001
#define IRQ_NOAUTOEN		(1 << 12)	/* request_irq() leaves the interrupt disabled */
#define IRQ_DISABLE_UNLAZY	(1 << 19)	/* disable_irq() masks at once: it always does here */
int enable_irq_wake(unsigned int irq);
int disable_irq_wake(unsigned int irq);
/* which processor takes an interrupt is the kernel's choice here (all go to the one thread of hk_lx.c) */
static inline int irq_set_affinity_and_hint(unsigned int irq, const cpumask_t *mask) { return (0); }

/* (no CONFIG_PM_SLEEP: the system's sleep reaches the driver through lx_suspend()/lx_resume(), not its PM ops) */
#ifndef SET_SYSTEM_SLEEP_PM_OPS
#define SET_SYSTEM_SLEEP_PM_OPS(suspend_function, resume_function)
#endif
#ifndef SET_LATE_SYSTEM_SLEEP_PM_OPS
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(suspend_function, resume_function)
#endif

/* the phone is not woken by its radio: nothing here suspends */
static inline bool device_may_wakeup(struct device *dev) { return (false); }

/* --- DMA: memory for the device that need not be coherent, and the barriers around ring indices */
void *dma_alloc_noncoherent(struct device *dev, size_t size, dma_addr_t *handle,
    enum dma_data_direction dir, gfp_t gfp);
void dma_free_noncoherent(struct device *dev, size_t size, void *vaddr, dma_addr_t handle,
    enum dma_data_direction dir);
#define dma_rmb()	rmb()
#define dma_wmb()	wmb()
#define dma_mb()	mb()

/* --- a table of device ids is found through the driver (of_match_table), not through a section */
#undef MODULE_DEVICE_TABLE
#define MODULE_DEVICE_TABLE(bus, table)

/* --- 802.11: radio measurement action frames (IEEE 802.11-2020, 9.6.6) */
#define WLAN_CATEGORY_RADIO_MEASUREMENT			5
#define WLAN_RM_ACTION_LINK_MEASUREMENT_REQUEST		2
#define WLAN_RM_ACTION_LINK_MEASUREMENT_REPORT		3
/* the driver wants to hear of every change of regulatory domain it brought about itself */
#define WIPHY_FLAG_NOTIFY_REGDOM_BY_DRIVER		BIT(24)

#endif
