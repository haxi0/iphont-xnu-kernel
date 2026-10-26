/*
 * Ours, in place of LinuxKPI's crypto/hash.h, which has the names and nothing behind them: the
 * one keyed hash ath11k asks for, "michael_mic" (TKIP's check, wifi/lx/src/lx_wpa.c), for the
 * frames of a TKIP group that the radio leaves to its driver to check (dp_rx.c:
 * ath11k_dp_rx_h_verify_tkip_mic()). Any other name: -ENOENT, as Linux says of an algorithm it has not.
 */
#ifndef _LINUXKPI_CRYPTO_HASH_H
#define _LINUXKPI_CRYPTO_HASH_H

#include <linux/kernel.h>
#include <linux/err.h>

#include "../../src/lx_wpa.h"

struct crypto_shash {
	unsigned char key[8];
	bool keyed;
};

struct shash_desc {
	struct crypto_shash *tfm;
	struct lx_michael state;
};

struct crypto_shash *crypto_alloc_shash(const char *name, int type, int mask);
void crypto_free_shash(struct crypto_shash *tfm);
int crypto_shash_setkey(struct crypto_shash *tfm, const uint8_t *key, size_t length);
int crypto_shash_init(struct shash_desc *desc);
int crypto_shash_update(struct shash_desc *desc, const uint8_t *data, size_t length);
int crypto_shash_final(struct shash_desc *desc, uint8_t *out);

static inline void
shash_desc_zero(struct shash_desc *desc)
{
	explicit_bzero(desc, sizeof(*desc));
}

#define SHASH_DESC_ON_STACK(desc, tfm)		\
	struct shash_desc ___##desc##_desc;	\
	struct shash_desc *desc = &___##desc##_desc

#endif
