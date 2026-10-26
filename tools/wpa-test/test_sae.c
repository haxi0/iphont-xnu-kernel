/*
 * Tests of wifi/lx/src/lx_sae.c and of lx_wpa.c's WPA3 parts on the machine the kernel is built on: SAE against an
 * access point written in Python on plain integers (sae_reference.py), the four-way handshake of AKM 8 with the
 * group's management key, AES-CMAC against RFC 4493's examples, and a BIP-CMAC-128 MIC.
 */
#include <stdio.h>
#include <stdlib.h>

#include "lx_wpa.c"
#include "lx_sae.c"

static unsigned checks, failed;

static void
check(int good, const char *what, int n)
{
	checks++;
	if (!good) {
		failed++;
		printf("FAILED: %s (%d)\n", what, n);
	}
}

static int
hex_is(const unsigned char *bytes, unsigned length, const char *hex)
{
	for (unsigned i = 0; i < length; i++) {
		unsigned value;

		if (sscanf(hex + 2 * i, "%2x", &value) != 1 || value != bytes[i]) {
			return 0;
		}
	}
	return hex[2 * length] == 0;
}

struct sae_case {
#define FIELD(x) const unsigned char *x; size_t x##_length;
	FIELD(password) FIELD(ap) FIELD(station) FIELD(station_random) FIELD(pwe) FIELD(station_commit) FIELD(ap_commit)
	FIELD(kck) FIELD(pmk) FIELD(pmkid) FIELD(station_confirm) FIELD(ap_confirm) FIELD(ap_confirm_wrong) FIELD(snonce)
	FIELD(rsn) FIELD(tk) FIELD(group) FIELD(igtk) FIELD(m1) FIELD(m2) FIELD(m3) FIELD(m4)
#undef FIELD
};

struct psk256_case {
#define FIELD(x) const unsigned char *x; size_t x##_length;
	FIELD(password) FIELD(ssid) FIELD(ap) FIELD(station) FIELD(snonce) FIELD(rsn) FIELD(tk) FIELD(group)
	FIELD(m1) FIELD(m2) FIELD(m3) FIELD(m4)
#undef FIELD
};

#include "sae_cases.inc"

/* the random bytes of the case at hand, in order */
static const unsigned char *random_bytes;
static size_t random_left;

static void
random_from_case(void *out, unsigned length)
{
	if (length > random_left) {
		printf("FAILED: more random bytes asked for than the reference drew\n");
		failed++;
		memset(out, 0x55, length);
		return;
	}
	memcpy(out, random_bytes, length);
	random_bytes += length;
	random_left -= length;
}

static void
test_cmac(void)
{
	/* RFC 4493, 4: the examples */
	static const unsigned char key[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf,
		                               0x4f, 0x3c };
	static const unsigned char message[64] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57,
		0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
		0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b,
		0xe6, 0x6c, 0x37, 0x10,
	};
	static const struct {
		unsigned length;
		const char *mac;
	} examples[] = {
		{ 0, "bb1d6929e95937287fa37d129b756746" }, { 16, "070a16b46b4d4144f79bdd9dd04a287c" },
		{ 40, "dfa66747de9ae63030ca32611497c827" }, { 64, "51f0bebf7e3b9d92fc49741779363cfe" },
	};

	for (unsigned i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
		unsigned char mac[16];
		const unsigned char *pieces[2] = { message, message + examples[i].length / 2 };
		const unsigned lengths[2] = { examples[i].length / 2, examples[i].length - examples[i].length / 2 };

		lx_wpa_cmac(key, pieces, lengths, 2, mac);
		check(hex_is(mac, 16, examples[i].mac), "AES-CMAC, RFC 4493's example", (int)examples[i].length);
	}
}

static void
test_sha256(void)
{
	/* FIPS 180-2's examples, and RFC 4231's first HMAC-SHA256 */
	unsigned char digest[32], key[20];
	const unsigned char *abc[1] = { (const unsigned char *)"abc" };
	const unsigned three[1] = { 3 };
	struct sha256 s;
	const unsigned char *hi[1] = { (const unsigned char *)"Hi There" };
	const unsigned eight[1] = { 8 };

	sha256_begin(&s);
	sha256_more(&s, abc[0], 3);
	sha256_end(&s, digest);
	check(hex_is(digest, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc", 0);
	sha256_begin(&s);
	sha256_more(&s, (const unsigned char *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
	sha256_end(&s, digest);
	check(hex_is(digest, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 of 448 bits", 0);
	memset(key, 0x0b, sizeof(key));
	lx_wpa_hmac_sha256(key, 20, hi, eight, 1, digest);
	check(hex_is(digest, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC-SHA256, RFC 4231", 1);
	(void)abc;
	(void)three;
}

static void
test_michael(void)
{
	/* IEEE 802.11 (and Linux's testmgr): each check is the next one's key */
	static const struct {
		const char *key, *message, *mic;
	} vectors[] = {
		{ "0000000000000000", "", "82925c1ca1d130b8" }, { "82925c1ca1d130b8", "M", "434721ca40639b3f" },
		{ "434721ca40639b3f", "Mi", "e8f9becae97e5d29" }, { "e8f9becae97e5d29", "Mic", "90038fc6cf13c1db" },
		{ "90038fc6cf13c1db", "Mich", "d55e100510128986" }, { "d55e100510128986", "Michael", "0a942b124ecaa546" },
	};

	for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		unsigned char key[8], mic[8];
		struct lx_michael m;
		unsigned length = (unsigned)strlen(vectors[i].message);

		for (unsigned j = 0; j < 8; j++) {
			unsigned v;

			sscanf(vectors[i].key + 2 * j, "%2x", &v);
			key[j] = (unsigned char)v;
		}
		lx_wpa_michael_begin(&m, key);
		lx_wpa_michael_more(&m, (const unsigned char *)vectors[i].message, length);
		lx_wpa_michael_end(&m, mic);
		check(hex_is(mic, 8, vectors[i].mic), "Michael, the standard's example", (int)i);
		/* the same a byte at a time */
		lx_wpa_michael_begin(&m, key);
		for (unsigned j = 0; j < length; j++) {
			lx_wpa_michael_more(&m, (const unsigned char *)vectors[i].message + j, 1);
		}
		lx_wpa_michael_end(&m, mic);
		check(hex_is(mic, 8, vectors[i].mic), "Michael, a byte at a time", (int)i);
	}
}

static void
test_bip(void)
{
	/* BIP-CMAC-128: the MIC of the MMIE, over the AAD (masked frame control, three addresses) and the body */
	unsigned char aad[20], mac[16], body[64];
	unsigned body_length = sizeof(bip_frame) - 24;
	const unsigned char *pieces[2] = { aad, body };
	const unsigned lengths[2] = { 20, body_length };

	memcpy(aad, bip_frame, 2);
	memcpy(aad + 2, bip_frame + 4, 18);
	memcpy(body, bip_frame + 24, body_length);
	memset(body + body_length - 8, 0, 8);
	lx_wpa_cmac(bip_igtk, pieces, lengths, 2, mac);
	check(memcmp(mac, bip_frame + sizeof(bip_frame) - 8, 8) == 0, "BIP-CMAC-128 MIC of a deauthentication", 0);
}

int
main(void)
{
	test_sha256();
	test_cmac();
	test_bip();
	test_michael();
	for (unsigned n = 0; n < sizeof(sae_cases) / sizeof(sae_cases[0]); n++) {
		const struct sae_case *c = &sae_cases[n];
		struct lx_sae sae, other;
		struct lx_wpa wpa;
		unsigned char out[LX_SAE_COMMIT_MOST], reply[LX_WPA_REPLY_MOST], pwe[64], bad[sizeof(off_curve)];
		unsigned length, reply_length;
		const char *why = "";
		int said;

		random_bytes = c->station_random;
		random_left = c->station_random_length;
		check(lx_sae_prepare(&sae, c->password, (unsigned)c->password_length, c->station, c->ap, random_from_case) == 0,
		    "SAE: a password element is found", (int)n);
		for (unsigned i = 0; i < 4; i++) {
			for (unsigned j = 0; j < 8; j++) {
				pwe[31 - 8 * i - j] = (unsigned char)(sae.pwe[i] >> (8 * j));
				pwe[63 - 8 * i - j] = (unsigned char)(sae.pwe[4 + i] >> (8 * j));
			}
		}
		check(memcmp(pwe, c->pwe, 64) == 0, "SAE: the password element is the reference's", (int)n);
		check(random_left == 0, "SAE: as many random bytes taken as the reference drew", (int)n);
		length = lx_sae_commit(&sae, NULL, 0, out);
		check(length == c->station_commit_length && memcmp(out, c->station_commit, length) == 0, "SAE: our commit", (int)n);
		length = lx_sae_commit(&sae, (const unsigned char *)"token", 5, out);
		check(length == c->station_commit_length + 5 && memcmp(out + 2, "token", 5) == 0, "SAE: a commit with a token",
		    (int)n);

		/* refusals: a point off the curve, a scalar of 0, our own commit sent back */
		other = sae;
		check(lx_sae_peer_commit(&other, off_curve, sizeof(off_curve), &why) == -1, "SAE: an element off the curve", (int)n);
		memcpy(bad, c->ap_commit, sizeof(bad));
		memset(bad + 2, 0, 32);
		other = sae;
		check(lx_sae_peer_commit(&other, bad, sizeof(bad), &why) == -1, "SAE: a scalar of 0", (int)n);
		other = sae;
		check(lx_sae_peer_commit(&other, c->station_commit, (unsigned)c->station_commit_length, &why) == -1,
		    "SAE: a reflected commit", (int)n);

		check(lx_sae_peer_commit(&sae, c->ap_commit, (unsigned)c->ap_commit_length, &why) == 0, "SAE: the access point's "
		    "commit is taken", (int)n);
		check(memcmp(sae.kck, c->kck, 32) == 0 && memcmp(sae.pmk, c->pmk, 32) == 0 && memcmp(sae.pmkid, c->pmkid, 16) == 0,
		    "SAE: KCK, PMK and PMKID are the reference's", (int)n);
		length = lx_sae_confirm(&sae, out);
		check(length == c->station_confirm_length && memcmp(out, c->station_confirm, length) == 0, "SAE: our confirm",
		    (int)n);
		check(lx_sae_peer_confirm(&sae, c->ap_confirm, (unsigned)c->ap_confirm_length, &why) == 0,
		    "SAE: the access point's confirm proves the key", (int)n);
		check(lx_sae_peer_confirm(&sae, c->ap_confirm_wrong, (unsigned)c->ap_confirm_wrong_length, &why) == -1,
		    "SAE: a confirm of another key is refused", (int)n);

		/* the four-way handshake of AKM 8 with that PMK */
		memset(&wpa, 0, sizeof(wpa));
		wpa.akm = LX_WPA_AKM_SAE;
		memcpy(wpa.pmk, sae.pmk, 32);
		memcpy(wpa.own, c->station, 6);
		memcpy(wpa.peer, c->ap, 6);
		memcpy(wpa.own_nonce, c->snonce, 32);
		wpa.rsn = c->rsn;
		wpa.rsn_length = (unsigned)c->rsn_length;
		said = lx_wpa_message(&wpa, c->m1, (unsigned)c->m1_length, reply, &reply_length, &why);
		check(said == LX_WPA_ANSWER && reply_length == c->m2_length && memcmp(reply, c->m2, reply_length) == 0,
		    "AKM 8: message 2", (int)n);
		said = lx_wpa_message(&wpa, c->m3, (unsigned)c->m3_length, reply, &reply_length, &why);
		check(said == LX_WPA_KEYS, "AKM 8: message 3 is taken", (int)n);
		if (said != LX_WPA_KEYS) {
			printf("  (%s)\n", why);
		}
		check(reply_length == c->m4_length && memcmp(reply, c->m4, reply_length) == 0, "AKM 8: message 4", (int)n);
		check(memcmp(wpa.pairwise, c->tk, 16) == 0 && wpa.group_length == 16 && memcmp(wpa.group, c->group, 16) == 0,
		    "AKM 8: the pairwise and group keys", (int)n);
		check(wpa.have_igtk && wpa.igtk_index == 4 && memcmp(wpa.igtk_counter, c->igtk + 2, 6) == 0 &&
		    memcmp(wpa.igtk, c->igtk + 8, 16) == 0, "AKM 8: the group's management key (IGTK)", (int)n);

		/* downgrade protection: message 3's RSN element must be the beacon's, and its RSNXE too */
		{
			static const unsigned char rsnxe[3] = { 244, 1, 0x20 };
			unsigned char other_rsn[64];
			int cases[3] = { 0, 1, 2 };		/* the same; another RSN in the beacon; an RSNXE in the beacon only */

			for (unsigned k = 0; k < 3; k++) {
				memset(&wpa, 0, sizeof(wpa));
				wpa.akm = LX_WPA_AKM_SAE;
				memcpy(wpa.pmk, sae.pmk, 32);
				memcpy(wpa.own, c->station, 6);
				memcpy(wpa.peer, c->ap, 6);
				memcpy(wpa.own_nonce, c->snonce, 32);
				wpa.rsn = c->rsn;
				wpa.rsn_length = (unsigned)c->rsn_length;
				memcpy(other_rsn, c->rsn, c->rsn_length);
				other_rsn[c->rsn_length - 7] ^= 0x01;	/* (a pairwise cipher's kind, changed) */
				wpa.peer_rsn = cases[k] == 1 ? other_rsn : c->rsn;
				wpa.peer_rsn_length = (unsigned)c->rsn_length;
				wpa.peer_rsnxe = cases[k] == 2 ? rsnxe : NULL;
				wpa.peer_rsnxe_length = cases[k] == 2 ? 3 : 0;
				lx_wpa_message(&wpa, c->m1, (unsigned)c->m1_length, reply, &reply_length, &why);
				said = lx_wpa_message(&wpa, c->m3, (unsigned)c->m3_length, reply, &reply_length, &why);
				check(cases[k] == 0 ? said == LX_WPA_KEYS : said == LX_WPA_BAD,
				    k == 0 ? "message 3: the beacon's RSN element" : k == 1 ? "message 3: another RSN element, refused" :
				    "message 3: no RSNXE where the beacon had one, refused", (int)n);
			}
		}
	}
	/* IEEE 802.11-2020, Annex J.10 (as wpa_supplicant's src/common/common_module_tests.c has it) */
	{
		static const unsigned char addr1[6] = { 0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87 }, addr2[6] = { 0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c };
		static const unsigned char rand_mask[64] = {
			0x99, 0x24, 0x65, 0xfd, 0x3d, 0xaa, 0x3c, 0x60, 0xaa, 0x65, 0x65, 0xb7, 0xf6, 0x2a, 0x2a, 0x7f,
			0x2e, 0x12, 0xdd, 0x12, 0xf1, 0x98, 0xfa, 0xf4, 0xfb, 0xed, 0x89, 0xd7, 0xff, 0x1a, 0xce, 0x94,
			0x95, 0x07, 0xa9, 0x0f, 0x77, 0x7a, 0x04, 0x4d, 0x6a, 0x08, 0x30, 0xb9, 0x1e, 0xa3, 0xd5, 0xdd,
			0x70, 0xbe, 0xce, 0x44, 0xe1, 0xac, 0xff, 0xb8, 0x69, 0x83, 0xb5, 0xe1, 0xbf, 0x9f, 0xb3, 0x22 };
		static const char *local_commit = "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c65d5ad9e008297"
		    "07aa36ba8b859738fc961d08243505f47c035376d7ac4bc8d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325dbe1";
		static const unsigned char peer_commit[] = {
			0x13, 0x00, 0x59, 0x1b, 0x96, 0xf3, 0x39, 0x7f, 0xb9, 0x45, 0x10, 0x08, 0x48, 0xe7, 0xb5, 0x50,
			0x54, 0x3b, 0x67, 0x20, 0xd8, 0x83, 0x37, 0xee, 0x93, 0xfc, 0x49, 0xfd, 0x6d, 0xf7, 0xe0, 0x8b,
			0x52, 0x23, 0xe7, 0x1b, 0x9b, 0xb0, 0x48, 0xd3, 0x87, 0x3f, 0x20, 0x55, 0x69, 0x53, 0xa9, 0x6c,
			0x91, 0x53, 0x6f, 0xd8, 0xee, 0x6c, 0xa9, 0xb4, 0xa6, 0x8a, 0x14, 0x8b, 0x05, 0x6a, 0x90, 0x9b,
			0xe0, 0x3e, 0x83, 0xae, 0x20, 0x8f, 0x60, 0xf8, 0xef, 0x55, 0x37, 0x85, 0x80, 0x74, 0xdb, 0x06,
			0x68, 0x70, 0x32, 0x39, 0x98, 0x62, 0x99, 0x9b, 0x51, 0x1e, 0x0a, 0x15, 0x52, 0xa5, 0xfe, 0xa3,
			0x17, 0xc2 };
		static const unsigned char addr1b[6] = { 0x00, 0x09, 0x5b, 0x66, 0xec, 0x1e }, addr2b[6] = { 0x00, 0x0b, 0x6b, 0xd9, 0x02, 0x46 };
		struct lx_sae sae;
		unsigned char out[LX_SAE_COMMIT_MOST], pt[64], pwe[64];
		const char *why = "";
		unsigned length;

		random_bytes = rand_mask;
		random_left = sizeof(rand_mask);
		check(lx_sae_prepare(&sae, (const unsigned char *)"mekmitasdigoat", 14, addr1, addr2, random_from_case) == 0,
		    "Annex J.10: hunting and pecking", 0);
		length = lx_sae_commit(&sae, NULL, 0, out);
		check(length == 98 && hex_is(out, 98, local_commit), "Annex J.10: our commit", 0);
		check(lx_sae_peer_commit(&sae, peer_commit + 0, sizeof(peer_commit), &why) == 0 &&
		    hex_is(sae.kck, 32, "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a") &&
		    hex_is(sae.pmk, 32, "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59") &&
		    hex_is(sae.pmkid, 16, "8747a600eea3f9f22475df58ca1e5498"), "Annex J.10: KCK, PMK and PMKID", 0);
		/* hash to element: PT of the name, the passphrase and a password identifier; the PWE of two addresses */
		check(lx_sae_pt(pt, (const unsigned char *)"mekmitasdigoat", 14, (const unsigned char *)"byteme", 6,
		    (const unsigned char *)"psk4internet", 12) == 0, "Annex J.10: H2E's PT", 0);
		random_bytes = rand_mask;
		random_left = sizeof(rand_mask);
		check(lx_sae_prepare_pt(&sae, pt, addr1b, addr2b, random_from_case) == 0 && sae.h2e, "Annex J.10: H2E's PWE", 0);
		for (unsigned i = 0; i < 4; i++) {
			for (unsigned j = 0; j < 8; j++) {
				pwe[31 - 8 * i - j] = (unsigned char)(sae.pwe[i] >> (8 * j));
				pwe[63 - 8 * i - j] = (unsigned char)(sae.pwe[4 + i] >> (8 * j));
			}
		}
		check(hex_is(pwe, 32, "c93049b9e64000f848201649e999f2b5c22dea69b5632c9df4d633b8aa1f6c1e") &&
		    hex_is(pwe + 32, 32, "73634e94b53d82e7383a8d258199d9dc1a5ee8269d060382ccbf33e614ff59a0"),
		    "Annex J.10: H2E's PWE is the standard's", 0);
	}

	/* PSK-SHA256 (AKM 6): descriptor version 3, AES-CMAC, KDF-SHA-256 */
	for (unsigned n = 0; n < sizeof(psk256_cases) / sizeof(psk256_cases[0]); n++) {
		const struct psk256_case *c = &psk256_cases[n];
		unsigned char reply[LX_WPA_REPLY_MOST];
		unsigned reply_length;
		struct lx_wpa wpa;
		const char *why = "";
		int said;

		memset(&wpa, 0, sizeof(wpa));
		wpa.akm = LX_WPA_AKM_PSK_SHA256;
		lx_wpa_pmk(c->password, (unsigned)c->password_length, c->ssid, (unsigned)c->ssid_length, wpa.pmk);
		memcpy(wpa.own, c->station, 6);
		memcpy(wpa.peer, c->ap, 6);
		memcpy(wpa.own_nonce, c->snonce, 32);
		wpa.rsn = c->rsn;
		wpa.rsn_length = (unsigned)c->rsn_length;
		said = lx_wpa_message(&wpa, c->m1, (unsigned)c->m1_length, reply, &reply_length, &why);
		check(said == LX_WPA_ANSWER && reply_length == c->m2_length && memcmp(reply, c->m2, reply_length) == 0,
		    "AKM 6: message 2", (int)n);
		said = lx_wpa_message(&wpa, c->m3, (unsigned)c->m3_length, reply, &reply_length, &why);
		check(said == LX_WPA_KEYS && reply_length == c->m4_length && memcmp(reply, c->m4, reply_length) == 0,
		    "AKM 6: message 3 taken, message 4", (int)n);
		if (said != LX_WPA_KEYS) {
			printf("  (%s)\n", why);
		}
		check(memcmp(wpa.pairwise, c->tk, 16) == 0 && wpa.group_length == 16 && memcmp(wpa.group, c->group, 16) == 0,
		    "AKM 6: the pairwise and group keys", (int)n);
		/* a MIC failure's report: a request with "error", our counter, signed with the KCK (descriptor version 3) */
		check(lx_wpa_mic_failure_report(&wpa, 1, reply, &reply_length) == 0 && reply_length == 99 &&
		    (reply[5] << 8 | reply[6]) == (0x0400 | 0x0800 | 0x0200 | 0x0100 | 0x0008 | 3) && reply[16] == 1,
		    "AKM 6: a MIC failure's report", (int)n);
		{
			unsigned char mic[16];
			unsigned char copy[99];

			memcpy(copy, reply, 99);
			memset(copy + 81, 0, 16);
			{
				const unsigned char *pieces[1] = { copy };
				const unsigned lengths[1] = { 99 };

				lx_wpa_cmac(wpa.kck, pieces, lengths, 1, mic);
			}
			check(memcmp(mic, reply + 81, 16) == 0, "AKM 6: the report's MIC", (int)n);
		}
		check(lx_wpa_mic_failure_report(&wpa, 0, reply, &reply_length) == 0 && reply[16] == 2 && !(reply[6] & 0x08),
		    "AKM 6: a second report, of the group: the counter goes on", (int)n);
	}
	printf("%u checks, %u failed\n", checks, failed);
	return failed != 0;
}
