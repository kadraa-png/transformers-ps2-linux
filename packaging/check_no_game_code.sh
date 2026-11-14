#!/bin/sh
# usage: check_no_game_code.sh <AppDir or install tree>
# Fails if anything derived from the game disc is inside: PS2 (MIPS) ELF
# files, IOP modules, recompiler output, or prebuilt archives holding
# game-derived objects.  Launcher art (art/*.png) is expected.
set -u
dir=$1
bad=0
here=$(cd "$(dirname "$0")" && pwd)

# Paths that only recompiler output or disc files have.
hits=$(find "$dir" \( -iname '*.irx' -o \( -type d -name 'generated*' \) -o -name 'SLES_*' -o -name 'SLUS_*' \
        -o -name 'ee_[0-9]*.cpp*' -o -name 'vu_image_shard*' -o -name 'vu_pairs.cpp*' -o -name 'iso_files' \) \
        -print 2>/dev/null | head -20)
if [ -n "$hits" ]; then
  echo "game-derived paths:"; echo "$hits"; bad=1
fi

# MIPS ELF files (e_machine 8 at offset 18, little endian).
mips=$(mktemp)
find "$dir" -type f -size +52c 2>/dev/null | while read -r f; do
  magic=$(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' \n')
  [ "$magic" = "7f454c46" ] || continue
  machine=$(od -An -tu2 -j18 -N2 "$f" 2>/dev/null | tr -d ' \n')
  [ "$machine" = "8" ] && echo "MIPS ELF: $f"
done > "$mips"
if [ -s "$mips" ]; then cat "$mips"; bad=1; fi
rm -f "$mips"

sh "$here/check_runtime_split.sh" "$dir" >/dev/null || { sh "$here/check_runtime_split.sh" "$dir"; bad=1; }

[ $bad -eq 0 ] && echo "OK: no game code"
exit $bad
