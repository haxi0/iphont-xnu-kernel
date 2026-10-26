/*
 * Darwin-on-redwood: the station boundary.
 *
 * On one side whatever does the scanning and the joining with a radio: the station logic over
 * ath11k on the phone (wifi/lx in the repository). On the other whoever wants a network: the
 * Apple80211 requests of iOS's Wi-Fi side, answered in bsd/dev/hk_apple80211.c, and the
 * kernel's own join of the network it was given in a file (bsd/dev/hk_wifi.c). Frames do
 * not pass here, and neither does "the link is up": those go through bsd/sys/hk_net.h,
 * which the station side calls itself.
 *
 * Between the two stands bsd/dev/hk_sta.c: one thread, on which every call into the station
 * side is made, one at a time, and what the last scan heard, kept for whoever asks.
 *
 * Nothing but C's own types here: the station side is not compiled as XNU's code.
 */
#ifndef _SYS_HK_STA_H_
#define _SYS_HK_STA_H_

#include <stdbool.h>
#include <stdint.h>

#define HK_STA_ELEMENTS_MOST       1024

/* One access point, as a scan heard it. */
struct hk_sta_bss {
	uint8_t bssid[6];
	uint8_t ssid[32], ssid_length;
	uint16_t frequency;                     /* MHz */
	int16_t rssi;                           /* dBm */
	uint16_t beacon_interval;               /* in units of 1024 microseconds */
	uint16_t capability;                    /* the beacon's capability field */
	uint32_t age_ms;                        /* how long ago it was last heard */
	uint16_t elements_length;               /* the beacon's information elements as received: who asks */
	uint8_t elements[HK_STA_ELEMENTS_MOST];    /* reads the network's protection out of them itself */
};

/* What to scan for. All zero: every channel, the radio's own way. */
struct hk_sta_scan {
	uint8_t ssid[32], ssid_length;          /* ask for this network by name (a hidden one answers only then) */
	uint16_t frequencies[64], frequency_count;      /* only these channels */
	bool passive;                           /* only listen */
	uint16_t dwell_ms;                      /* on each channel; 0: the radio's own */
};

enum { HK_STA_OPEN, HK_STA_WPA2_PSK };     /* WPA2_PSK: a passphrase (or its key) -- WPA3's SAE too, which takes the passphrase */

struct hk_sta_join {
	uint8_t ssid[32], ssid_length;
	uint8_t bssid[6];                       /* all zero: the access point of that name heard best */
	int security;
	int akm;                                /* the key management asked for (00-0f-ac-n: 2 PSK, 6 PSK-SHA256, 8 SAE); 0: the station's choice */
	uint8_t address[6];                     /* ours for this network (iOS's private Wi-Fi address); all zero: the radio's own */
	uint8_t pmk[32];                        /* WPA2 with a passphrase: the key made of the passphrase and the name ... */
	uint8_t passphrase[64], passphrase_length;      /* ... or, if this is not empty, the passphrase itself */
};

enum hk_sta_phase { HK_STA_OFF, HK_STA_IDLE, HK_STA_SCANNING, HK_STA_JOINING, HK_STA_JOINED };

struct hk_sta_state {
	enum hk_sta_phase phase;
	struct hk_sta_bss bss;             /* the network joined or being joined */
	int last_error;                         /* why the last join failed or the network was lost (an errno); 0: it was not */
};

#define HK_STA_CHANNEL_PASSIVE     1u      /* the radio may only listen there */
#define HK_STA_CHANNEL_RADAR       2u      /* ... and has to look out for radar */

struct hk_sta_channel {
	uint16_t frequency;                     /* MHz */
	uint16_t flags;
};

/*
 * What the station side supplies. All but state() are called on one thread, one at a time, and
 * may take as long as the thing takes: scan() returns when the scan is over, join() when frames
 * can pass or it is known why not. Each gives 0 or an errno.
 */
struct hk_sta_ops {
	int (*power)(bool on);                  /* off: leave the network, stop the radio; on: start it */
	int (*scan)(const struct hk_sta_scan *what);
	void (*heard)(void (*each)(const struct hk_sta_bss *bss, void *context), void *context);   /* by the last scan */
	int (*join)(const struct hk_sta_join *what);
	int (*leave)(void);
	void (*state)(struct hk_sta_state *state);         /* on any thread; does not sleep */
	unsigned (*channels)(struct hk_sta_channel *channels, unsigned most);      /* what the radio may tune to */
	uint8_t address[6];
};

/* What happens, said to whoever listens (the Apple80211 side). */
enum hk_sta_event {
	HK_STA_POWER,                      /* status: 1 on, 0 off */
	HK_STA_SCAN_DONE,                  /* status: 0, or the scan's errno */
	HK_STA_JOIN_DONE,                  /* status: 0, or the join's errno */
	HK_STA_LINK,                       /* status: 1 a network is joined, 0 it has been left when asked,
	                                         * an errno: it was lost (the access point showed us the door) */
};

#ifdef KERNEL
/* ---- the station side ---- */
void     hk_sta_attach(const struct hk_sta_ops *ops);         /* NULL: it is going away (waits for a call under way) */
void     hk_sta_lost(int error);                                   /* the network joined is gone by itself */

/* ---- who wants a network: each asks and returns; the outcome is an event. 0, or an errno ---- */
bool     hk_sta_present(void);
int      hk_sta_ask_power(bool on);
int      hk_sta_ask_scan(const struct hk_sta_scan *what);
int      hk_sta_ask_join(const struct hk_sta_join *what);
/* ... and joined again whenever the radio is turned on, until some other network is asked for */
int      hk_sta_ask_join_standing(const struct hk_sta_join *what);
int      hk_sta_ask_leave(void);
bool     hk_sta_busy(void);                                        /* something asked for is not over yet */
bool     hk_sta_powered(void);
/* the `index`th access point the last scan heard: true, or false when there are not that many */
bool     hk_sta_heard(unsigned index, struct hk_sta_bss *bss);
void     hk_sta_state(struct hk_sta_state *state);
unsigned hk_sta_channels(struct hk_sta_channel *channels, unsigned most);
bool     hk_sta_address(uint8_t *address);
/*
 * The system's sleep (bsd/dev/hk_wifi.c): the station's calls held, the network left and the radio stopped -- iOS
 * is told the link is down, not that the radio is off; true if it was on. And after it: the calls go on, the radio
 * is on again if it was, and the standing network is joined again.
 */
bool     hk_sta_suspend(void);
void     hk_sta_resume(bool was_on);

/* ---- the listener: bsd/dev/hk_apple80211.c ---- */
void     hk_apple80211_event(enum hk_sta_event event, int status);
void     hk_apple80211_summary(void);                              /* what was asked in this run, to the log */
#endif

#endif
