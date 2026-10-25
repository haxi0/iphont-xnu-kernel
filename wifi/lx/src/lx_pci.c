/*
 * The radio's bus when it is a PCIe device -- the OnePlus 8's QCA6390 -- for ath11k's own pci.c,
 * pcic.c and mhi.c, which are built unmodified: what they ask of Linux's PCI core, its interrupt
 * core, tasklets and NAPI, on the few calls of lx_os.h. The device is the one behind the kernel's
 * host bridge, which has its link up and its memory window placed before the driver is started
 * (bsd/dev/instantnoodle_wifi.c of the kernel); its configuration space is the kernel's to read
 * and write (lx_os_pci_config_*()).
 *
 * Interrupts: the device's MSI vectors are the GIC's SPIs that lx_os_irq_msi() names, one thread
 * of the kernel's runs their functions (hk_lx.c). A Linux handler is called there; what it
 * defers -- a tasklet, a NAPI poll -- runs on one thread of this file's ("wifi-soft"), which is
 * what softirq context is to Linux. Nothing of the driver runs in an interrupt.
 *
 * Power: Linux's PCI core keeps a device's configuration across a suspend for its driver; here the
 * writes the driver made to the first 256 bytes are kept and made again after the kernel has
 * powered the device and brought its link up again (lx_pci_restore()).
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <lx_os.h>
#include "lx.h"

#define LX_IRQ_BASE	1000		/* the Linux interrupt number of the device's vector 0 */
#define LX_VECTORS	32

static struct pci_dev radio;
static struct pci_bus radio_bus;
static struct pci_driver *radio_driver;
static const struct pci_device_id *radio_id;
static bool radio_probed;

/* ---- configuration space ---------------------------------------------------------------------- */

static struct {
	uint8_t bytes[256];
	uint8_t written[256 / 8];	/* a bit per byte the driver has written */
} shadow;

static bool
is_radio(device_t dev)
{
	return ((struct pci_dev *)dev == &radio);
}

uint32_t
lx_pci_read_config(device_t dev, int reg, int width)
{
	return (is_radio(dev) ? lx_os_pci_config_read((unsigned)reg, (unsigned)width) : ~0u);
}

void
lx_pci_write_config(device_t dev, int reg, uint32_t value, int width)
{
	if (!is_radio(dev)) {
		return;
	}
	for (int i = 0; i < width && reg + i < 256; i++) {
		shadow.bytes[reg + i] = (uint8_t)(value >> (8 * i));
		shadow.written[(reg + i) / 8] |= (uint8_t)(1u << ((reg + i) % 8));
	}
	lx_os_pci_config_write((unsigned)reg, value, (unsigned)width);
}

static void
command_set(uint16_t set, uint16_t clear)
{
	uint16_t command = (uint16_t)lx_os_pci_config_read(PCIR_COMMAND, 2);

	lx_pci_write_config((device_t)&radio, PCIR_COMMAND, (command & ~clear) | set, 2);
}

int
lx_pci_enable_io(device_t dev, int space)
{
	if (is_radio(dev) && space == SYS_RES_MEMORY) {
		command_set(PCIM_CMD_MEMEN, 0);
	}
	return (0);
}

int
lx_pci_enable_busmaster(device_t dev)
{
	if (is_radio(dev)) {
		command_set(PCIM_CMD_BUSMASTEREN, 0);
	}
	return (0);
}

int
lx_pci_disable_busmaster(device_t dev)
{
	if (is_radio(dev)) {
		command_set(0, PCIM_CMD_BUSMASTEREN);
	}
	return (0);
}

/* The capability lists: the classic one from 0x34, the extended one from 0x100. 0 and *where, or ENOENT. */
int
lx_pci_find_cap(device_t dev, int capability, int *where)
{
	unsigned at = lx_pci_read_config(dev, PCIR_CAP_PTR, 1) & 0xfc;

	for (unsigned hops = 0; at >= 0x40 && hops < 48; hops++) {
		uint32_t header = lx_pci_read_config(dev, (int)at, 2);

		if ((header & 0xff) == (unsigned)capability) {
			*where = (int)at;
			return (0);
		}
		at = header >> 8 & 0xfc;
	}
	return (ENOENT);
}

int
lx_pci_find_extcap(device_t dev, int capability, int *where)
{
	unsigned at = 0x100;

	for (unsigned hops = 0; at >= 0x100 && hops < 96; hops++) {
		uint32_t header = lx_pci_read_config(dev, (int)at, 4);

		if (header == 0 || header == ~0u) {
			break;
		}
		if ((header & 0xffff) == (unsigned)capability) {
			*where = (int)at;
			return (0);
		}
		at = header >> 20 & 0xffc;
	}
	return (ENOENT);
}

/*
 * After the device's power came back: what the driver had written, written again -- all but the
 * command register and the MSI capability's control word first, those last (the window and the
 * vectors are in place before the device may use them).
 */
static void
lx_pci_restore(void)
{
	unsigned msi_control = radio.msi_cap != 0 ? radio.msi_cap + PCIR_MSI_CTRL : ~0u;

	for (unsigned pass = 0; pass < 2; pass++) {
		for (unsigned at = 0; at < 256; at += 2) {
			bool last = at == PCIR_COMMAND || at == msi_control;
			unsigned mask = (unsigned)shadow.written[at / 8] >> (at % 8) & 3;
			uint32_t value;

			if (mask == 0 || last != (pass == 1)) {
				continue;
			}
			value = lx_os_pci_config_read(at, 2);
			value = (mask & 1 ? shadow.bytes[at] : value & 0xff) | (mask & 2 ? shadow.bytes[at + 1] : value >> 8 & 0xff) << 8;
			lx_os_pci_config_write(at, value, 2);
		}
	}
}

/* ---- the memory window ---------------------------------------------------------------------- */

static void *window_mapped;

unsigned long
pci_resource_start(struct pci_dev *pdev, int bar)
{
	unsigned long long physical = 0;
	unsigned long size = 0;

	return (pdev == &radio && lx_os_pci_bar((unsigned)bar, &physical, &size) ? (unsigned long)physical : 0);
}

unsigned long
pci_resource_len(struct pci_dev *pdev, int bar)
{
	unsigned long long physical = 0;
	unsigned long size = 0;

	return (pdev == &radio && lx_os_pci_bar((unsigned)bar, &physical, &size) ? size : 0);
}

int
linuxkpi_pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	return (pdev == &radio && pci_resource_len(pdev, bar) != 0 ? 0 : -ENODEV);
}

void
linuxkpi_pci_release_region(struct pci_dev *pdev, int bar)
{
}

/* (mapped once, and kept: the kernel's mappings of registers are never taken back) */
void *
linuxkpi_pci_iomap(struct pci_dev *pdev, int bar, unsigned long length)
{
	if (pdev != &radio || bar != 0 || pci_resource_len(pdev, 0) == 0) {
		return (NULL);
	}
	if (window_mapped == NULL) {
		window_mapped = lx_os_map_registers(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
	}
	return (window_mapped);
}

void
linuxkpi_pci_iounmap(struct pci_dev *pdev, void *mapped)
{
}

/* The radio's memory is all below 4 GB as it sees it (lx_base.c's pool): any mask of 32 bits or more will do. */
int
linux_dma_tag_init(struct device *dev, u64 mask)
{
	return (mask >= 0xffffffffull ? 0 : -EIO);
}

int
linux_dma_tag_init_coherent(struct device *dev, u64 mask)
{
	return (linux_dma_tag_init(dev, mask));
}

/* ---- MSI ------------------------------------------------------------------------------------------ */

static struct msi_desc radio_msi;
static unsigned radio_vectors;

/*
 * The device's MSI capability set up for `max` vectors (a power of two, as many as it can take): it
 * writes the GIC's number of vector 0 plus the vector's to the address lx_os_irq_msi() gives. The
 * number of vectors, or an errno (negative).
 */
int
pci_alloc_irq_vectors(struct pci_dev *pdev, int minv, int maxv, unsigned int flags)
{
	unsigned long long address;
	unsigned data, control, can, want;
	bool wide;

	if (pdev != &radio || !(flags & PCI_IRQ_MSI) || radio.msi_cap == 0 || maxv < 1 || maxv > LX_VECTORS) {
		return (-EINVAL);
	}
	if (!lx_os_irq_msi(&address, &data)) {
		return (-ENOSPC);
	}
	control = lx_os_pci_config_read(radio.msi_cap + PCIR_MSI_CTRL, 2);
	can = 1u << (control >> 1 & 7);		/* (what the device says it can take: a power of two) */
	for (want = can; want > (unsigned)maxv; want >>= 1) {
	}
	if (want < (unsigned)minv) {
		return (-ENOSPC);
	}
	if ((data & (want - 1)) != 0) {
		printf("lx: pci: the GIC's numbers for the radio's vectors begin at %u, which %u vectors cannot share\n", data, want);
		return (-ENOSPC);
	}
	wide = (control & PCIM_MSICTRL_64BIT) != 0;
	lx_pci_write_config((device_t)&radio, radio.msi_cap + PCIR_MSI_ADDR, (uint32_t)address, 4);
	if (wide) {
		lx_pci_write_config((device_t)&radio, radio.msi_cap + PCIR_MSI_ADDR_HIGH, (uint32_t)(address >> 32), 4);
		lx_pci_write_config((device_t)&radio, radio.msi_cap + PCIR_MSI_DATA_64BIT, data, 2);
	} else if ((address >> 32) != 0) {
		return (-ENOSPC);
	} else {
		lx_pci_write_config((device_t)&radio, radio.msi_cap + PCIR_MSI_DATA, data, 2);
	}
	control &= ~(7u << 4);
	control |= (unsigned)(__builtin_ctz(want) << 4) | PCIM_MSICTRL_MSI_ENABLE;
	lx_pci_write_config((device_t)&radio, radio.msi_cap + PCIR_MSI_CTRL, control, 2);
	command_set(PCIM_CMD_INTxDIS, 0);	/* (the old wire interrupt is not used) */

	radio_msi.msg.data = data;
	radio_msi.pci.msi_attrib.is_64 = wide;
	radio_vectors = want;
	radio.msi_enabled = true;
	radio.irq = LX_IRQ_BASE;
	radio.dev.irq_start = LX_IRQ_BASE;
	radio.dev.irq_end = LX_IRQ_BASE + want;
	printf("lx: pci: %u MSI vectors: the radio writes %u + v at %#llx%s\n", want, data, address,
	    wide ? "" : " (32-bit addresses)");
	return ((int)want);
}

int
lx_pci_release_msi(device_t dev)
{
	unsigned control;

	if (!is_radio(dev) || radio.msi_cap == 0) {
		return (0);
	}
	control = lx_os_pci_config_read(radio.msi_cap + PCIR_MSI_CTRL, 2);
	lx_pci_write_config(dev, radio.msi_cap + PCIR_MSI_CTRL, control & ~PCIM_MSICTRL_MSI_ENABLE, 2);
	radio_vectors = 0;
	return (0);
}

/* (irq_get_msi_desc(): the description of the device's MSI, kept here; never freed) */
struct msi_desc *
lkpi_pci_msi_desc_alloc(int irq)
{
	return (radio_vectors != 0 && irq >= LX_IRQ_BASE && irq < LX_IRQ_BASE + (int)radio_vectors ? &radio_msi : NULL);
}

/* ---- interrupts --------------------------------------------------------------------------------- */

static struct lx_irq {
	irq_handler_t handler, thread;
	void *argument;
	bool requested;			/* a handler is there (request_irq() to free_irq()) */
	bool attached;			/* the kernel's vector is attached to irq_runs() (from the first request on) */
	volatile int depth;		/* disable_irq() calls not yet undone by enable_irq(): 0 is enabled */
	volatile bool running;
} irqs[LX_VECTORS];

static struct lx_irq *
irq_of(unsigned int irq)
{
	return (irq >= LX_IRQ_BASE && irq < LX_IRQ_BASE + LX_VECTORS ? &irqs[irq - LX_IRQ_BASE] : NULL);
}

/* On the kernel's interrupt thread, the vector masked: the handler, and the vector unmasked unless it was disabled meanwhile. */
static void
irq_runs(void *argument)
{
	struct lx_irq *i = argument;
	unsigned vector = (unsigned)(i - irqs);

	if (i->requested) {
		i->running = true;
		if (i->handler(LX_IRQ_BASE + (int)vector, i->argument) == IRQ_WAKE_THREAD && i->thread != NULL) {
			i->thread(LX_IRQ_BASE + (int)vector, i->argument);
		}
		i->running = false;
	}
	if (i->requested && i->depth <= 0) {
		lx_os_irq_enable(vector);
	}
}

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread, unsigned long flags,
    const char *name, void *argument)
{
	struct lx_irq *i = irq_of(irq);
	unsigned vector;
	int error;

	if (i == NULL || handler == NULL || i->requested || irq >= LX_IRQ_BASE + radio_vectors) {
		return (-EINVAL);
	}
	vector = (unsigned)(i - irqs);
	i->handler = handler;
	i->thread = thread;
	i->argument = argument;
	i->depth = 0;
	if (!i->attached) {
		if ((error = lx_os_irq_attach(vector, irq_runs, i)) != 0) {
			return (-error);
		}
		i->attached = true;
	}
	i->requested = true;
	lx_os_irq_enable(vector);
	return (0);
}

void
lkpi_free_irq(unsigned int irq, void *argument)
{
	struct lx_irq *i = irq_of(irq);

	if (i == NULL || !i->requested) {
		return;
	}
	lx_os_irq_disable((unsigned)(i - irqs));	/* (returns when its handler is not running) */
	i->requested = false;
}

int
lkpi_enable_irq(unsigned int irq)
{
	struct lx_irq *i = irq_of(irq);

	if (i != NULL && i->requested && __atomic_sub_fetch(&i->depth, 1, __ATOMIC_ACQ_REL) <= 0) {
		i->depth = 0;
		if (!i->running) {
			lx_os_irq_enable((unsigned)(i - irqs));
		}
	}
	return (0);
}

/* disable_irq() and disable_irq_nosync() alike: masked, and its handler is not running when this returns (but from itself) */
void
lkpi_disable_irq(unsigned int irq)
{
	struct lx_irq *i = irq_of(irq);

	if (i != NULL && i->requested) {
		__atomic_add_fetch(&i->depth, 1, __ATOMIC_ACQ_REL);
		lx_os_irq_disable((unsigned)(i - irqs));
	}
}

/* synchronize_irq(): its handler is not running when this returns */
void
_intr_drain(int irq)
{
	struct lx_irq *i = irq_of((unsigned)irq);

	if (i != NULL && i->requested) {
		lx_os_irq_disable((unsigned)(i - irqs));
		if (i->depth <= 0) {
			lx_os_irq_enable((unsigned)(i - irqs));
		}
	}
}

int
lkpi_bind_irq_to_cpu(unsigned int irq, int cpu)
{
	return (0);
}

/* ---- softirq context: tasklets and NAPI, on one thread ---------------------------------------- */

#define NAPI_BUDGET	64

static struct {
	bool made;
	void *self;				/* the thread's lx_os_thread_self() */
	unsigned long long wanted;		/* bumped whenever there is something to run */
	TAILQ_HEAD(, tasklet_struct) tasklets;
	TAILQ_HEAD(, napi_struct) polls;
	void *running;				/* the tasklet or NAPI the thread is in */
} soft = { .tasklets = TAILQ_HEAD_INITIALIZER(soft.tasklets), .polls = TAILQ_HEAD_INITIALIZER(soft.polls) };

#define TASKLET_SCHEDULED	1u
#define TASKLET_PARKED		2u		/* scheduled while disabled: tasklet_enable() schedules it */
#define NAPI_ENABLED		(1ul << 8)	/* (the bits of napi->state that are ours; LinuxKPI's are below 8) */
#define NAPI_QUEUED		(1ul << 9)
#define NAPI_AGAIN		(1ul << 10)

static void
soft_runs(void *argument)
{
	soft.self = lx_os_thread_self();
	for (;;) {
		struct tasklet_struct *t;
		struct napi_struct *n;

		lx_os_lock();
		while ((t = TAILQ_FIRST(&soft.tasklets)) == NULL && (n = TAILQ_FIRST(&soft.polls)) == NULL) {
			lx_os_sleep(&soft.wanted, 0);
		}
		if (t != NULL) {
			TAILQ_REMOVE(&soft.tasklets, t, entry);
			t->tasklet_state &= ~TASKLET_SCHEDULED;
			soft.running = t;
			lx_os_unlock();
			if (atomic_read(&t->count) == 0) {
				if (t->use_callback) {
					t->callback(t);
				} else {
					t->func(t->data);
				}
			} else {
				lx_os_lock();
				t->tasklet_state |= TASKLET_PARKED;
				lx_os_unlock();
			}
		} else {
			TAILQ_REMOVE(&soft.polls, n, entry);
			soft.running = n;
			lx_os_unlock();
			if (n->state & NAPI_ENABLED) {
				int done = n->poll(n, NAPI_BUDGET);

				/* a poll that used all it was allowed is not complete: it goes again */
				lx_os_lock();
				if (done >= NAPI_BUDGET || (n->state & NAPI_AGAIN)) {
					n->state &= ~NAPI_AGAIN;
					TAILQ_INSERT_TAIL(&soft.polls, n, entry);
				} else {
					n->state &= ~NAPI_QUEUED;
				}
				lx_os_unlock();
			} else {
				lx_os_lock();
				n->state &= ~NAPI_QUEUED;
				lx_os_unlock();
			}
		}
		lx_os_lock();
		soft.running = NULL;
		lx_os_wakeup(&soft.running);
		lx_os_unlock();
	}
}

static void
soft_wake(void)
{
	if (!soft.made) {
		soft.made = lx_os_thread("wifi-soft", soft_runs, NULL) == 0;
	}
	soft.wanted++;
	lx_os_wakeup(&soft.wanted);
}

/* Until the thread is not in `what` (at once from the thread itself). */
static void
soft_wait(void *what)
{
	if (lx_os_thread_self() == soft.self) {
		return;
	}
	lx_os_lock();
	while (soft.running == what) {
		lx_os_sleep(&soft.running, 2000000ull);
	}
	lx_os_unlock();
}

void
tasklet_setup(struct tasklet_struct *t, tasklet_callback_t *callback)
{
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->use_callback = true;
}

void
tasklet_init(struct tasklet_struct *t, tasklet_func_t *function, unsigned long data)
{
	memset(t, 0, sizeof(*t));
	t->func = function;
	t->data = data;
}

void
tasklet_schedule(struct tasklet_struct *t)
{
	lx_os_lock();
	if (!(t->tasklet_state & TASKLET_SCHEDULED)) {
		t->tasklet_state |= TASKLET_SCHEDULED;
		TAILQ_INSERT_TAIL(&soft.tasklets, t, entry);
		soft_wake();
	}
	lx_os_unlock();
}

void
tasklet_kill(struct tasklet_struct *t)
{
	lx_os_lock();
	if (t->tasklet_state & TASKLET_SCHEDULED) {
		TAILQ_REMOVE(&soft.tasklets, t, entry);
	}
	t->tasklet_state &= ~(TASKLET_SCHEDULED | TASKLET_PARKED);
	lx_os_unlock();
	soft_wait(t);
}

void
tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
}

void
tasklet_disable(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	soft_wait(t);
}

void
tasklet_enable(struct tasklet_struct *t)
{
	bool parked;

	atomic_dec(&t->count);
	lx_os_lock();
	parked = (t->tasklet_state & TASKLET_PARKED) != 0;
	t->tasklet_state &= ~TASKLET_PARKED;
	lx_os_unlock();
	if (parked) {
		tasklet_schedule(t);
	}
}

void
linuxkpi_netif_napi_add(struct net_device *ndev, struct napi_struct *n, int (*poll)(struct napi_struct *, int))
{
	memset(n, 0, sizeof(*n));
	n->dev = ndev;
	n->poll = poll;
	n->budget = NAPI_BUDGET;
}

void
linuxkpi_netif_napi_del(struct napi_struct *n)
{
	linuxkpi_napi_disable(n);
}

bool
linuxkpi_napi_schedule_prep(struct napi_struct *n)
{
	return ((n->state & NAPI_ENABLED) != 0);
}

void
linuxkpi___napi_schedule(struct napi_struct *n)
{
	lx_os_lock();
	if (!(n->state & NAPI_ENABLED)) {
		;
	} else if (n->state & NAPI_QUEUED) {
		n->state |= soft.running == n ? NAPI_AGAIN : 0;	/* (asked for while it polls: it polls again) */
	} else {
		n->state |= NAPI_QUEUED;
		TAILQ_INSERT_TAIL(&soft.polls, n, entry);
		soft_wake();
	}
	lx_os_unlock();
}

bool
linuxkpi_napi_schedule(struct napi_struct *n)
{
	if (!linuxkpi_napi_schedule_prep(n)) {
		return (false);
	}
	linuxkpi___napi_schedule(n);
	return (true);
}

void
linuxkpi_napi_reschedule(struct napi_struct *n)
{
	linuxkpi___napi_schedule(n);
}

/* (from the poll: it is through; soft_runs() takes it off the list when it returns with less than its budget) */
bool
linuxkpi_napi_complete_done(struct napi_struct *n, int done)
{
	return (true);
}

bool
linuxkpi_napi_complete(struct napi_struct *n)
{
	return (true);
}

void
linuxkpi_napi_enable(struct napi_struct *n)
{
	lx_os_lock();
	n->state |= NAPI_ENABLED;
	lx_os_unlock();
}

void
linuxkpi_napi_disable(struct napi_struct *n)
{
	lx_os_lock();
	n->state &= ~NAPI_ENABLED;
	if ((n->state & NAPI_QUEUED) && soft.running != n) {
		TAILQ_REMOVE(&soft.polls, n, entry);
		n->state &= ~NAPI_QUEUED;
	}
	lx_os_unlock();
	soft_wait(n);
}

void
linuxkpi_napi_synchronize(struct napi_struct *n)
{
	soft_wait(n);
}

/* The "dummy" interfaces NAPI hangs on. */
struct net_device *
linuxkpi_alloc_netdev(size_t private, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *ndev = kzalloc(sizeof(*ndev) + private, GFP_KERNEL);

	if (ndev != NULL && setup != NULL) {
		setup(ndev);
	}
	return (ndev);
}

void
linuxkpi_free_netdev(struct net_device *ndev)
{
	kfree(ndev);
}

/* ---- the device tree, of which there is none ------------------------------------------------- */

const struct property *
of_find_property(const struct device_node *np, const char *name, int *length)
{
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *length)
{
	return (NULL);
}

struct device_node *
of_find_node_by_type(struct device_node *from, const char *type)
{
	return (NULL);
}

int
of_address_to_resource(struct device_node *np, int index, struct resource *r)
{
	return (-ENODEV);
}

/* ---- the driver: registered by its module_init(), and given the device ----------------------------- */

int
linux_pci_register_driver(struct pci_driver *driver)
{
	if (radio_driver != NULL) {
		return (-EBUSY);
	}
	radio_driver = driver;
	return (0);
}

void
linux_pci_unregister_driver(struct pci_driver *driver)
{
	if (radio_driver == driver) {
		radio_driver = NULL;
	}
}

/* What the driver's files do at module_init() (FreeBSD's sysinit set: <sys/linker_set.h> keeps it in a section of ours). */
extern struct sysinit *lx_sysinit_start __asm("section$start$__DATA$lx_sysinit");
extern struct sysinit *lx_sysinit_end __asm("section$end$__DATA$lx_sysinit");

static void
modules_init(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	for (struct sysinit **s = &lx_sysinit_start; s < &lx_sysinit_end; s++) {
		(*s)->func((void *)(*s)->udata);
	}
}

/*
 * The device found and given to the driver that knows it (pci.c's ath11k_pci_probe()): its
 * identity from its configuration space. 0, or an errno.
 */
int
lx_pci_probe(void)
{
	uint32_t ids;
	int where;

	modules_init();
	ids = lx_os_pci_config_read(PCIR_VENDOR, 4);
	if (ids == ~0u || ids == 0) {
		lx_tell("lx: pci: no device behind the host bridge (its identity reads %#x)\n", ids);
		return (ENODEV);
	}
	memset(&radio, 0, sizeof(radio));
	memset(&shadow, 0, sizeof(shadow));
	radio.dev.bsddev = (device_t)&radio;
	radio.dev.dma_priv = &radio;		/* (dma_set_mask() asks for there being a DMA tag) */
	radio.dev.kobj.name = "wifi";
	radio.bus = &radio_bus;
	radio.vendor = (uint16_t)ids;
	radio.device = (uint16_t)(ids >> 16);
	ids = lx_os_pci_config_read(PCIR_SUBVEND_0, 4);
	radio.subsystem_vendor = (uint16_t)ids;
	radio.subsystem_device = (uint16_t)(ids >> 16);
	radio.revision = (uint8_t)lx_os_pci_config_read(PCIR_REVID, 1);
	radio.class = lx_os_pci_config_read(PCIR_REVID, 4) >> 8;
	radio.devfn = 0;
	radio_bus.number = 1;
	radio_bus.domain = 0;
	radio.msi_cap = lx_pci_find_cap((device_t)&radio, PCIY_MSI, &where) == 0 ? (uint8_t)where : 0;
	spin_lock_init(&radio.pcie_cap_lock);
	if (radio_driver == NULL) {
		lx_tell("lx: pci: the driver did not register itself\n");
		return (ENXIO);
	}
	for (radio_id = radio_driver->id_table; radio_id != NULL && radio_id->vendor != 0; radio_id++) {
		if (radio_id->vendor == radio.vendor && radio_id->device == radio.device) {
			break;
		}
	}
	if (radio_id == NULL || radio_id->vendor == 0) {
		lx_tell("lx: pci: the driver does not know device %04x:%04x\n", radio.vendor, radio.device);
		return (ENODEV);
	}
	radio.pdrv = radio_driver;
	lx_tell("lx: pci: device %04x:%04x (subsystem %04x:%04x, revision %u, MSI capability at %#x): the driver probes it\n",
	    radio.vendor, radio.device, radio.subsystem_vendor, radio.subsystem_device, radio.revision, radio.msi_cap);
	if ((where = radio_driver->probe(&radio, radio_id)) != 0) {
		lx_tell("lx: pci: the driver's probe fails: %d\n", where);
		return (-where);
	}
	radio_probed = true;
	return (0);
}

/* The driver's removal (ath11k_pci_remove(): the firmware off, everything given back), as Linux does it before a device goes. */
void
lx_pci_remove(void)
{
	if (radio_probed && radio_driver->remove != NULL) {
		radio_driver->remove(&radio);
	}
	radio_probed = false;
}

void *
lx_pci_driver(void)
{
	return (radio_probed ? pci_get_drvdata(&radio) : NULL);
}

/* After the kernel has given the device its power and its link back. */
void
lx_pci_resumed(void)
{
	if (radio_probed) {
		lx_pci_restore();
	}
}
