#!/bin/bash
# Save the kernel work as it is in the working tree, in the two forms apply.sh puts back:
#   hackios.patch   the changes to Apple's files (against the tag the work sits on)
#   files/          the files that are not in Apple's tree, at their paths
# usage: xnu/export.sh [KERNEL-TREE]     (default: $XNU_SRC, else xnu-src/xnu-8796.121.2)
# Run it before committing whenever the kernel tree has changed. Committed or not makes no
# difference there: everything is compared with the base commit.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
BASE=c54f35ca767986246321eb901baf8f5ff7923f6a           # tag xnu-8796.121.2
# never part of the work: build output and what editors, patch and python leave behind
JUNK='^BUILD/|(^|/)__pycache__/|\.(orig|rej|pyc|o|swp)$|\.bak[0-9]*$|~$|(^|/)\.DS_Store$'
# and Apple's HFS+ sources, which xnu/hfs-stage.sh fetches as apply.sh fetches XNU (the glue beside them stays)
JUNK="$JUNK|^bsd/hk_hfs/(core|hfs_encodings)/|^bsd/hk_hfs/TargetConditionals\.h\$"

cd "$SRC"
git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "export.sh: $SRC is not a clone of xnu at $BASE" >&2; exit 1; }

# no renames: a moved file is a deletion in the patch and a new file in files/
git -c core.abbrev=12 diff --binary --no-renames --no-color --no-ext-diff --diff-filter=MDT "$BASE" -- . > "$HERE/hackios.patch.new"
mv "$HERE/hackios.patch.new" "$HERE/hackios.patch"

rm -rf "$HERE/files.new"
mkdir "$HERE/files.new"
{ git diff --no-renames --name-only --diff-filter=A "$BASE" -- .; git ls-files --others --exclude-standard; } |
	sort -u | grep -Ev "$JUNK" > "$HERE/files.list"
while IFS= read -r f; do
	[ -f "$f" ] || [ -L "$f" ] || continue      # added in a commit, deleted since
	mkdir -p "$HERE/files.new/$(dirname "$f")"
	cp -P --preserve=mode "$f" "$HERE/files.new/$f"
done < "$HERE/files.list"
rm -rf "$HERE/files" "$HERE/files.list"
mv "$HERE/files.new" "$HERE/files"

echo "xnu/hackios.patch: $(grep -c '^diff --git' "$HERE/hackios.patch") files changed, $(wc -c < "$HERE/hackios.patch") bytes"
echo "xnu/files: $(find "$HERE/files" -type f | wc -l) new files, $(du -sk "$HERE/files" | cut -f1) KB"
