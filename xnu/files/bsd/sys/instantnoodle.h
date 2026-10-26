/*
 * What the OnePlus 8 (instantnoodle, SM8250) drivers share: the board check, the TLMM pins and
 * I2C done on two of them by hand (bsd/dev/instantnoodle_i2c.c).
 */
#ifndef _SYS_INSTANTNOODLE_H_
#define _SYS_INSTANTNOODLE_H_

#include <stdbool.h>
#include <stdint.h>

/* The three tiles of the SM8250 TLMM (Linux pinctrl-sm8250.c); a pin's registers are 0x1000 * pin into its tile. */
#define INSTANTNOODLE_TLMM_WEST         0x0f100000UL
#define INSTANTNOODLE_TLMM_SOUTH        0x0f500000UL
#define INSTANTNOODLE_TLMM_NORTH        0x0f900000UL

bool instantnoodle_board(void);         /* boot-arg board=instantnoodle (xnuloader passes it) */

struct instantnoodle_pin {
	uint64_t tile;                  /* INSTANTNOODLE_TLMM_* */
	unsigned pin;
};
#define INSTANTNOODLE_PULL_NONE         0
#define INSTANTNOODLE_PULL_DOWN         1
#define INSTANTNOODLE_PULL_UP           3
uint32_t instantnoodle_gpio_cfg(struct instantnoodle_pin p);       /* the pin's configuration register as it is */
void instantnoodle_gpio_input(struct instantnoodle_pin p, unsigned pull);
void instantnoodle_gpio_output(struct instantnoodle_pin p, bool high, unsigned pull, unsigned ma);
void instantnoodle_gpio_function(struct instantnoodle_pin p, unsigned function, unsigned pull, unsigned ma);   /* a function of the pin's other than GPIO */
bool instantnoodle_gpio_level(struct instantnoodle_pin p);

/*
 * An I2C bus driven on its two pins (open drain: low = output 0, high = input, the board's
 * pull-ups, or the pins' own, make the level), about 100 kHz. The SoC's serial engines are not used: their FIFO
 * mode is off on this phone and the DMA mode needs GPI. One transfer at a time per bus.
 */
struct instantnoodle_i2c {
	const char *name;
	struct instantnoodle_pin sda, scl;
	unsigned pull;                  /* INSTANTNOODLE_PULL_UP where the board has no pull-ups of its own */
	unsigned errors;                /* transfers that were not acknowledged */
	struct _lck_mtx_ *lock;          /* initialized on first use; the bus lives for the kernel's lifetime */
	bool trace;                     /* touch_lat=1: lock wait and wire time */
};
int instantnoodle_i2c_write(struct instantnoodle_i2c *bus, uint8_t addr, const uint8_t *bytes, unsigned n);
int instantnoodle_i2c_read(struct instantnoodle_i2c *bus, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n);
void instantnoodle_i2c_recover(struct instantnoodle_i2c *bus);     /* nine clocks and a stop: a target holding SDA lets go */

void instantnoodle_battery_init(void);  /* the bq27541 fuel gauge (bsd/dev/instantnoodle_battery.c) */
void instantnoodle_slider_init(void);
/* an RPMh LDO on at `mv' (its name in RPMh's command database, e.g. "ldoc10"): bsd/dev/instantnoodle_touch.c */
bool instantnoodle_supply_on(const char *name, uint32_t mv);   /* the alert slider's hall sensors (bsd/dev/instantnoodle_slider.c) */
/*
 * Votes of the application processor's in RPMh (bsd/dev/instantnoodle_touch.c), by the resources' names in its command
 * database: a regulator ("ldoc5", "smpa6") at `mv' and on, or off (an LDO in its high-power mode); an ARC rail
 * ("cx.lvl") at the first of its levels at or above `level' (0: no vote); a bus clock manager ("SN12"): x average, y
 * peak, in its units (0 0: no vote). true: RPMh has said it took it.
 */
bool instantnoodle_supply(const char *name, uint32_t mv, bool on);
bool instantnoodle_rail_level(const char *name, uint32_t level);
bool instantnoodle_bcm_vote(const char *name, uint32_t x, uint32_t y);

/*
 * The Wi-Fi's QCA6390 on PCIe (bsd/dev/instantnoodle_wifi.c): its power and the SoC's PCIe 0 with its link up and the
 * device placed (on), or the link down and the device off; its configuration space; its memory window. For
 * bsd/dev/hk_wifi.c, which runs the driver on it.
 */
bool instantnoodle_wifi_power(bool on);
uint32_t instantnoodle_wifi_config_read(uint32_t offset, unsigned width);
void instantnoodle_wifi_config_write(uint32_t offset, uint32_t value, unsigned width);
bool instantnoodle_wifi_bar(unsigned bar, uint64_t *physical, uint64_t *size);
void instantnoodle_wifi_quiesce(void);             /* the device in reset and the link down, from a panic's way back: no locks */

/* The speakers' two TFA9874 amplifiers (bsd/dev/instantnoodle_speaker.c): hk_speaker.c's hk_speaker_*() on this board */
bool instantnoodle_speaker_prepare(const uint8_t *container, uint64_t size);
bool instantnoodle_speaker_start(void);
bool instantnoodle_speaker_volume(uint32_t below, uint32_t step_ms);
bool instantnoodle_speaker_healthy(void);
uint32_t instantnoodle_speaker_monitor(bool tell);
void instantnoodle_speaker_stop(void);
void instantnoodle_speaker_quiesce(void);
void instantnoodle_speaker_probe(uint32_t level);     /* boot-arg speaker_probe=N */

#endif /* _SYS_INSTANTNOODLE_H_ */
