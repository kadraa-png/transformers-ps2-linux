#!/bin/sh
# usage: check_runtime_split.sh <runtime-only build or install dir>
# Fails if a prebuilt runtime archive holds game-derived code: objects built
# from generated sources, or the symbols they define.
pattern='^(ee_[0-9]+|iop_registry|[a-z0-9_]+_(module|server)(_[0-9]+)?|sound_device_library(_[0-9]+)?|twofish(_[0-9]+)?|vu_pairs|VUops|vu_image_shard[0-9]+(\.local)?)\.(cpp\.)?o:$| f_[0-9a-f]{8}\b| vu_img[0-9]+_|g_ee_elf_crc32'
bad=0
for a in $(find "$1" -name 'libps2_*.a' -o -name 'libgsdx.a'); do
  if nm -C --defined-only "$a" 2>/dev/null | grep -Eq "$pattern"; then
    echo "game-derived code in $a"; bad=1
  fi
done
[ $bad -eq 0 ] && echo "OK: prebuilt archives hold no game code"
exit $bad
