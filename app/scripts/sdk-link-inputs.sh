# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
# Shared by app linker and CI debug stripping. PS5_SYSROOT and HB must be set.
SDK_HOMEBREW_ARCHIVES=()
for a in libharfbuzz libfribidi libfreetype \
         libpng16 libjpeg libwebp libsharpyuv \
         libcurl libpsl libssl libcrypto libiconv libxml2 libz libbz2 liblzma libzstd libm; do
    SDK_HOMEBREW_ARCHIVES+=("${HB}/lib/${a}.a")
done
CXX_RUNTIME=("${PS5_SYSROOT}/lib/libc++.a" "${PS5_SYSROOT}/lib/libc++abi.a" "${PS5_SYSROOT}/lib/libunwind.a")
LIBC_ARCHIVE="${PS5_SYSROOT}/lib/libc.a"
