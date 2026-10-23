#!/bin/sh
# Build and run the thermal governor's tests on the computer.
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -Wextra -Werror -o "${TMPDIR:-/tmp}/hackios-thermal-test" thermal-test.c
"${TMPDIR:-/tmp}/hackios-thermal-test"
