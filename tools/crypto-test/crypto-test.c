/*
 * The loader's crypto (xnuloader/crypto.c), built for the computer: reads lines from stdin and answers
 * each, for run.py to compare with Python's hashlib and `cryptography`.
 *   sha256 HEX           -> the digest
 *   verify SIG PK MSG    -> 1 or 0 (all hex)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../xnuloader/crypto.c"

static uint64_t
unhex(const char *s, uint8_t *out)
{
	uint64_t n = 0;
	for (; s[0] && s[1] && s[0] != ' ' && s[0] != '\n'; s += 2) {
		unsigned v;
		sscanf(s, "%2x", &v);
		out[n++] = (uint8_t)v;
	}
	return n;
}

int
main(void)
{
	static char line[1 << 20];
	static uint8_t a[1 << 19], b[64], c[64];

	while (fgets(line, sizeof(line), stdin)) {
		if (strncmp(line, "sha256 ", 7) == 0) {
			uint8_t d[32];
			sha256(a, unhex(line + 7, a), d);
			for (int i = 0; i < 32; i++) printf("%02x", d[i]);
			printf("\n");
		} else if (strncmp(line, "verify ", 7) == 0) {
			char *p = line + 7, *q = strchr(p, ' ') + 1, *r = strchr(q, ' ') + 1;
			unhex(p, b); unhex(q, c);
			uint64_t n = *r == '-' ? 0 : unhex(r, a);
			printf("%d\n", ed25519_verify(b, a, n, c));
		}
		fflush(stdout);
	}
	return 0;
}
