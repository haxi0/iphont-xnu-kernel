/*
 * Linux's MHI interface (linux/mhi.h, as wifi/lx/patches makes LinuxKPI's declare it) for ath11k's
 * mhi.c and pci.c, on the MHI host of mhi_host.c -- and, in the place of Linux's qrtr-mhi, the
 * carrying of QRTR over the device's channels "IPCR" (20 out, 21 in) to and from the kernel's
 * QRTR (lx_os_qrtr_*()), which is where the driver's QMI goes.
 *
 * There is one controller: the radio. Its interrupts are its first MSI vectors (pcic.c gives
 * the "MHI" user three: BHI and the two event rings); beside them a thread looks at the device
 * every 10 ms while it is powered, so that nothing waits on an interrupt that did not come.
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/mhi.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <lx_os.h>
#include "lx.h"
#include "mhi_host.h"

#define IPCR_OUT	20
#define IPCR_IN		21
#define LOOK_MS		10

static struct {
	struct mhi_controller *c;
	const struct mhi_controller_config *config;
	struct mhi_device device;
	struct mhi_host host;
	struct mutex lock;
	bool lock_made;
	const struct firmware *firmware;
	int vectors[MHI_HOST_EVENT_RINGS + 1];	/* the argument of each interrupt's handler: its vector */
	bool irqs;				/* requested */
	volatile bool looking, looks;		/* the thread that looks: wanted, running */
} mhi;

/* ---- what the host asks of the machine ------------------------------------------------------------- */

static uint32_t
op_read(void *context, uint32_t offset)
{
	uint32_t value = ~0u;

	mhi.c->read_reg(mhi.c, (uint8_t *)mhi.c->regs + offset, &value);
	return (value);
}

static void
op_write(void *context, uint32_t offset, uint32_t value)
{
	mhi.c->write_reg(mhi.c, (uint8_t *)mhi.c->regs + offset, value);
}

static void *
op_dma_alloc(void *context, size_t size, size_t align, uint64_t *address)
{
	dma_addr_t dma;
	void *memory = lx_dma_alloc(size, align, &dma);

	*address = dma;
	return (memory);
}

static void
op_dma_free(void *context, void *memory, size_t size)
{
	lx_dma_free(memory, size);
}

static uint64_t
op_nanoseconds(void *context)
{
	return (lx_os_nanoseconds());
}

static void
op_lock(void *context)
{
	mutex_lock(&mhi.lock);
}

static void
op_unlock(void *context)
{
	mutex_unlock(&mhi.lock);
}

static void
op_pause(void *context, unsigned microseconds)
{
	static int nobody;

	lx_os_lock();
	lx_os_sleep(&nobody, (unsigned long long)microseconds * 1000ull);
	lx_os_unlock();
}

static void
op_random(void *context, void *bytes, size_t length)
{
	lx_os_random(bytes, length);
}

static void
op_say(void *context, const char *line)
{
	lx_tell("%s\n", line);
}

static void
op_status(void *context, enum mhi_host_status what)
{
	if (mhi.c->status_cb != NULL) {
		mhi.c->status_cb(mhi.c, what == MHI_HOST_MISSION ? MHI_CB_EE_MISSION_MODE : what == MHI_HOST_RDDM ? MHI_CB_EE_RDDM :
		    MHI_CB_SYS_ERROR);
	}
}

static void
op_receive(void *context, unsigned channel, const void *data, size_t length)
{
	if (channel == IPCR_IN) {
		lx_os_qrtr_input(data, length);
	}
}

static const struct mhi_host_ops ops = {
	.read = op_read,
	.write = op_write,
	.dma_alloc = op_dma_alloc,
	.dma_free = op_dma_free,
	.nanoseconds = op_nanoseconds,
	.lock = op_lock,
	.unlock = op_unlock,
	.pause = op_pause,
	.random = op_random,
	.say = op_say,
	.status = op_status,
	.receive = op_receive,
};

/* The kernel's QRTR sends through here. */
static int
qrtr_send(const void *data, unsigned long length)
{
	return (mhi_host_send(&mhi.host, IPCR_OUT, data, length));
}

/* ---- interrupts, and the look every 10 ms ------------------------------------------------------- */

static irqreturn_t
interrupt(int irq, void *argument)
{
	mhi_host_interrupt(&mhi.host, *(int *)argument);
	return (IRQ_HANDLED);
}

static void
looks(void *argument)
{
	mhi.looks = true;
	while (mhi.looking) {
		mhi_host_interrupt(&mhi.host, -1);
		lx_pause_ms(LOOK_MS);
	}
	mhi.looks = false;
}

static void
looking(bool on)
{
	if (on && !mhi.looks) {
		mhi.looking = true;
		if (lx_spawn("wifi-mhi", looks, NULL) != 0) {
			mhi.looking = false;
		}
		return;
	}
	if (!on) {
		mhi.looking = false;
		while (mhi.looks) {
			lx_pause_ms(1);
		}
	}
}

static void
irqs_free(void)
{
	if (!mhi.irqs) {
		return;
	}
	for (int i = 0; i < mhi.c->nr_irqs && i < (int)nitems(mhi.vectors); i++) {
		free_irq(mhi.c->irq[i], &mhi.vectors[i]);
	}
	mhi.irqs = false;
}

static int
irqs_request(void)
{
	int ret;

	if (mhi.irqs) {
		return (0);
	}
	for (int i = 0; i < mhi.c->nr_irqs && i < (int)nitems(mhi.vectors); i++) {
		mhi.vectors[i] = i;
		if ((ret = request_irq(mhi.c->irq[i], interrupt, mhi.c->irq_flags, "mhi", &mhi.vectors[i])) != 0) {
			while (--i >= 0) {
				free_irq(mhi.c->irq[i], &mhi.vectors[i]);
			}
			return (ret);
		}
	}
	mhi.irqs = true;
	return (0);
}

/* ---- the controller ------------------------------------------------------------------------------ */

struct mhi_controller *
linuxkpi_mhi_alloc_controller(void)
{
	return (kzalloc(sizeof(struct mhi_controller), GFP_KERNEL));
}

void
linuxkpi_mhi_free_controller(struct mhi_controller *c)
{
	if (c == mhi.c) {
		mhi.c = NULL;
	}
	kfree(c);
}

int
linuxkpi_mhi_register_controller(struct mhi_controller *c, const struct mhi_controller_config *config)
{
	if (mhi.c != NULL && mhi.c != c) {
		return (-EBUSY);
	}
	if (!mhi.lock_made) {
		mutex_init(&mhi.lock);
		mhi.lock_made = true;
	}
	mhi.c = c;
	mhi.config = config;
	mhi.device.mhi_cntrl = c;
	c->mhi_dev = &mhi.device;
	c->host = &mhi.host;
	return (0);
}

void
linuxkpi_mhi_unregister_controller(struct mhi_controller *c)
{
	if (c != mhi.c) {
		return;
	}
	looking(false);
	irqs_free();
	c->mhi_dev = NULL;
}

/* The host's description of the device, from the driver's (mhi.c's ath11k_mhi_config_qca6390) and the controller's. */
static int
host_config(struct mhi_host_config *h)
{
	const struct mhi_controller_config *config = mhi.config;
	struct mhi_controller *c = mhi.c;

	memset(h, 0, sizeof(*h));
	if (config->num_channels > MHI_HOST_CHANNELS || config->num_events > MHI_HOST_EVENT_RINGS) {
		lx_tell("lx: mhi: %d channels and %d event rings asked for: this host takes %u and %u\n", config->num_channels,
		    config->num_events, MHI_HOST_CHANNELS, MHI_HOST_EVENT_RINGS);
		return (EINVAL);
	}
	h->max_channels = (uint32_t)config->max_channels;
	h->timeout_ms = c->timeout_ms != 0 ? c->timeout_ms : config->timeout_ms;
	h->buffer_length = config->buf_len != 0 ? (uint32_t)config->buf_len : 8192;
	h->channel_count = (unsigned)config->num_channels;
	for (int i = 0; i < config->num_channels; i++) {
		h->channels[i].number = (uint32_t)config->ch_cfg[i].num;
		h->channels[i].elements = (uint32_t)config->ch_cfg[i].num_elements;
		h->channels[i].event_ring = (uint32_t)config->ch_cfg[i].event_ring;
		h->channels[i].inbound = config->ch_cfg[i].dir == DMA_FROM_DEVICE;
		h->channels[i].ee_mask = (uint32_t)config->ch_cfg[i].ee_mask;
	}
	h->event_count = (unsigned)config->num_events;
	for (int i = 0; i < config->num_events; i++) {
		h->events[i].elements = (uint32_t)config->event_cfg[i].num_elements;
		h->events[i].vector = (uint32_t)config->event_cfg[i].irq;
		h->events[i].moderation_ms = (uint32_t)config->event_cfg[i].irq_moderation_ms;
		h->events[i].control = config->event_cfg[i].data_type == MHI_ER_CTRL;
	}
	h->sbl_length = c->sbl_size;
	h->segment_length = c->seg_len;
	h->iova_start = c->iova_start;
	h->iova_stop = c->iova_stop;
	if (c->fw_data != NULL && c->fw_sz != 0) {
		h->image = c->fw_data;
		h->image_length = c->fw_sz;
	} else {
		if (mhi.firmware == NULL && request_firmware(&mhi.firmware, c->fw_image, c->cntrl_dev) != 0) {
			lx_tell("lx: mhi: no firmware \"%s\"\n", c->fw_image != NULL ? c->fw_image : "(none named)");
			mhi.firmware = NULL;
			return (ENOENT);
		}
		h->image = mhi.firmware->data;
		h->image_length = mhi.firmware->size;
	}
	return (0);
}

int
mhi_prepare_for_power_up(struct mhi_controller *c)
{
	struct mhi_host_config config;
	int error;

	if (c != mhi.c) {
		return (-EINVAL);
	}
	if (!mhi.host.allocated) {
		if ((error = host_config(&config)) != 0 || (error = mhi_host_init(&mhi.host, &ops, NULL, &config)) != 0) {
			return (-error);
		}
	} else {
		mhi.host.config.timeout_ms = c->timeout_ms != 0 ? c->timeout_ms : mhi.config->timeout_ms;
	}
	return (irqs_request());
}

int
mhi_sync_power_up(struct mhi_controller *c)
{
	int error;

	if (c != mhi.c || !mhi.host.allocated) {
		return (-EINVAL);
	}
	/*
	 * (the kernel's QRTR may send from here on: the radio's hello comes as soon as channel 21 has its buffers,
	 * and ours is to go back at once; until channel 20 runs a send says ENOTCONN)
	 */
	lx_os_qrtr_attach(qrtr_send);
	looking(true);
	if ((error = mhi_host_power_up(&mhi.host)) != 0) {
		lx_os_qrtr_attach(NULL);
		looking(false);
		return (-error);
	}
	return (0);
}

int
mhi_async_power_up(struct mhi_controller *c)
{
	return (mhi_sync_power_up(c));
}

void
mhi_power_down(struct mhi_controller *c, bool graceful)
{
	if (c != mhi.c) {
		return;
	}
	lx_os_qrtr_attach(NULL);
	looking(false);
	mhi_host_power_down(&mhi.host);
}

void
mhi_power_down_keep_dev(struct mhi_controller *c, bool graceful)
{
	mhi_power_down(c, graceful);
}

/* (the memory is kept for the next power-up: the driver frees and makes it again only when it goes) */
void
mhi_unprepare_after_power_down(struct mhi_controller *c)
{
	if (c == mhi.c) {
		irqs_free();
	}
}

int
mhi_pm_suspend(struct mhi_controller *c)
{
	return (c == mhi.c ? -mhi_host_suspend(&mhi.host) : -EINVAL);
}

int
mhi_pm_resume(struct mhi_controller *c)
{
	return (c == mhi.c ? -mhi_host_resume(&mhi.host) : -EINVAL);
}

int
mhi_pm_resume_force(struct mhi_controller *c)
{
	return (mhi_pm_resume(c));
}

int
mhi_device_get_sync(struct mhi_device *device)
{
	return (device != NULL && device->mhi_cntrl == mhi.c ? -mhi_host_wake_get(&mhi.host) : -EINVAL);
}

void
mhi_device_put(struct mhi_device *device)
{
	if (device != NULL && device->mhi_cntrl == mhi.c) {
		mhi_host_wake_put(&mhi.host);
	}
}

int
mhi_force_rddm_mode(struct mhi_controller *c)
{
	return (c == mhi.c ? -mhi_host_force_rddm(&mhi.host) : -EINVAL);
}

/* A crash dump is not taken here: the firmware's memory is not read back (there is nowhere to keep 4 MB of it). */
int
mhi_download_rddm_image(struct mhi_controller *c, bool in_panic)
{
	lx_tell("lx: mhi: the firmware's crash dump is not taken\n");
	return (-EOPNOTSUPP);
}
