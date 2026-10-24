#!/bin/bash
# Put Apple's open-source HFS+ (hfs-627.100.6, the release macOS 13.4 pairs with xnu-8796.121.2) into the
# kernel tree as bsd/hk_hfs/core and bsd/hk_hfs/hfs_encodings, for the built-in filesystem of an installed
# phone's data volume (bsd/hk_hfs/hk_hfs_iokit.c; the integration follows ios-instantnoodle's 0026 patch,
# which ran iOS 16 on it). Like XNU itself the sources are fetched, not kept in this repository:
# xnu/apply.sh runs this, and xnu/export.sh leaves them out.
# usage: xnu/hfs-stage.sh [KERNEL-TREE]     (default: $XNU_SRC, else xnu-src/xnu-8796.121.2)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DIR=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
SHA=31d5ec238dfab485e78a9a30b8b92d13e8914f03      # refs/tags/hfs-627.100.6: an immutable commit, not a branch
[ -f "$DIR/bsd/kern/bsd_init.c" ] || { echo "hfs-stage.sh: $DIR is not a kernel tree" >&2; exit 1; }
[ -e "$DIR/bsd/hk_hfs/core" ] && { echo "hfs-stage.sh: $DIR/bsd/hk_hfs/core is there already"; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp"
git -C "$tmp" fetch -q --depth 1 https://github.com/apple-oss-distributions/hfs "$SHA"
git -C "$tmp" checkout -q FETCH_HEAD
[ "$(git -C "$tmp" rev-parse HEAD)" = "$SHA" ] || { echo "hfs-stage.sh: not the commit asked for" >&2; exit 1; }
mkdir -p "$DIR/bsd/hk_hfs"
cp -a "$tmp/core" "$tmp/hfs_encodings" "$DIR/bsd/hk_hfs/"
# no AppleKeyStore here (content protection is not built, hk_hfs_compat.h): its header only for the key API
python3 - "$DIR/bsd/hk_hfs/core/hfs_iokit.h" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
include = '#include <AppleKeyStore/AppleKeyStoreFSServices.h>\n'
first, end = 'int hfs_unwrap_key(', '__END_DECLS'
if text.count(include) != 1 or text.count(first) != 1 or text.count(end) != 1:
    sys.exit('hfs-stage.sh: %s is not the one expected' % path)
text = text.replace(include, '#if CONFIG_PROTECT\n' + include + '#endif\n')
i, j = text.index(first), text.index(end)
text = text[:i] + '#if CONFIG_PROTECT\n' + text[i:j].rstrip('\n') + '\n#endif\n\n' + text[j:]
open(path, 'w').write(text)
PY
# kernel builds use -nostdinc; HFS's kext-config.h wants the SDK's target macros
SDK=${XNU_SDK:-$HERE/../xnu-deps/sdk/iPhoneOS16.5.xnu.sdk}
cp "$SDK/usr/include/TargetConditionals.h" "$DIR/bsd/hk_hfs/"
echo "hfs-stage.sh: hfs-627.100.6 ($SHA) in $DIR/bsd/hk_hfs"
