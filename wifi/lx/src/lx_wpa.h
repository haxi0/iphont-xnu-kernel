/*
 * WPA2 and WPA3 with a passphrase, as a station sees it: the key a name and a passphrase make
 * (WPA2; WPA3's comes out of SAE, lx_sae.c), and the four-way handshake in which an access point
 * and a station prove that key to each other and arrive at the keys their frames are ciphered
 * with (IEEE 802.11-2020, 12.7; RSN, AKM "PSK" with key descriptor version 2: HMAC-SHA1 and the
 * AES key wrap; AKM "SAE" (8): AES-CMAC and KDF-SHA-256, descriptor version 0), with the group's
 * management key (IGTK) where management frames are protected. lx_wpa.c is the arithmetic and
 * nothing else -- no radio, no kernel --, so that it runs on the machine the kernel is built on
 * (tools/wpa-test) as it runs in the kernel.
 */
#ifndef _LX_WPA_H_
#define _LX_WPA_H_

/* The key ("PMK") of a network's name and passphrase: 32 bytes. Slow on purpose: 8192 rounds of HMAC-SHA1. */
void lx_wpa_pmk(const unsigned char *passphrase, unsigned passphrase_length, const unsigned char *name, unsigned name_length,
    unsigned char pmk[32]);

#define LX_WPA_AKM_PSK		2
#define LX_WPA_AKM_PSK_SHA256	6	/* PSK with KDF-SHA-256 and AES-CMAC, descriptor version 3 (networks with PMF) */
#define LX_WPA_AKM_SAE		8

struct lx_wpa {
	int akm;				/* LX_WPA_AKM_*: 0 is PSK */
	unsigned char pmk[32];
	unsigned char own[6], peer[6];		/* the station's address and the access point's */
	unsigned char own_nonce[32];		/* 32 bytes nobody can guess, fresh for every handshake */
	unsigned char next_nonce[32];		/* ... the next one: taken at the next message 1 when renew_nonce is set */
	int renew_nonce;			/* (the caller's, once a handshake's keys are in use: wpa_supplicant's renew_snonce) */
	const unsigned char *rsn;		/* the station's RSN element as its association request had it ... */
	unsigned rsn_length;			/* ... which goes into message 2 */
	const unsigned char *peer_rsn, *peer_rsnxe;	/* the access point's, as its beacon had them (RSNXE: NULL if none) ... */
	unsigned peer_rsn_length, peer_rsnxe_length;	/* ... which message 3 must repeat (no downgrade); NULL: not checked */
	/* from here on: lx_wpa.c's */
	unsigned char peer_nonce[32];
	unsigned char kck[16], kek[16];		/* the keys the handshake's own messages are signed and ciphered with */
	unsigned char replay[8];		/* the counter of the last message accepted */
	int have_keys, replay_known;
	/*
	 * A later handshake's keys (a new message 1 while keys are in use: the access point renewing them) are kept apart
	 * until its message 3 is signed with them -- wpa_supplicant's TPTK --, so that an unsigned message 1 cannot take
	 * the keys the access point's group messages are checked with.
	 */
	unsigned char t_kck[16], t_kek[16], t_pairwise[16];
	int have_tptk;
	/* what comes out of it */
	unsigned char pairwise[16];		/* the key for frames between the two ("TK", CCMP) */
	unsigned char group[32];		/* the key the access point sends to everybody with ("GTK") ... */
	unsigned group_length, group_index;	/* ... its length, and its number (1 to 3) */
	unsigned char group_counter[8];		/* ... and the packet number its frames have got to ("RSC") */
	unsigned char igtk[16];			/* the group's key for management frames (BIP-CMAC-128), if given ... */
	unsigned igtk_index, have_igtk;		/* ... its number (4 or 5) */
	unsigned char igtk_counter[6];		/* ... and the packet number its frames have got to ("IPN") */
	unsigned char eapol_version;		/* of the access point's last message, for ours unasked */
	unsigned char request_counter[8];	/* the counter of our own requests (a MIC failure's report) */
};

/*
 * What a message of the handshake (an EAPOL frame: what follows the Ethernet header of a frame
 * of type 0x888e) calls for. `reply` has room for LX_WPA_REPLY_MOST bytes.
 */
#define LX_WPA_REPLY_MOST	256
enum {
	LX_WPA_BAD = -1,		/* not a message that can be answered: *why says what was wrong with it */
	LX_WPA_NOTHING = 0,		/* not for us to answer (not a key message) */
	LX_WPA_ANSWER = 1,		/* message 1: send the reply (message 2) */
	LX_WPA_KEYS = 2,		/* message 3: send the reply (message 4), then both keys are to be used */
	LX_WPA_GROUP = 3,		/* a new group key later on: send the reply, then use the new group key */
};
int lx_wpa_message(struct lx_wpa *wpa, const unsigned char *message, unsigned length, unsigned char *reply,
    unsigned *reply_length, const char **why);

/*
 * A report of a failed Michael check (TKIP), as a key request with "error" set (IEEE 802.11-2020,
 * 12.7.2 and 12.5.2.4): of frames between the two (pairwise) or to everybody. 0, or -1 when there
 * are no keys yet to sign it with. `out` has room for LX_WPA_REPLY_MOST bytes.
 */
int lx_wpa_mic_failure_report(struct lx_wpa *wpa, int pairwise, unsigned char *out, unsigned *length);

/* The primitives, for lx_sae.c and the protection of management frames: several pieces hashed one after another. */
void lx_wpa_hmac_sha256(const unsigned char *key, unsigned key_length, const unsigned char *const pieces[],
    const unsigned lengths[], unsigned count, unsigned char digest[32]);
void lx_wpa_kdf_sha256(const unsigned char *key, unsigned key_length, const char *label, const unsigned char *context,
    unsigned context_length, unsigned char *out, unsigned bits);
void lx_wpa_cmac(const unsigned char key[16], const unsigned char *const pieces[], const unsigned lengths[], unsigned count,
    unsigned char mac[16]);

/*
 * Michael, TKIP's integrity check (IEEE 802.11-2020, 12.5.2.3): 8 bytes of key, then the
 * message a piece at a time, then the 8 bytes of the check. For the frames of a network whose
 * group cipher is TKIP that the radio leaves the check of to its driver (ath11k: crypto/hash.h).
 */
struct lx_michael {
	unsigned int l, r;
	unsigned char pending[4];
	unsigned pending_length;
};
void lx_wpa_michael_begin(struct lx_michael *m, const unsigned char key[8]);
void lx_wpa_michael_more(struct lx_michael *m, const unsigned char *data, unsigned length);
void lx_wpa_michael_end(struct lx_michael *m, unsigned char mic[8]);

#endif
