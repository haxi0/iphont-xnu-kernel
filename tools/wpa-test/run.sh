#!/bin/bash
# Build and run the tests of the WPA2 and WPA3 arithmetic (wifi/lx/src/lx_wpa.c, lx_sae.c) on this
# machine, under the address and undefined-behaviour sanitizers. Needs Python 3 with the
# "cryptography" package: the access point the handshakes are held with is reference.py.
# usage: tools/wpa-test/run.sh [SEED]      another seed, other made-up networks
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=$ROOT/xnu-deps/build/wpa-test
rm -rf "$B"
mkdir -p "$B"
"$HERE/reference.py" "${1:-1}" > "$B/cases.inc"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B" -I"$ROOT/wifi/lx/src" -o "$B/test" "$HERE/test_wpa.c"
"$B/test"
# ... and WPA3: SAE and the handshake of AKM 8 against sae_reference.py (plain integers for the curve)
"$HERE/sae_reference.py" "${1:-1}" > "$B/sae_cases.inc"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B" -I"$ROOT/wifi/lx/src" -o "$B/test_sae" "$HERE/test_sae.c"
"$B/test_sae"
