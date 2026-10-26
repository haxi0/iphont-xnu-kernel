/*
 * SAE with group 19 (lx_sae.h): the NIST P-256 curve's arithmetic, then the protocol.
 *
 * Numbers are four 64-bit words, least significant first. Arithmetic modulo the field's prime p
 * and the group's order n is Montgomery's (a number x is kept as x R mod m, R = 2^256), points
 * are Jacobian (X, Y, Z: x = X / Z^2, y = Y / Z^3), and Z = 0 is the point at infinity. A scalar
 * multiplication adds at every bit and keeps the sum or not by a mask: no branch on a secret's
 * bits. Nothing here is written for speed.
 */
#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <string.h>
#endif

#include "lx_wpa.h"
#include "lx_sae.h"

typedef unsigned __int128 wide;
typedef struct {
	uint64_t v[4];
} num;

struct field {
	num m;
	uint64_t minv;				/* -m^-1 modulo 2^64 */
	num rr;					/* R^2 mod m */
	num one;				/* R mod m: 1, as it is kept */
};

static const num p_256 = { { 0xffffffffffffffffull, 0x00000000ffffffffull, 0x0000000000000000ull, 0xffffffff00000001ull } };
static const num b_256 = { { 0x3bce3c3e27d2604bull, 0x651d06b0cc53b0f6ull, 0xb3ebbd55769886bcull, 0x5ac635d8aa3a93e7ull } };
static const num n_256 = { { 0xf3b9cac2fc632551ull, 0xbce6faada7179e84ull, 0xffffffffffffffffull, 0xffffffff00000000ull } };

/* ---- numbers ------------------------------------------------------------------------------------- */

static uint64_t
add(num *r, const num *a, const num *b)
{
	uint64_t carry = 0;

	for (unsigned i = 0; i < 4; i++) {
		wide x = (wide)a->v[i] + b->v[i] + carry;

		r->v[i] = (uint64_t)x;
		carry = (uint64_t)(x >> 64);
	}
	return (carry);
}

static uint64_t
sub(num *r, const num *a, const num *b)
{
	uint64_t borrow = 0;

	for (unsigned i = 0; i < 4; i++) {
		wide x = (wide)a->v[i] - b->v[i] - borrow;

		r->v[i] = (uint64_t)x;
		borrow = (uint64_t)(x >> 64) & 1;
	}
	return (borrow);
}

static int
compare(const num *a, const num *b)
{
	for (int i = 3; i >= 0; i--) {
		if (a->v[i] != b->v[i]) {
			return (a->v[i] < b->v[i] ? -1 : 1);
		}
	}
	return (0);
}

static int
is_zero(const num *a)
{
	return ((a->v[0] | a->v[1] | a->v[2] | a->v[3]) == 0);
}

static num
small(uint64_t x)
{
	num r = { { x, 0, 0, 0 } };

	return (r);
}

static num
from_bytes(const uint8_t b[32])
{
	num r;

	for (unsigned i = 0; i < 4; i++) {
		r.v[i] = 0;
		for (unsigned j = 0; j < 8; j++) {
			r.v[i] |= (uint64_t)b[31 - 8 * i - j] << (8 * j);
		}
	}
	return (r);
}

static void
to_bytes(const num *a, uint8_t b[32])
{
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned j = 0; j < 8; j++) {
			b[31 - 8 * i - j] = (uint8_t)(a->v[i] >> (8 * j));
		}
	}
}

static num
shift_right(const num *a, unsigned bits)	/* (bits < 64) */
{
	num r;

	for (unsigned i = 0; i < 4; i++) {
		r.v[i] = a->v[i] >> bits | (i < 3 && bits != 0 ? a->v[i + 1] << (64 - bits) : 0);
	}
	return (r);
}

/* ---- modular arithmetic ------------------------------------------------------------------------- */

static num
mod_add(const struct field *f, const num *a, const num *b)
{
	num r, t;
	uint64_t carry = add(&r, a, b);

	if (carry || compare(&r, &f->m) >= 0) {
		sub(&t, &r, &f->m);
		r = t;
	}
	return (r);
}

static num
mod_sub(const struct field *f, const num *a, const num *b)
{
	num r, t;

	if (sub(&r, a, b)) {
		add(&t, &r, &f->m);
		r = t;
	}
	return (r);
}

/* a b R^-1 mod m (Montgomery's multiplication, word by word) */
static num
mul(const struct field *f, const num *a, const num *b)
{
	uint64_t t[6] = { 0 };
	num r, s;

	for (unsigned i = 0; i < 4; i++) {
		uint64_t carry = 0, q;
		wide x;

		for (unsigned j = 0; j < 4; j++) {
			x = (wide)a->v[i] * b->v[j] + t[j] + carry;
			t[j] = (uint64_t)x;
			carry = (uint64_t)(x >> 64);
		}
		x = (wide)t[4] + carry;
		t[4] = (uint64_t)x;
		t[5] = (uint64_t)(x >> 64);
		q = t[0] * f->minv;
		x = (wide)q * f->m.v[0] + t[0];
		carry = (uint64_t)(x >> 64);
		for (unsigned j = 1; j < 4; j++) {
			x = (wide)q * f->m.v[j] + t[j] + carry;
			t[j - 1] = (uint64_t)x;
			carry = (uint64_t)(x >> 64);
		}
		x = (wide)t[4] + carry;
		t[3] = (uint64_t)x;
		t[4] = t[5] + (uint64_t)(x >> 64);
	}
	memcpy(r.v, t, sizeof(r.v));
	if (t[4] != 0 || compare(&r, &f->m) >= 0) {
		sub(&s, &r, &f->m);
		r = s;
	}
	return (r);
}

static num
to_field(const struct field *f, const num *a)
{
	return (mul(f, a, &f->rr));
}

static num
from_field(const struct field *f, const num *a)
{
	num one = small(1);

	return (mul(f, a, &one));
}

/* a^e, a as it is kept, e a plain number */
static num
power(const struct field *f, const num *a, const num *e)
{
	num r = f->one;

	for (int bit = 255; bit >= 0; bit--) {
		r = mul(f, &r, &r);
		if (e->v[bit / 64] >> (bit % 64) & 1) {
			r = mul(f, &r, a);
		}
	}
	return (r);
}

static void
field_init(struct field *f, const num *m)
{
	uint64_t inverse = m->v[0];
	num x = small(1);

	f->m = *m;
	for (unsigned i = 0; i < 5; i++) {
		inverse *= 2 - m->v[0] * inverse;	/* (Newton: the bits right double every time) */
	}
	f->minv = (uint64_t)0 - inverse;
	for (unsigned i = 0; i < 512; i++) {
		x = mod_add(f, &x, &x);
	}
	f->rr = x;
	x = small(1);
	f->one = to_field(f, &x);
}

/* ---- the curve ------------------------------------------------------------------------------------ */

struct curve {
	struct field p;
	num b;					/* as kept */
	num p_minus_2, p_minus_1_half, p_plus_1_quarter;
};

struct point {
	num x, y, z;				/* Jacobian, as kept; z = 0: infinity */
};

static void
curve_init(struct curve *c)
{
	num two = small(2), one = small(1), t;

	field_init(&c->p, &p_256);
	c->b = to_field(&c->p, &b_256);
	sub(&c->p_minus_2, &p_256, &two);
	c->p_minus_1_half = shift_right(&p_256, 1);
	add(&t, &p_256, &one);
	c->p_plus_1_quarter = shift_right(&t, 2);
}

static num
twice(const struct curve *c, const num *a)
{
	return (mod_add(&c->p, a, a));
}

static struct point
point_double(const struct curve *c, const struct point *a)
{
	const struct field *f = &c->p;
	struct point r;
	num delta, gamma, beta, alpha, t, u, beta4, gamma2;

	if (is_zero(&a->z) || is_zero(&a->y)) {
		r.x = f->one;
		r.y = f->one;
		r.z = small(0);
		return (r);
	}
	delta = mul(f, &a->z, &a->z);
	gamma = mul(f, &a->y, &a->y);
	beta = mul(f, &a->x, &gamma);
	t = mod_sub(f, &a->x, &delta);
	u = mod_add(f, &a->x, &delta);
	t = mul(f, &t, &u);
	alpha = mod_add(f, &t, &t);
	alpha = mod_add(f, &alpha, &t);			/* 3 (x - delta)(x + delta), a = -3 */
	beta4 = twice(c, &beta);
	beta4 = twice(c, &beta4);
	r.x = mul(f, &alpha, &alpha);
	t = twice(c, &beta4);
	r.x = mod_sub(f, &r.x, &t);			/* alpha^2 - 8 beta */
	t = mod_add(f, &a->y, &a->z);
	t = mul(f, &t, &t);
	t = mod_sub(f, &t, &gamma);
	r.z = mod_sub(f, &t, &delta);
	t = mod_sub(f, &beta4, &r.x);
	r.y = mul(f, &alpha, &t);
	gamma2 = mul(f, &gamma, &gamma);
	gamma2 = twice(c, &gamma2);
	gamma2 = twice(c, &gamma2);
	gamma2 = twice(c, &gamma2);
	r.y = mod_sub(f, &r.y, &gamma2);		/* alpha (4 beta - x3) - 8 gamma^2 */
	return (r);
}

static struct point
point_add(const struct curve *c, const struct point *a, const struct point *b)
{
	const struct field *f = &c->p;
	struct point r;
	num z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;

	if (is_zero(&a->z)) {
		return (*b);
	}
	if (is_zero(&b->z)) {
		return (*a);
	}
	z1z1 = mul(f, &a->z, &a->z);
	z2z2 = mul(f, &b->z, &b->z);
	u1 = mul(f, &a->x, &z2z2);
	u2 = mul(f, &b->x, &z1z1);
	t = mul(f, &b->z, &z2z2);
	s1 = mul(f, &a->y, &t);
	t = mul(f, &a->z, &z1z1);
	s2 = mul(f, &b->y, &t);
	h = mod_sub(f, &u2, &u1);
	rr = mod_sub(f, &s2, &s1);
	if (is_zero(&h)) {
		if (is_zero(&rr)) {
			return (point_double(c, a));
		}
		r.x = f->one;
		r.y = f->one;
		r.z = small(0);
		return (r);
	}
	rr = twice(c, &rr);
	i = twice(c, &h);
	i = mul(f, &i, &i);
	j = mul(f, &h, &i);
	v = mul(f, &u1, &i);
	r.x = mul(f, &rr, &rr);
	r.x = mod_sub(f, &r.x, &j);
	t = twice(c, &v);
	r.x = mod_sub(f, &r.x, &t);
	t = mod_sub(f, &v, &r.x);
	r.y = mul(f, &rr, &t);
	t = mul(f, &s1, &j);
	t = twice(c, &t);
	r.y = mod_sub(f, &r.y, &t);
	t = mod_add(f, &a->z, &b->z);
	t = mul(f, &t, &t);
	t = mod_sub(f, &t, &z1z1);
	t = mod_sub(f, &t, &z2z2);
	r.z = mul(f, &t, &h);
	return (r);
}

static void
select_point(struct point *r, const struct point *a, uint64_t mask)	/* r = mask ? a : r, mask all ones or zero */
{
	for (unsigned w = 0; w < 4; w++) {
		r->x.v[w] = (r->x.v[w] & ~mask) | (a->x.v[w] & mask);
		r->y.v[w] = (r->y.v[w] & ~mask) | (a->y.v[w] & mask);
		r->z.v[w] = (r->z.v[w] & ~mask) | (a->z.v[w] & mask);
	}
}

static struct point
point_times(const struct curve *c, const num *k, const struct point *a)
{
	struct point r = { c->p.one, c->p.one, small(0) }, t;

	for (int bit = 255; bit >= 0; bit--) {
		r = point_double(c, &r);
		t = point_add(c, &r, a);
		select_point(&r, &t, (uint64_t)0 - (k->v[bit / 64] >> (bit % 64) & 1));
	}
	return (r);
}

/* A point from plain coordinates; and back (false: infinity). */
static struct point
point_of(const struct curve *c, const num *x, const num *y)
{
	struct point r = { to_field(&c->p, x), to_field(&c->p, y), c->p.one };

	return (r);
}

static int
coordinates(const struct curve *c, const struct point *a, num *x, num *y)
{
	num zi, zi2, t;

	if (is_zero(&a->z)) {
		return (0);
	}
	zi = power(&c->p, &a->z, &c->p_minus_2);
	zi2 = mul(&c->p, &zi, &zi);
	t = mul(&c->p, &a->x, &zi2);
	*x = from_field(&c->p, &t);
	t = mul(&c->p, &zi2, &zi);
	t = mul(&c->p, &a->y, &t);
	*y = from_field(&c->p, &t);
	return (1);
}

/* x^3 - 3x + b, x and the result as kept */
static num
curve_rhs(const struct curve *c, const num *x)
{
	num t = mul(&c->p, x, x), three_x;

	t = mul(&c->p, &t, x);
	three_x = mod_add(&c->p, x, x);
	three_x = mod_add(&c->p, &three_x, x);
	t = mod_sub(&c->p, &t, &three_x);
	return (mod_add(&c->p, &t, &c->b));
}

static int
on_curve(const struct curve *c, const num *x, const num *y)
{
	num xm, ym, lhs, rhs;

	if (compare(x, &p_256) >= 0 || compare(y, &p_256) >= 0) {
		return (0);
	}
	xm = to_field(&c->p, x);
	ym = to_field(&c->p, y);
	lhs = mul(&c->p, &ym, &ym);
	rhs = curve_rhs(c, &xm);
	return (compare(&lhs, &rhs) == 0);
}

/* ---- SAE ----------------------------------------------------------------------------------------- */

static struct curve curve;
static int curve_made;

static void
curve_once(void)
{
	if (!curve_made) {
		curve_init(&curve);
		curve_made = 1;
	}
}

/* A number in [2, n - 1], from `random`. */
static num
random_scalar(void (*random)(void *, unsigned))
{
	num two = small(2), r;
	uint8_t bytes[32];

	do {
		random(bytes, sizeof(bytes));
		r = from_bytes(bytes);
	} while (compare(&r, &n_256) >= 0 || compare(&r, &two) < 0);
	memset(bytes, 0, sizeof(bytes));
	return (r);
}

static int commit_from_pwe(struct lx_sae *sae, num *x_pwe, num *y_pwe, void (*random)(void *, unsigned));

int
lx_sae_prepare(struct lx_sae *sae, const unsigned char *password, unsigned password_length, const unsigned char own[6],
    const unsigned char peer[6], void (*random)(void *, unsigned))
{
	uint8_t addresses[12], p_bytes[32], seed[32], found_seed[32], value[32];
	num found_x = small(0), x, y, rhs, legendre;
	int found = 0;

	curve_once();
	memset(sae, 0, sizeof(*sae));
	memcpy(sae->own, own, 6);
	memcpy(sae->peer, peer, 6);
	/* the larger address first */
	memcpy(addresses, memcmp(own, peer, 6) > 0 ? own : peer, 6);
	memcpy(addresses + 6, memcmp(own, peer, 6) > 0 ? peer : own, 6);
	to_bytes(&p_256, p_bytes);
	/* every one of the 40 tries is made, whatever the first good one was (no timing of the passphrase) */
	for (unsigned counter = 1; counter <= 40; counter++) {
		uint8_t number = (uint8_t)counter;
		const uint8_t *pieces[2] = { password, &number };
		const unsigned lengths[2] = { password_length, 1 };
		int good;

		lx_wpa_hmac_sha256(addresses, 12, pieces, lengths, 2, seed);
		lx_wpa_kdf_sha256(seed, 32, "SAE Hunting and Pecking", p_bytes, 32, value, 256);
		x = from_bytes(value);
		if (compare(&x, &p_256) >= 0) {
			continue;
		}
		x = to_field(&curve.p, &x);
		rhs = curve_rhs(&curve, &x);
		legendre = power(&curve.p, &rhs, &curve.p_minus_1_half);
		good = compare(&legendre, &curve.p.one) == 0;
		if (good && !found) {
			found_x = x;
			memcpy(found_seed, seed, 32);
			found = 1;
		}
	}
	memset(seed, 0, sizeof(seed));
	memset(value, 0, sizeof(value));
	if (!found) {
		return (-1);
	}
	rhs = curve_rhs(&curve, &found_x);
	y = power(&curve.p, &rhs, &curve.p_plus_1_quarter);
	y = from_field(&curve.p, &y);
	x = from_field(&curve.p, &found_x);
	if ((found_seed[31] & 1) != (y.v[0] & 1)) {
		sub(&y, &p_256, &y);
	}
	memset(found_seed, 0, sizeof(found_seed));
	return (commit_from_pwe(sae, &x, &y, random));
}

/* With the password element (plain x, y) there: our commit -- rand and mask, scalar = rand + mask mod n (not below 2), element = -(mask PWE) */
static int
commit_from_pwe(struct lx_sae *sae, num *x_pwe, num *y_pwe, void (*random)(void *, unsigned))
{
	struct field order;
	num x = *x_pwe, y = *y_pwe, mask, scalar, two = small(2);
	struct point pwe, m;

	memcpy(sae->pwe, x.v, 32);
	memcpy(sae->pwe + 4, y.v, 32);
	sae->have_pwe = 1;
	field_init(&order, &n_256);
	pwe = point_of(&curve, &x, &y);
	do {
		num r = random_scalar(random);

		mask = random_scalar(random);
		scalar = mod_add(&order, &r, &mask);
		memcpy(sae->rand, r.v, 32);
	} while (compare(&scalar, &two) < 0);
	m = point_times(&curve, &mask, &pwe);
	if (!coordinates(&curve, &m, &x, &y)) {
		return (-1);
	}
	sub(&y, &p_256, &y);
	to_bytes(&scalar, sae->scalar);
	to_bytes(&x, sae->element);
	to_bytes(&y, sae->element + 32);
	memset(&mask, 0, sizeof(mask));
	return (0);
}

/* ---- hash to element (H2E: IEEE 802.11-2020, 12.4.4.2.3, after RFC 9380's simplified SWU) ----------------- */

/* HKDF-Expand with SHA-256 (RFC 5869), up to 64 bytes */
static void
hkdf_expand(const uint8_t prk[32], const char *info, uint8_t *out, unsigned length)
{
	uint8_t t[32];
	unsigned have = 0, info_length = (unsigned)strlen(info);

	for (uint8_t counter = 1; have < length; counter++) {
		const uint8_t *pieces[3] = { t, (const uint8_t *)info, &counter };
		const unsigned lengths[3] = { counter == 1 ? 0 : 32, info_length, 1 };
		unsigned n = length - have < 32 ? length - have : 32;

		lx_wpa_hmac_sha256(prk, 32, pieces, lengths, 3, t);
		memcpy(out + have, t, n);
		have += n;
	}
	memset(t, 0, sizeof(t));
}

/* a 48-byte number, big-endian, modulo p (as kept): high * (2^256 mod p) + low */
static num
mod_p_48(const uint8_t v[48])
{
	uint8_t high_bytes[32] = { 0 };
	num high, low, r256;

	memcpy(high_bytes + 16, v, 16);
	high = from_bytes(high_bytes);
	low = from_bytes(v + 16);
	if (compare(&low, &p_256) >= 0) {
		sub(&low, &low, &p_256);	/* (2^256 < 2p: once is enough) */
	}
	r256 = curve.p.one;				/* 1 as kept is R mod p: the plain number 2^256 mod p */
	high = to_field(&curve.p, &high);
	r256 = to_field(&curve.p, &r256);
	low = to_field(&curve.p, &low);
	high = mul(&curve.p, &high, &r256);
	return (mod_add(&curve.p, &high, &low));
}

/* r = mask ? a : b, mask all ones or zero */
static num
select_num(const num *a, const num *b, uint64_t mask)
{
	num r;

	for (unsigned i = 0; i < 4; i++) {
		r.v[i] = (a->v[i] & mask) | (b->v[i] & ~mask);
	}
	return (r);
}

static uint64_t
mask_of(int condition)
{
	return ((uint64_t)0 - (uint64_t)(condition != 0));
}

/* The curve's point of u (kept) by the simplified SWU map with Z = -10; x and y plain. No branch on u. */
static void
sswu(const num *u, num *x_out, num *y_out)
{
	num ten = small(10), three = small(3), z, a, t1, t2, m, t, x1a, x1b, x1, gx1, x2, gx2, l, v, x, y, u_plain, y_plain, neg_y, one;
	uint64_t m_is_zero, is_qr, same_sign;

	sub(&z, &p_256, &ten);
	z = to_field(&curve.p, &z);
	sub(&a, &p_256, &three);
	a = to_field(&curve.p, &a);
	one = curve.p.one;
	/* m = Z^2 u^4 + Z u^2 = t1^2 + t1, t1 = Z u^2 */
	t1 = mul(&curve.p, u, u);
	t1 = mul(&curve.p, &z, &t1);
	t2 = mul(&curve.p, &t1, &t1);
	m = mod_add(&curve.p, &t1, &t2);
	m_is_zero = mask_of(is_zero(&m));
	t = power(&curve.p, &m, &curve.p_minus_2);		/* 1/m, and 0 for 0 */
	/* x1 = m == 0 ? B / (Z A) : (-B / A) (1 + t) */
	t2 = mul(&curve.p, &z, &a);
	t2 = power(&curve.p, &t2, &curve.p_minus_2);
	x1a = mul(&curve.p, &curve.b, &t2);
	t2 = power(&curve.p, &a, &curve.p_minus_2);
	x1b = mod_sub(&curve.p, &(num){ { 0 } }, &curve.b);
	x1b = mul(&curve.p, &x1b, &t2);
	t2 = mod_add(&curve.p, &one, &t);
	x1b = mul(&curve.p, &x1b, &t2);
	x1 = select_num(&x1a, &x1b, m_is_zero);
	gx1 = curve_rhs(&curve, &x1);
	x2 = mul(&curve.p, &t1, &x1);
	gx2 = curve_rhs(&curve, &x2);
	/* gx1 a square (or zero): x1, else x2 */
	l = power(&curve.p, &gx1, &curve.p_minus_1_half);
	is_qr = mask_of(is_zero(&l) | (compare(&l, &one) == 0));
	v = select_num(&gx1, &gx2, is_qr);
	x = select_num(&x1, &x2, is_qr);
	y = power(&curve.p, &v, &curve.p_plus_1_quarter);
	/* y's sign as u's */
	u_plain = from_field(&curve.p, u);
	y_plain = from_field(&curve.p, &y);
	sub(&neg_y, &p_256, &y_plain);
	same_sign = mask_of((u_plain.v[0] & 1) == (y_plain.v[0] & 1));
	*y_out = select_num(&y_plain, &neg_y, same_sign);
	*x_out = from_field(&curve.p, &x);
}

int
lx_sae_pt(unsigned char pt[64], const unsigned char *password, unsigned password_length, const unsigned char *ssid,
    unsigned ssid_length, const unsigned char *identifier, unsigned identifier_length)
{
	uint8_t seed[32], value[48], b[32];
	const uint8_t *pieces[2] = { password, identifier };
	const unsigned lengths[2] = { password_length, identifier != NULL ? identifier_length : 0 };
	num u, x1, y1, x2, y2, x, y;
	struct point p1, p2, sum;

	curve_once();
	/* pwd-seed = HKDF-Extract(SSID, password [|| identifier]) */
	lx_wpa_hmac_sha256(ssid, ssid_length, pieces, lengths, identifier != NULL && identifier_length != 0 ? 2 : 1, seed);
	hkdf_expand(seed, "SAE Hash to Element u1 P1", value, sizeof(value));
	u = mod_p_48(value);
	sswu(&u, &x1, &y1);
	hkdf_expand(seed, "SAE Hash to Element u2 P2", value, sizeof(value));
	u = mod_p_48(value);
	sswu(&u, &x2, &y2);
	memset(seed, 0, sizeof(seed));
	memset(value, 0, sizeof(value));
	p1 = point_of(&curve, &x1, &y1);
	p2 = point_of(&curve, &x2, &y2);
	sum = point_add(&curve, &p1, &p2);
	if (!coordinates(&curve, &sum, &x, &y)) {
		return (-1);
	}
	to_bytes(&x, b);
	memcpy(pt, b, 32);
	to_bytes(&y, b);
	memcpy(pt + 32, b, 32);
	return (0);
}

int
lx_sae_prepare_pt(struct lx_sae *sae, const unsigned char pt[64], const unsigned char own[6], const unsigned char peer[6],
    void (*random)(void *, unsigned))
{
	uint8_t zeros[32] = { 0 }, addresses[12], hash[32];
	const uint8_t *pieces[1] = { addresses };
	const unsigned lengths[1] = { 12 };
	num val, n_minus_1, one = small(1), x, y;
	struct point pt_point, pwe;

	curve_once();
	memset(sae, 0, sizeof(*sae));
	memcpy(sae->own, own, 6);
	memcpy(sae->peer, peer, 6);
	x = from_bytes(pt);
	y = from_bytes(pt + 32);
	if (!on_curve(&curve, &x, &y)) {
		return (-1);
	}
	/* val = H(0^32, MAX(addresses) || MIN(addresses)) modulo (n - 1) + 1; PWE = val PT */
	memcpy(addresses, memcmp(own, peer, 6) > 0 ? own : peer, 6);
	memcpy(addresses + 6, memcmp(own, peer, 6) > 0 ? peer : own, 6);
	lx_wpa_hmac_sha256(zeros, 32, pieces, lengths, 1, hash);
	val = from_bytes(hash);
	sub(&n_minus_1, &n_256, &one);
	if (compare(&val, &n_minus_1) >= 0) {
		sub(&val, &val, &n_minus_1);
	}
	add(&val, &val, &one);
	pt_point = point_of(&curve, &x, &y);
	pwe = point_times(&curve, &val, &pt_point);
	if (!coordinates(&curve, &pwe, &x, &y)) {
		return (-1);
	}
	sae->h2e = 1;
	return (commit_from_pwe(sae, &x, &y, random));
}

unsigned
lx_sae_commit(const struct lx_sae *sae, const unsigned char *token, unsigned token_length, unsigned char *out)
{
	unsigned at = 0;

	out[at++] = LX_SAE_GROUP;
	out[at++] = 0;
	if (!sae->h2e && token != NULL && token_length != 0 && token_length <= 256) {
		memcpy(out + at, token, token_length);
		at += token_length;
	}
	memcpy(out + at, sae->scalar, 32);
	memcpy(out + at + 32, sae->element, 64);
	at += 96;
	/* with H2E the token is an element after them: the Anti-Clogging Token Container (255, extension 93) */
	if (sae->h2e && token != NULL && token_length != 0 && token_length <= 254) {
		out[at++] = 255;
		out[at++] = (uint8_t)(1 + token_length);
		out[at++] = 93;
		memcpy(out + at, token, token_length);
		at += token_length;
	}
	return (at);
}

int
lx_sae_peer_commit(struct lx_sae *sae, const unsigned char *body, unsigned length, const char **why)
{
	struct field order;
	num scalar, x, y, rand, context, one = small(1);
	struct point pwe, element, k;
	uint8_t k_bytes[32], keyseed[32], zeros[32] = { 0 }, context_bytes[32], keys[64];
	const uint8_t *pieces[1] = { k_bytes };
	const unsigned lengths[1] = { 32 };

	if (!sae->have_pwe) {
		*why = "there is no password element";
		return (-1);
	}
	if (length < 2 + 96 || (body[0] | body[1] << 8) != LX_SAE_GROUP) {
		*why = length < 2 ? "its commit is too short" : "its commit is for another group than 19";
		return (-1);
	}
	body += 2;
	scalar = from_bytes(body);
	x = from_bytes(body + 32);
	y = from_bytes(body + 64);
	if (compare(&scalar, &one) <= 0 || compare(&scalar, &n_256) >= 0) {
		*why = "its scalar is not between 1 and the group's order";
		return (-1);
	}
	if (!on_curve(&curve, &x, &y)) {
		*why = "its element is not a point of the curve";
		return (-1);
	}
	if (memcmp(body, sae->scalar, 32) == 0 && memcmp(body + 32, sae->element, 64) == 0) {
		*why = "its commit is ours, sent back (a reflection)";
		return (-1);
	}
	memcpy(sae->peer_scalar, body, 32);
	memcpy(sae->peer_element, body + 32, 64);

	/* K = rand (peer scalar PWE + peer element); k its x */
	memcpy(x.v, sae->pwe, 32);
	memcpy(y.v, sae->pwe + 4, 32);
	pwe = point_of(&curve, &x, &y);
	x = from_bytes(sae->peer_element);
	y = from_bytes(sae->peer_element + 32);
	element = point_of(&curve, &x, &y);
	k = point_times(&curve, &scalar, &pwe);
	k = point_add(&curve, &k, &element);
	memcpy(rand.v, sae->rand, 32);
	k = point_times(&curve, &rand, &k);
	if (!coordinates(&curve, &k, &x, &y)) {
		*why = "the shared point is the point at infinity";
		return (-1);
	}
	to_bytes(&x, k_bytes);

	/* keyseed = H(<0>32, k); KCK | PMK = KDF-512(keyseed, "SAE KCK and PMK", (scalar + peer scalar) mod n) */
	lx_wpa_hmac_sha256(zeros, 32, pieces, lengths, 1, keyseed);
	field_init(&order, &n_256);
	x = from_bytes(sae->scalar);
	context = mod_add(&order, &x, &scalar);
	to_bytes(&context, context_bytes);
	lx_wpa_kdf_sha256(keyseed, 32, "SAE KCK and PMK", context_bytes, 32, keys, 512);
	memcpy(sae->kck, keys, 32);
	memcpy(sae->pmk, keys + 32, 32);
	memcpy(sae->pmkid, context_bytes, 16);
	sae->have_keys = 1;
	memset(k_bytes, 0, sizeof(k_bytes));
	memset(keyseed, 0, sizeof(keyseed));
	memset(keys, 0, sizeof(keys));
	memset(&rand, 0, sizeof(rand));
	return (0);
}

/* CN(KCK, send-confirm, scalar, element, peer scalar, peer element), ours or the peer's way round */
static void
confirm_of(const struct lx_sae *sae, unsigned counter, int ours, unsigned char out[32])
{
	uint8_t number[2] = { (uint8_t)counter, (uint8_t)(counter >> 8) };
	const uint8_t *pieces[5] = { number, ours ? sae->scalar : sae->peer_scalar, ours ? sae->element : sae->peer_element,
		                     ours ? sae->peer_scalar : sae->scalar, ours ? sae->peer_element : sae->element };
	const unsigned lengths[5] = { 2, 32, 64, 32, 64 };

	lx_wpa_hmac_sha256(sae->kck, 32, pieces, lengths, 5, out);
}

unsigned
lx_sae_confirm(struct lx_sae *sae, unsigned char *out)
{
	if (sae->send_confirm < 0xffff) {
		sae->send_confirm++;
	}
	out[0] = (uint8_t)sae->send_confirm;
	out[1] = (uint8_t)(sae->send_confirm >> 8);
	confirm_of(sae, sae->send_confirm, 1, out + 2);
	return (LX_SAE_CONFIRM);
}

int
lx_sae_peer_confirm(const struct lx_sae *sae, const unsigned char *body, unsigned length, const char **why)
{
	uint8_t expected[32];
	unsigned difference = 0;

	if (!sae->have_keys || length < LX_SAE_CONFIRM) {
		*why = !sae->have_keys ? "a confirm before any commit" : "its confirm is too short";
		return (-1);
	}
	confirm_of(sae, (unsigned)(body[0] | body[1] << 8), 0, expected);
	for (unsigned i = 0; i < 32; i++) {
		difference |= expected[i] ^ body[2 + i];
	}
	if (difference != 0) {
		*why = "its confirm does not prove the same key (a wrong passphrase gives this)";
		return (-1);
	}
	return (0);
}
