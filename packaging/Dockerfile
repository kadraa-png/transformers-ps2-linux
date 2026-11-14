# Build environment for the AppImage (packaging/build_appimage.sh).
#
# Ubuntu 22.04 (glibc 2.35) so the result runs on any distro with glibc >= 2.35.
# It also provides the toolchain that ships inside the AppImage: GCC 14 and
# binutils with a relocatable sysroot holding Ubuntu 22.04's glibc, so players
# need no compiler or libc headers of their own.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl xz-utils unzip file git pkg-config patchelf \
      bison flex texinfo wget bzip2 \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
      libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libgles-dev \
      libpulse-dev libpipewire-0.3-dev libasound2-dev libudev-dev libdbus-1-dev libdecor-0-dev \
      libglib2.0-dev libglib2.0-dev-bin zlib1g-dev libpng-dev liblzma-dev \
      squashfs-tools desktop-file-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Pinned downloads: <file> <sha256> <url>
RUN set -e; \
    fetch() { curl -fsSL -o "$1" "$3"; echo "$2  $1" | sha256sum -c -; }; \
    fetch binutils.tar.xz f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800 \
      https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz; \
    fetch gcc.tar.xz a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9 \
      https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz; \
    fetch cmake.tar.gz f747d9b23e1a252a8beafb4ed2bc2ddf78cff7f04a8e4de19f4ff88e9b51dc9d \
      https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz; \
    fetch ninja.zip 6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255 \
      https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip; \
    fetch python.tar.gz 43576f7db1033dd57b900307f09c2e86f371152ac8a2607133afa51cbfc36064 \
      "https://github.com/astral-sh/python-build-standalone/releases/download/20241016/cpython-3.12.7%2B20241016-x86_64-unknown-linux-gnu-install_only.tar.gz"; \
    fetch appimagetool 46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1 \
      https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage; \
    fetch freetype.tar.xz 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289 \
      https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz; \
    fetch sdl.tar.gz dad488474a51a0b01d547cd2834893d6299328d2e30f479a3564088b5476bae2 \
      https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26.tar.gz

# Tools that run inside the AppImage (static CMake, Ninja, standalone Python).
RUN set -e; mkdir -p /opt/tools; \
    tar -xzf cmake.tar.gz -C /opt/tools && mv /opt/tools/cmake-3.30.5-linux-x86_64 /opt/tools/cmake; \
    mkdir -p /opt/tools/ninja && unzip -q ninja.zip -d /opt/tools/ninja; \
    tar -xzf python.tar.gz -C /opt/tools; \
    chmod +x appimagetool && mv appimagetool /opt/tools/appimagetool
ENV PATH=/opt/tools/cmake/bin:/opt/tools/ninja:$PATH

# Relocatable sysroot: Ubuntu 22.04's glibc and kernel headers.
# 22.04 is usr-merged (/lib -> usr/lib), so lay out the same links first and
# copy only files and file symlinks.  GCC built with
# --with-native-system-header-dir has no multiarch search path, so the
# x86_64-linux-gnu headers and libraries are also made visible flat.  Absolute
# symlinks become relative so they resolve inside the sysroot.
RUN set -e; S=/opt/tc/sysroot; \
    for d in bin sbin lib lib64; do mkdir -p "$S/usr/$d"; ln -s "usr/$d" "$S/$d"; done; \
    for pkg in libc6 libc6-dev linux-libc-dev libcrypt1 libcrypt-dev; do \
      dpkg -L $pkg | while read -r f; do \
        if [ ! -d "$f" ] && { [ -f "$f" ] || [ -L "$f" ]; }; then \
          mkdir -p "$S$(dirname "$f")"; cp -a "$f" "$S$f"; \
        fi; \
      done; \
    done; \
    test -e "$S/lib64/ld-linux-x86-64.so.2" || cp -a /lib64/ld-linux-x86-64.so.2 "$S/lib64/"; \
    cp -a "$S/usr/include/x86_64-linux-gnu/." "$S/usr/include/"; \
    for f in "$S/usr/lib/x86_64-linux-gnu/"*; do \
      [ -e "$S/usr/lib/${f##*/}" ] || ln -s "x86_64-linux-gnu/${f##*/}" "$S/usr/lib/${f##*/}"; \
    done; \
    find "$S" -type l | while read -r l; do \
      t=$(readlink "$l"); case "$t" in /*) \
        d=$(realpath -m "$(dirname "$l")"); ln -sfn "$(realpath -m --relative-to="$d" "$S$t")" "$d/${l##*/}";; \
      esac; \
    done

# binutils + GCC 14 (C, C++) against that sysroot.  A sysroot under --prefix
# keeps the toolchain relocatable.
RUN set -e; tar -xJf binutils.tar.xz; mkdir b && cd b; \
    ../binutils-2.42/configure --prefix=/opt/tc --with-sysroot=/opt/tc/sysroot --disable-nls \
      --disable-werror --disable-gprofng --disable-gold --enable-deterministic-archives \
      --without-zstd --with-static-standard-libraries; \
    make -j"$(nproc)"; make install-strip; cd .. && rm -rf b binutils-2.42
# GMP/MPFR/MPC/ISL are built in-tree (download_prerequisites checks their
# SHA-512) so the compiler binaries need nothing beyond glibc on the player's PC.
RUN set -e; tar -xJf gcc.tar.xz; (cd gcc-14.2.0 && ./contrib/download_prerequisites); mkdir g && cd g; \
    ../gcc-14.2.0/configure --prefix=/opt/tc --with-sysroot=/opt/tc/sysroot \
      --with-native-system-header-dir=/usr/include --enable-languages=c,c++ \
      --disable-multilib --disable-bootstrap --disable-nls --disable-libsanitizer \
      --disable-libvtv --disable-libquadmath --disable-libgomp --disable-libitm \
      --with-gcc-major-version-only --without-zstd --with-static-standard-libraries; \
    make -j"$(nproc)"; make install-strip; cd .. && rm -rf g gcc-14.2.0

# Static dependencies the runtime links (/deps, also shipped for the game link).
RUN set -e; mkdir -p /deps/lib /deps/include; \
    cp -a /usr/lib/x86_64-linux-gnu/libz.a /usr/lib/x86_64-linux-gnu/libpng16.a \
          /usr/lib/x86_64-linux-gnu/liblzma.a /deps/lib/; \
    ln -s libpng16.a /deps/lib/libpng.a; \
    cp -a /usr/include/zlib.h /usr/include/zconf.h /usr/include/lzma.h /usr/include/lzma /deps/include/; \
    cp -a /usr/include/libpng16 /deps/include/; cp -a /usr/include/png*.h /deps/include/; \
    tar -xJf freetype.tar.xz; \
    cmake -S freetype-2.13.3 -B ft -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/deps \
      -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
      -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON; \
    ninja -C ft install; rm -rf ft freetype-2.13.3; \
    tar -xzf sdl.tar.gz; \
    cmake -S SDL3-3.2.26 -B sdl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/deps \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF; \
    ninja -C sdl install; rm -rf sdl SDL3-3.2.26

# Link-time-only copies of host libraries the game links dynamically (GL,
# GLib): the player's own copies are used at run time.
# (libopengl-dev/libglx-dev provide the libOpenGL.so/libGLX.so links; they sit
# here rather than in the first apt layer so adding them kept the GCC layer.)
RUN set -e; apt-get update; apt-get install -y --no-install-recommends libopengl-dev libglx-dev; \
    rm -rf /var/lib/apt/lists/*; \
    mkdir -p /deps/hostlink/lib /deps/hostlink/include; \
    for l in libgio-2.0 libgobject-2.0 libglib-2.0 libOpenGL libGLX; do \
      cp -L /usr/lib/x86_64-linux-gnu/$l.so /deps/hostlink/lib/$l.so; \
    done; \
    cp -a /usr/include/GL /usr/include/KHR /deps/hostlink/include/; \
    mkdir -p /deps/hostlink/include/X11; \
    cp -a /usr/include/X11/keysym.h /usr/include/X11/keysymdef.h /deps/hostlink/include/X11/

# The runtime and the game build with the bundled GCC against /deps only, as on
# a player's machine.  (The /deps libraries above use Ubuntu's own GCC, which
# sees the X11/Wayland/audio headers they need.)
ENV CC=/opt/tc/bin/gcc CXX=/opt/tc/bin/g++

WORKDIR /work
