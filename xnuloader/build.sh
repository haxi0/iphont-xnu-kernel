#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
# the owner key the loader checks boot bundles against (common/flash/ownerkey.py); none pinned by default
[ -f owner_key.h ] || OWNER_KEY_H= python3 ../common/flash/ownerkey.py unpin > /dev/null
# where this phone's darwin is (DARWIN_ID=phones/<serial>.darwin; none: the defaults), made anew every build
python3 ../common/flash/darwinid.py header "${DARWIN_ID:+$(readlink -f "$DARWIN_ID")}" darwin_identity.h
# OWNER_KEY_H=file: build with that key header instead (tests); OUT=name: the .efi to write; LOADER_DEFS: -D... (tests)
OUT=${OUT:-loader.efi}
KEYDEF=; [ -n "$OWNER_KEY_H" ] && KEYDEF="-DOWNER_KEY_FILE=\"$(readlink -f "$OWNER_KEY_H")\""
clang --target=aarch64-unknown-windows $KEYDEF $LOADER_DEFS -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -c loader.c -o ${OUT%.efi}.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:$OUT ${OUT%.efi}.obj
ls -la $OUT
