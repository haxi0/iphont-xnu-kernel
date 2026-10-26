/*
 * SAE -- WPA3's "simultaneous authentication of equals" -- as a station does it with an access
 * point (IEEE 802.11-2020, 12.4): from a passphrase and the two addresses a point of the curve
 * (the "password element", by hunting and pecking, 12.4.4.2.2), a commit of a scalar and an
 * element each way, and a confirm each way that proves both have the same key; out of it the
 * PMK and PMKID the four-way handshake goes on with (AKM 8, lx_wpa.c). Group 19 only: the NIST
 * P-256 curve, the one every WPA3 access point has to take.
 *
 * Like lx_wpa.c, arithmetic and nothing else, run on the build machine too (tools/wpa-test) as it
 * runs in the kernel. Not done: the hash-to-element way of finding the point (H2E, which an
 * access point that takes only that refuses us for with status 126), and groups but 19.
 */
#ifndef _LX_SAE_H_
#define _LX_SAE_H_

#define LX_SAE_GROUP		19
#define LX_SAE_COMMIT_MOST	(2 + 256 + 32 + 64 + 3)	/* group, an anti-clogging token of up to 256 bytes, scalar, element (H2E: the token's container) */
#define LX_SAE_CONFIRM		(2 + 32)

struct lx_sae {
	unsigned char own[6], peer[6];		/* the station's address and the access point's */
	/* from here on: lx_sae.c's */
	unsigned long long pwe[8];		/* the password element, x and y */
	unsigned long long rand[4];		/* ours, secret */
	unsigned char scalar[32], element[64];	/* our commit */
	unsigned char peer_scalar[32], peer_element[64];
	unsigned char kck[32];			/* what the confirms are made with */
	unsigned send_confirm;
	int have_pwe, have_keys;
	int h2e;				/* the password element is hash-to-element's (status 126 in the frames) */
	/* what comes out of it */
	unsigned char pmk[32], pmkid[16];
};

/*
 * The password element and our commit, from the passphrase; `random` gives bytes nobody can
 * guess. 0, or -1 (no point found in 40 tries: does not happen with a passphrase of real length).
 */
int lx_sae_prepare(struct lx_sae *sae, const unsigned char *password, unsigned password_length, const unsigned char own[6],
    const unsigned char peer[6], void (*random)(void *bytes, unsigned length));
/*
 * Hash to element (H2E, IEEE 802.11-2020, 12.4.4.2.3): the network's PT, once per name and
 * passphrase (and password identifier, if any: NULL for none), 64 bytes x || y; then for an
 * access point the password element and our commit from it. 0, or -1.
 */
int lx_sae_pt(unsigned char pt[64], const unsigned char *password, unsigned password_length, const unsigned char *ssid,
    unsigned ssid_length, const unsigned char *identifier, unsigned identifier_length);
int lx_sae_prepare_pt(struct lx_sae *sae, const unsigned char pt[64], const unsigned char own[6], const unsigned char peer[6],
    void (*random)(void *bytes, unsigned length));
/* Our commit, as the authentication frame's body after its status: group, the token if one was asked for, scalar, element. */
unsigned lx_sae_commit(const struct lx_sae *sae, const unsigned char *token, unsigned token_length, unsigned char *out);
/* The access point's commit (from its group on): checked, and the keys made. 0, or -1 with *why. */
int lx_sae_peer_commit(struct lx_sae *sae, const unsigned char *body, unsigned length, const char **why);
/* Our confirm: the counter and the 32 bytes. */
unsigned lx_sae_confirm(struct lx_sae *sae, unsigned char *out);
/* The access point's confirm: 0 if it proves the same key (the PMK is then good), or -1 with *why. */
int lx_sae_peer_confirm(const struct lx_sae *sae, const unsigned char *body, unsigned length, const char **why);

#endif
