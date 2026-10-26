#!/bin/bash
# Build and run the tests of the MHI host (wifi/lx/src/mhi_host.c) on this machine, against the
# device test_mhi.c makes in software, under the address and undefined-behaviour sanitizers.
# usage: tools/mhi-test/run.sh [loud]      "loud": what the host says, too
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=$ROOT/xnu-deps/build/mhi-test
rm -rf "$B"
mkdir -p "$B"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all \
	-pthread -I"$ROOT/wifi/lx/src" -o "$B/test" "$HERE/test_mhi.c" "$ROOT/wifi/lx/src/mhi_host.c"
"$B/test" "$@"
