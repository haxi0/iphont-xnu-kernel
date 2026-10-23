/* ------------------------------------------------------------------------ */
/* SHA-256, SHA-512 and Ed25519 signature checking                          */
/*
 * Included by loader.c (and by tools/crypto-test.c, which checks it against Python's `cryptography` on
 * the computer). Freestanding: no allocation, no tables bigger than the round constants, no secrets --
 * the loader only ever checks, so nothing here has to run in constant time.
 *
 * The Ed25519 part follows TweetNaCl (Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers; public
 * domain): field elements of sixteen 16-bit limbs in int64_t, the extended twisted Edwards group law,
 * and the reduction modulo the group order L. Only `crypto_sign_open`'s check survives: an owner
 * signs a boot bundle on the computer (common/flash/ownerkey.py), the loader checks it here.
 */

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

struct sha256 {
	uint32_t h[8];
	uint8_t buf[64];
	uint64_t len;
};

static void
sha256_block(struct sha256 *s, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
		uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void
sha256_init(struct sha256 *s)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
}

static void
sha256_add(struct sha256 *s, const void *data, uint64_t n)
{
	const uint8_t *p = data;
	uint64_t have = s->len % 64;

	s->len += n;
	if (have) {
		uint64_t take = 64 - have < n ? 64 - have : n;
		memcpy(s->buf + have, p, take);
		p += take; n -= take;
		if (have + take < 64) {
			return;
		}
		sha256_block(s, s->buf);
	}
	for (; n >= 64; p += 64, n -= 64) {
		sha256_block(s, p);
	}
	memcpy(s->buf, p, n);
}

static void
sha256_end(struct sha256 *s, uint8_t out[32])
{
	uint64_t bits = s->len * 8, have = s->len % 64;

	s->buf[have++] = 0x80;
	if (have > 56) {
		memset(s->buf + have, 0, 64 - have);
		sha256_block(s, s->buf);
		have = 0;
	}
	memset(s->buf + have, 0, 56 - have);
	for (int i = 0; i < 8; i++) {
		s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha256_block(s, s->buf);
	for (int i = 0; i < 32; i++) {
		out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
	}
}

static void
sha256(const void *data, uint64_t n, uint8_t out[32])
{
	struct sha256 s;

	sha256_init(&s);
	sha256_add(&s, data, n);
	sha256_end(&s, out);
}

/* ---- SHA-512 (for Ed25519) ----------------------------------------------- */

static const uint64_t sha512_k[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void
sha512_block(uint64_t st[8], const uint8_t *p)
{
	uint64_t w[80], v[8];

	for (int i = 0; i < 16; i++) {
		w[i] = 0;
		for (int k = 0; k < 8; k++) {
			w[i] = w[i] << 8 | p[8 * i + k];
		}
	}
	for (int i = 16; i < 80; i++) {
		uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
		uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(v, st, sizeof(v));
	for (int i = 0; i < 80; i++) {
		uint64_t t1 = v[7] + (ROR64(v[4], 14) ^ ROR64(v[4], 18) ^ ROR64(v[4], 41)) +
		    ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha512_k[i] + w[i];
		uint64_t t2 = (ROR64(v[0], 28) ^ ROR64(v[0], 34) ^ ROR64(v[0], 39)) +
		    ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
		v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++) {
		st[i] += v[i];
	}
}

/* SHA-512 of the concatenation of up to three pieces (all Ed25519 needs: R || A || M) */
static void
sha512_3(uint8_t out[64], const uint8_t *a, uint64_t na, const uint8_t *b, uint64_t nb, const uint8_t *c, uint64_t nc)
{
	uint64_t st[8] = {
		0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
		0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
	};
	const uint8_t *part[3] = { a, b, c };
	uint64_t len[3] = { na, nb, nc }, have = 0, total = na + nb + nc;
	uint8_t buf[128];

	for (int k = 0; k < 3; k++) {
		for (uint64_t i = 0; i < len[k]; i++) {
			buf[have++] = part[k][i];
			if (have == 128) {
				sha512_block(st, buf);
				have = 0;
			}
		}
	}
	buf[have++] = 0x80;
	if (have > 112) {
		memset(buf + have, 0, 128 - have);
		sha512_block(st, buf);
		have = 0;
	}
	memset(buf + have, 0, 128 - have);
	for (int i = 0; i < 8; i++) {
		buf[120 + i] = (uint8_t)((total * 8) >> (56 - 8 * i));
	}
	sha512_block(st, buf);
	for (int i = 0; i < 64; i++) {
		out[i] = (uint8_t)(st[i / 8] >> (56 - 8 * (i % 8)));
	}
}

/* ---- Ed25519 (after TweetNaCl) ------------------------------------------- */

typedef int64_t gf[16];

static const gf gf0 = { 0 }, gf1 = { 1 };
static const gf ed_D = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
	0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
static const gf ed_D2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
	0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
static const gf ed_X = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
	0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const gf ed_Y = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
	0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
static const gf ed_I = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
	0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };

static void
gf_set(gf r, const gf a)
{
	for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void
gf_car(gf o)
{
	for (int i = 0; i < 16; i++) {
		int64_t c;
		o[i] += (int64_t)1 << 16;
		c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c * ((int64_t)1 << 16);
	}
}

static void
gf_sel(gf p, gf q, int b)
{
	int64_t c = ~(b - 1);
	for (int i = 0; i < 16; i++) {
		int64_t t = c & (p[i] ^ q[i]);
		p[i] ^= t; q[i] ^= t;
	}
}

static void
gf_pack(uint8_t *o, const gf n)
{
	gf m, t;
	int b;

	gf_set(t, n);
	gf_car(t); gf_car(t); gf_car(t);
	for (int j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (int i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (int)((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		gf_sel(t, m, 1 - b);
	}
	for (int i = 0; i < 16; i++) {
		o[2 * i] = (uint8_t)(t[i] & 0xff);
		o[2 * i + 1] = (uint8_t)(t[i] >> 8);
	}
}

static int
gf_neq(const gf a, const gf b)
{
	uint8_t c[32], d[32];
	gf_pack(c, a); gf_pack(d, b);
	return memcmp(c, d, 32) != 0;
}

static uint8_t
gf_par(const gf a)
{
	uint8_t d[32];
	gf_pack(d, a);
	return d[0] & 1;
}

static void
gf_unpack(gf o, const uint8_t *n)
{
	for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	o[15] &= 0x7fff;
}

static void gf_A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void gf_Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void
gf_M(gf o, const gf a, const gf b)
{
	int64_t t[31];

	for (int i = 0; i < 31; i++) t[i] = 0;
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
	for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
	for (int i = 0; i < 16; i++) o[i] = t[i];
	gf_car(o); gf_car(o);
}

static void gf_S(gf o, const gf a) { gf_M(o, a, a); }

static void
gf_inv(gf o, const gf i)
{
	gf c;
	gf_set(c, i);
	for (int a = 253; a >= 0; a--) {
		gf_S(c, c);
		if (a != 2 && a != 4) gf_M(c, c, i);
	}
	gf_set(o, c);
}

static void
gf_pow2523(gf o, const gf i)
{
	gf c;
	gf_set(c, i);
	for (int a = 250; a >= 0; a--) {
		gf_S(c, c);
		if (a != 1) gf_M(c, c, i);
	}
	gf_set(o, c);
}

static void
ed_add(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;

	gf_Z(a, p[1], p[0]); gf_Z(t, q[1], q[0]); gf_M(a, a, t);
	gf_A(b, p[0], p[1]); gf_A(t, q[0], q[1]); gf_M(b, b, t);
	gf_M(c, p[3], q[3]); gf_M(c, c, ed_D2);
	gf_M(d, p[2], q[2]); gf_A(d, d, d);
	gf_Z(e, b, a); gf_Z(f, d, c); gf_A(g, d, c); gf_A(h, b, a);
	gf_M(p[0], e, f); gf_M(p[1], h, g); gf_M(p[2], g, f); gf_M(p[3], e, h);
}

static void
ed_cswap(gf p[4], gf q[4], uint8_t b)
{
	for (int i = 0; i < 4; i++) gf_sel(p[i], q[i], b);
}

static void
ed_pack(uint8_t *r, gf p[4])
{
	gf tx, ty, zi;
	gf_inv(zi, p[2]);
	gf_M(tx, p[0], zi);
	gf_M(ty, p[1], zi);
	gf_pack(r, ty);
	r[31] ^= gf_par(tx) << 7;
}

static void
ed_scalarmult(gf p[4], gf q[4], const uint8_t *s)
{
	gf_set(p[0], gf0); gf_set(p[1], gf1); gf_set(p[2], gf1); gf_set(p[3], gf0);
	for (int i = 255; i >= 0; i--) {
		uint8_t b = (s[i / 8] >> (i & 7)) & 1;
		ed_cswap(p, q, b);
		ed_add(q, p);
		ed_add(p, p);
		ed_cswap(p, q, b);
	}
}

static void
ed_scalarbase(gf p[4], const uint8_t *s)
{
	gf q[4];
	gf_set(q[0], ed_X); gf_set(q[1], ed_Y); gf_set(q[2], gf1); gf_M(q[3], ed_X, ed_Y);
	ed_scalarmult(p, q, s);
}

static const uint64_t ed_L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

static void
ed_modL(uint8_t *r, int64_t x[64])
{
	int64_t carry;
	int i, j;

	for (i = 63; i >= 32; --i) {
		carry = 0;
		for (j = i - 32; j < i - 12; ++j) {
			x[j] += carry - 16 * x[i] * (int64_t)ed_L[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry * 256;
		}
		x[j] += carry;
		x[i] = 0;
	}
	carry = 0;
	for (j = 0; j < 32; j++) {
		x[j] += carry - (x[31] >> 4) * (int64_t)ed_L[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; j++) x[j] -= carry * (int64_t)ed_L[j];
	for (i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = (uint8_t)(x[i] & 255);
	}
}

static void
ed_reduce(uint8_t *r)
{
	int64_t x[64];
	for (int i = 0; i < 64; i++) x[i] = (uint64_t)r[i];
	for (int i = 0; i < 64; i++) r[i] = 0;
	ed_modL(r, x);
}

/* -A from its 32 bytes; 0 when the bytes are not a point */
static int
ed_unpackneg(gf r[4], const uint8_t p[32])
{
	gf t, chk, num, den, den2, den4, den6;

	gf_set(r[2], gf1);
	gf_unpack(r[1], p);
	gf_S(num, r[1]);
	gf_M(den, num, ed_D);
	gf_Z(num, num, r[2]);
	gf_A(den, r[2], den);
	gf_S(den2, den); gf_S(den4, den2); gf_M(den6, den4, den2);
	gf_M(t, den6, num); gf_M(t, t, den);
	gf_pow2523(t, t);
	gf_M(t, t, num); gf_M(t, t, den); gf_M(t, t, den); gf_M(r[0], t, den);
	gf_S(chk, r[0]); gf_M(chk, chk, den);
	if (gf_neq(chk, num)) gf_M(r[0], r[0], ed_I);
	gf_S(chk, r[0]); gf_M(chk, chk, den);
	if (gf_neq(chk, num)) return 0;
	if (gf_par(r[0]) == (p[31] >> 7)) gf_Z(r[0], gf0, r[0]);
	gf_M(r[3], r[0], r[1]);
	return 1;
}

/* Is sig (R || S, 64 bytes) a signature of msg by the key pk? 1 yes, 0 no. */
static int
ed25519_verify(const uint8_t sig[64], const uint8_t *msg, uint64_t n, const uint8_t pk[32])
{
	uint8_t h[64], t[32];
	gf p[4], q[4];

	/* S must be below L (no malleable signatures): compare from the top byte down */
	for (int i = 31; i >= 0; i--) {
		if (sig[32 + i] < ed_L[i]) break;
		if (sig[32 + i] > ed_L[i] || i == 0) return 0;
	}
	if (!ed_unpackneg(q, pk)) return 0;
	sha512_3(h, sig, 32, pk, 32, msg, n);
	ed_reduce(h);
	ed_scalarmult(p, q, h);
	ed_scalarbase(q, sig + 32);
	ed_add(p, q);
	ed_pack(t, p);
	return memcmp(sig, t, 32) == 0;
}
