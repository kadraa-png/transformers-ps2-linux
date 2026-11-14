#!/bin/sh
# Builds packaging/out/Transformers-x86_64.AppImage in the Docker image from
# packaging/Dockerfile.  The AppImage holds the launcher, the prebuilt runtime
# (no game code), the game art and a compiler toolchain; the game itself is
# built from the player's disc on first run.
#
# usage: packaging/build_appimage.sh [--version vX.Y.Z]
set -eu
version=
in_container=0
while [ $# -gt 0 ]; do
  case "$1" in
    --version) version=$2; shift 2 ;;
    --in-container) in_container=1; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done

if [ $in_container -eq 0 ]; then
  root=$(cd "$(dirname "$0")/.." && pwd)
  [ -n "$version" ] || version=$(git -C "$root" describe --tags --always --dirty)
  docker build -t transformers-port-build "$root/packaging"
  mkdir -p "$root/packaging/out"
  exec docker run --rm -v "$root:/work" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    transformers-port-build sh /work/packaging/build_appimage.sh --in-container --version "$version"
fi

cd /work
out=/work/packaging/out
b=/tmp/build-rt
appdir=/tmp/AppDir
rm -rf "$b" "$appdir"

# Runtime archives and launcher, built with the bundled GCC against /deps (and
# /deps/hostlink for GL/GLib, as runtime/deps holds them on a player's machine).
# Those link-only .so files need libraries outside the sysroot, hence
# --allow-shlib-undefined, as in packaging/game.
cmake -S . -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2_RUNTIME_ONLY=ON \
  -DCMAKE_PREFIX_PATH="/deps;/deps/hostlink" -DPS2_RUNTIME_VERSION="$version" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined"
ninja -C "$b"
"$b/launcher/launcher_tests"
cmake --install "$b" --prefix "$appdir/usr"

share="$appdir/usr/share/transformers-port"
mkdir -p "$share/runtime/deps"
cp -a /deps/lib /deps/include "$share/runtime/deps/"
cp -a /deps/hostlink/lib/. "$share/runtime/deps/lib/"
cp -a /deps/hostlink/include/. "$share/runtime/deps/include/"
rm -rf "$share/runtime/deps/lib/hostlink" "$share/runtime/deps/hostlink"

tc="$appdir/usr/toolchain"
mkdir -p "$tc"
cp -a /opt/tc "$tc/gcc"
cp -a /opt/tools/cmake "$tc/cmake"
rm -rf "$tc/cmake/doc" "$tc/cmake/man" "$tc/cmake/share/cmake-"*/Help "$tc/cmake/bin/cmake-gui"
cp -a /opt/tools/ninja "$tc/ninja"
cp -a /opt/tools/python "$tc/python"
rm -rf "$tc/python/lib/python3.12/test" "$tc/python/lib/python3.12/idlelib" "$tc/python/lib/python3.12/tkinter"

# The toolchain must need nothing but glibc on the player's machine.
bad=0
for f in $(find "$tc/gcc/bin" "$tc/gcc/libexec" -type f -perm -u+x); do
  file "$f" | grep -q ELF || continue
  for lib in $(objdump -p "$f" | awk '/NEEDED/ {print $2}'); do
    case "$lib" in
      libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|ld-linux-x86-64.so.2) ;;
      *) echo "toolchain binary $f needs $lib"; bad=1 ;;
    esac
  done
done
[ $bad -eq 0 ] || exit 1

cp packaging/AppRun "$appdir/AppRun"
cp packaging/transformers-port.desktop "$appdir/"
cp packaging/art/icon.png "$appdir/transformers-port.png"
mkdir -p "$appdir/usr/share/licenses/transformers-port"
cp LICENSE NOTICE "$appdir/usr/share/licenses/transformers-port/"

sh packaging/check_no_game_code.sh "$appdir"

mkdir -p "$out"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 /opt/tools/appimagetool --no-appstream \
  "$appdir" "$out/Transformers-x86_64.AppImage"
(cd "$out" && sha256sum Transformers-x86_64.AppImage > Transformers-x86_64.AppImage.sha256)
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$out"
ls -lh "$out"
