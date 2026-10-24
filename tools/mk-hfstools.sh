#!/bin/bash
# Build Apple's newfs_hfs (hfs-627.100.6, the release of the kernel's built-in HFS+: xnu/hfs-stage.sh) for iOS,
# for userland/build.sh to put into the small image's bin: an installed phone formats its data volume with it
# (userland/src/init.c), as a RAM-only run formats its throwaway one. Apple builds this tool for iOS itself
# (makehfs.c's TARGET_OS_IPHONE paths); what the SDK here lacks is stood in for below, all of it headers:
#   IOKit/storage/IOMedia.h          only used by the macOS-only get_dev_uuid(): empty
#   IOKit/storage/IOMediaBSDClient.h the DKIOC* ioctls, which are the kernel's <sys/disk.h>
#   wipefs.h                         libutil's (/usr/lib/libutil.dylib exports wipefs_*: libutil.tbd)
# Output: xnu-deps/build/hfs/newfs_hfs (arm64, iOS 16). Sources are fetched, not kept in this repository.
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
SHA=31d5ec238dfab485e78a9a30b8b92d13e8914f03      # refs/tags/hfs-627.100.6
OUT=$ROOT/xnu-deps/build/hfs
SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
[ -d "$SDK" ] || SDK=$(ls -d /nix/store/*theos*/share/theos/sdks/iPhoneOS16.5.sdk 2>/dev/null | head -1)
[ -d "$SDK" ] || { echo "mk-hfstools.sh: no iPhoneOS16.5.sdk (theos)" >&2; exit 1; }
XNU=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}
mkdir -p "$OUT"
if [ ! -d "$OUT/src/.git" ] || [ "$(git -C "$OUT/src" rev-parse HEAD)" != "$SHA" ]; then
	rm -rf "$OUT/src"
	git init -q "$OUT/src"
	git -C "$OUT/src" fetch -q --depth 1 https://github.com/apple-oss-distributions/hfs "$SHA"
	git -C "$OUT/src" checkout -q FETCH_HEAD
fi
[ "$(git -C "$OUT/src" rev-parse HEAD)" = "$SHA" ] || { echo "mk-hfstools.sh: not the commit asked for" >&2; exit 1; }

INC=$OUT/include
rm -rf "$INC"
mkdir -p "$INC/IOKit/storage" "$INC/hfs" "$INC/sys"
: > "$INC/IOKit/storage/IOMedia.h"
echo '#include <sys/disk.h>' > "$INC/IOKit/storage/IOMediaBSDClient.h"
cp "$XNU/bsd/sys/disk.h" "$INC/sys/disk.h"
cp "$OUT/src/core/hfs_format.h" "$OUT/src/core/hfs_mount.h" "$OUT/src/core/hfs_unistr.h" "$INC/hfs/"
cat > "$INC/wipefs.h" <<'EOF'
/* libutil's wipefs (Apple's libutil, wipefs.h), as /usr/lib/libutil.dylib exports it */
#include <sys/types.h>
typedef struct __wipefs_ctx *wipefs_ctx;
int wipefs_alloc(int fd, size_t block_size, wipefs_ctx *handle);
int wipefs_include_blocks(wipefs_ctx handle, off_t block_offset, off_t nblocks);
int wipefs_except_blocks(wipefs_ctx handle, off_t block_offset, off_t nblocks);
int wipefs_wipe(wipefs_ctx handle);
void wipefs_free(wipefs_ctx *handle);
EOF

N=$OUT/src/newfs_hfs
xnu-clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -O1 -w -I"$INC" -I"$N" \
	-F"$SDK/System/Library/Frameworks" -I"$SDK/System/Library/Frameworks/IOKit.framework/Versions/A/Headers" \
	-o "$OUT/newfs_hfs" "$N/newfs_hfs.c" "$N/makehfs.c" "$N/hfs_endian.c" \
	-framework CoreFoundation -framework IOKit -lutil
echo "mk-hfstools.sh: $OUT/newfs_hfs ($(stat -c %s "$OUT/newfs_hfs") bytes, hfs-627.100.6)"
