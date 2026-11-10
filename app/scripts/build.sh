#!/usr/bin/env bash
# Jelly5 (based on Nuvio PS5, Copyright (C) 2026 Husam Osman)
# SPDX-License-Identifier: GPL-3.0-or-later
# =============================================================================
# app/scripts/build.sh - build the Nuvio app (PPSA99176): the web UI
# host plus the Nuvio Player (EVO Player's playback engine, vendored in
# engine/).
#
# Runs with the PS5 payload SDK and the pacbrew sysroot (FFmpeg, dav1d,
# OpenSSL) set up by scripts/setup-toolchain.sh (macOS or Linux), or inside a
# container image that has them. From the repository's app/ folder:
#
#   ./scripts/build.sh            # development build (in the container when
#                                 # NUVIO_BUILD_IMAGE is set)
#   ./scripts/build.sh --ffpfsc   # also pack NAVI00001.ffpfsc
#   ./scripts/build.sh --release  # for sharing: no .env.local server, no log
#                                 # target, packed as .ffpfsc and .zip
#   JELLY5_AV1=0 ./scripts/build.sh  # without AV1: the sysroot's FFmpeg 7.0,
#                                 # and the server transcodes AV1 (by default
#                                 # FFmpeg 7.1 + libdav1d from
#                                 # toolchain/av1-prefix, scripts/build-av1.sh)
#
# Output: build/app/NAVI00001/{eboot.bin, sce_sys/, sce_module/libc.prx}
# The packaging steps follow the toolkit's app packaging (player mode);
# the loader constants and PRX stub rules there are hardware-validated.
# =============================================================================
set -euo pipefail
# Byte-order sorting for globs: under a Norwegian (or any non-C) locale
# libkernel_web.so sorts before libkernel.so and the link binds the kernel
# imports to the web process's kernel library, which an app cannot load.
export LC_ALL=C

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NUVIO_ROOT="$(cd "${APP_ROOT}/.." && pwd)"
EVO_ROOT="${EVO_ROOT:-${NUVIO_ROOT}/toolkit}"   # native-app toolkit (vendored)
IMAGE="${NUVIO_BUILD_IMAGE:-}"

FFPFSC=0; RELEASE=0
for arg in "$@"; do
    case "${arg}" in
        --ffpfsc) FFPFSC=1 ;;
        --release) RELEASE=1; FFPFSC=1 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "unknown option: ${arg}" >&2; exit 2 ;;
    esac
done

# --- Outside the container: re-exec inside it --------------------------------
# Builds with the PS5 Payload SDK on this machine (PS5_PAYLOAD_SDK). Set
# NUVIO_BUILD_IMAGE to run the same build inside a container image that ships it.
if [[ -n "${NUVIO_BUILD_IMAGE:-}" && ! -f /etc/profile.d/ps5-sdk.sh ]]; then
    exec docker run --rm --platform linux/amd64 \
        -v "${NUVIO_ROOT}:/nuvio" \
        -v evoplayer_ps5_sdk:/opt/ps5-payload-sdk \
        -v evoplayer_ccache:/ccache -e CCACHE_DIR=/ccache \
        -w /nuvio/app "${IMAGE}" \
        bash -lc "./scripts/build.sh $*"
fi

log()  { echo "==> $*"; }
ok()   { echo "  ok $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
need() { [[ -e "$1" ]] || die "$2"; }

: "${PS5_PAYLOAD_SDK:=/opt/ps5-payload-sdk}"
PS5_SYSROOT="${PS5_PAYLOAD_SDK}/target"
HB="${PS5_SYSROOT}/user/homebrew"
NATIVE="${EVO_ROOT}/tools/native-app"
# AV1: FFmpeg 7.1 with libdav1d from a prefix of its own (scripts/build-av1.sh),
# used instead of the sysroot's FFmpeg 7.0, which has no AV1 decoder that
# works on the PS5. On by default; JELLY5_AV1=0 builds without it (then AV1
# stays out of the device profile and the server transcodes it).
FF_LIB="${HB}/lib"; AV1_INC=""; AV1_ON=0
if [[ "${JELLY5_AV1:-1}" == 1 ]]; then
    AV1_ON=1
    AV1_PREFIX="${JELLY5_AV1_PREFIX:-${NUVIO_ROOT}/toolchain/av1-prefix}"
    need "${AV1_PREFIX}/lib/libdav1d.a" \
        "no AV1 decoder at ${AV1_PREFIX}: run scripts/build-av1.sh once (or build with JELLY5_AV1=0)"
    [[ "$(llvm-nm "${AV1_PREFIX}/lib/libavcodec.a" 2>/dev/null | grep -c ' D ff_libdav1d_decoder$')" -gt 0 ]] ||
        die "${AV1_PREFIX}/lib/libavcodec.a has no libdav1d decoder (scripts/build-av1.sh --force)"
    FF_LIB="${AV1_PREFIX}/lib"
    AV1_INC="-I${AV1_PREFIX}/include"
fi
LLD="$(command -v prospero-lld || echo "${PS5_PAYLOAD_SDK}/bin/prospero-lld")"
AR="$(command -v prospero-ar || echo "${PS5_PAYLOAD_SDK}/bin/prospero-ar")"
need "${NATIVE}/ps5-pie.ld" "native-app toolkit not found at ${NATIVE}"

# Hardware-validated loader profile.
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

TCC="prospero-clang"
TCXX="prospero-clang++"
TFLAGS=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)
if (( RELEASE )); then
    # Keep useful DWARF and __FILE__, but not local build roots. Make's TFLAGS
    # interface is whitespace-separated; reject unrepresentable roots explicitly.
    for root in "${NUVIO_ROOT}" "${EVO_ROOT}" "${PS5_PAYLOAD_SDK}" \
                "$(cd "${EVO_ROOT}" && pwd -P)" "$(cd "${PS5_PAYLOAD_SDK}" && pwd -P)"; do
        [[ "${root}" =~ ^[-/A-Za-z0-9_.:+]+$ ]] || die "release build root cannot be represented safely in Make TFLAGS"
    done
    TFLAGS+=(-fdebug-compilation-dir=/navi5/app
             "-ffile-prefix-map=${NUVIO_ROOT}=/navi5"
             "-ffile-prefix-map=${EVO_ROOT}=/navi5/toolkit"
             "-ffile-prefix-map=${PS5_PAYLOAD_SDK}=/ps5-sdk")
    if (( AV1_ON )); then
        [[ "${AV1_PREFIX}" =~ ^[-/A-Za-z0-9_.:+]+$ ]] || die "release AV1 root cannot be represented safely in Make TFLAGS"
        TFLAGS+=("-ffile-prefix-map=${AV1_PREFIX}=/sdk"
                 "-ffile-prefix-map=$(cd "${AV1_PREFIX}" && pwd -P)=/sdk")
    fi
    # SDK/toolkit caches may be symlinks; clang can canonicalize include paths.
    TFLAGS+=("-ffile-prefix-map=$(cd "${PS5_PAYLOAD_SDK}" && pwd -P)=/ps5-sdk"
             "-ffile-prefix-map=$(cd "${EVO_ROOT}" && pwd -P)=/navi5/toolkit")
fi

BUILD="${APP_ROOT}/build"
PARAM="${APP_ROOT}/sce_sys/param.json"
TITLE_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "${PARAM}")"
APPDIR="${BUILD}/app/${TITLE_ID}"
mkdir -p "${BUILD}/obj" "${BUILD}/host" "${BUILD}/stubs"

# --- Host converter + runtime shim ---------------------------------------------
log "native-app tools"
eval "$("${EVO_ROOT}/scripts/setup-native-app-deps.sh")"
TOOL="${BUILD}/host/ps5-native-tool"
if [[ ! -x "${TOOL}" ]] || find "${NATIVE}" -maxdepth 1 \( -name '*.cpp' -o -name '*.hpp' \) -newer "${TOOL}" -print -quit | grep -q .; then
    clang++ -std=c++20 -O2 -I "${ZLIB_INCLUDE}" \
        "${NATIVE}/native_app_builder.cpp" "${NATIVE}/self_container.cpp" \
        "${NATIVE}/elf_object.cpp" "${NATIVE}/sce_module_writer.cpp" \
        "${ZLIB_ARCHIVE}" -o "${TOOL}"
fi
LIBC_PRX="${NATIVE}/runtime/libc.prx"
need "${LIBC_PRX}" "libc.prx missing from the toolkit"
ok "converter + libc.prx"

# --- Compile ---------------------------------------------------------------------
log "compiling (${TCC})"
# Development log destination only; no server credentials in build flags.
ENV_LOCAL="${NUVIO_ROOT}/.env.local"
PS5_HOST=""
(( RELEASE )) && ENV_LOCAL=/dev/null   # nothing personal in a shared build
[[ -f "${ENV_LOCAL}" ]] && eval "$(grep -E '^PS5_HOST=' "${ENV_LOCAL}")"
LOG_HOST="${JELLY5_LOG_HOST:-}"
(( RELEASE )) && LOG_HOST=""
if [[ -z "${LOG_HOST}" && -n "${PS5_HOST}" ]]; then
    if [[ "$(uname -s)" == "Darwin" ]]; then
        IFACE="$(route -n get "${PS5_HOST}" 2>/dev/null | awk '/interface:/{print $2}')"
        [[ -n "${IFACE}" ]] && LOG_HOST="$(ipconfig getifaddr "${IFACE}" 2>/dev/null || true)"
    else   # Linux: the source address of the route to the console
        LOG_HOST="$(ip route get "${PS5_HOST}" 2>/dev/null |
            awk '{for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit }}' || true)"
    fi
fi
JELLY5_DEFS="-DNAVI5_CA_BUNDLE=\\\"/app0/assets/ca-bundle.pem\\\" -DJELLY5_VERSION=\\\"$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["contentVersion"])' "${PARAM}")\\\""
(( AV1_ON )) && JELLY5_DEFS+=" -DJELLY5_AV1=1"
BUILD_ID="$(git -C "${NUVIO_ROOT}" describe --always --dirty 2>/dev/null || echo unknown)"
JELLY5_DEFS+=" -DNAVI5_BUILD_ID=\\\"${BUILD_ID}\\\""
if [[ "${NAVI5_DEV_TOOLS:-0}" == 1 ]] && (( ! RELEASE )); then
    JELLY5_DEFS+=" -DNAVI5_DEV_TOOLS=1"
fi
# JELLY5_REMOTE_LOG=1: a remote test build (issue #12), raw remote samples in
# /download0/jelly5/remote-log.txt.
if (( ! RELEASE )) && [[ -n "${JELLY5_REMOTE_LOG:-}" ]]; then
    JELLY5_DEFS+=" -DJELLY5_REMOTE_LOG=1"
fi
[[ -n "${LOG_HOST}" ]] && JELLY5_DEFS+=" -DJELLY5_LOG_HOST=\\\"${LOG_HOST}\\\" -DJELLY5_LOG_PORT=${JELLY5_LOG_PORT:-5555}"
[[ -z "${LOG_HOST}" ]] && ok "UDP log disabled" || ok "UDP log configured (private destination)"
(( AV1_ON )) && ok "AV1: FFmpeg + dav1d from ${FF_LIB%/lib}"
make -C "${APP_ROOT}" -j"$(nproc)" objects \
    CC="${TCC}" CXX="${TCXX}" TFLAGS="${TFLAGS[*]}" HB="${HB}" JELLY5_DEFS="${JELLY5_DEFS}" AV1_INC="${AV1_INC}" \
    > "${BUILD}/compile.log" 2>&1 || { tail -40 "${BUILD}/compile.log"; die "compile failed"; }
mapfile -t OBJS < <(make -C "${APP_ROOT}" -s print-objects | tr ' ' '\n' | grep -E '\.o$')
ok "compiled ${#OBJS[@]} objects"

# Runtime pieces from EVO's native-app kit: CRT, C++ allocation runtime, the
# libc gap fillers, getaddrinfo on the console resolver, the mmap malloc.
"${TCXX}" -std=c++20 -O2 -fno-exceptions -fno-rtti "${TFLAGS[@]}" \
    -c "${NATIVE}/app_crt.cpp" -o "${BUILD}/obj/app_crt.o"
"${TCXX}" -std=c++20 -O2 -fno-exceptions -fno-rtti "${TFLAGS[@]}" \
    -c "${NATIVE}/app_cpp_runtime.cpp" -o "${BUILD}/obj/app_cpp_runtime.o"
"${AR}" rcs "${BUILD}/obj/libpthread.a" "${BUILD}/obj/app_cpp_runtime.o"
"${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" -c "${NATIVE}/stubs/libc_ext.c" -o "${BUILD}/obj/libc_ext.o"
"${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" -c "${NATIVE}/stubs/evo_dns.c" -o "${BUILD}/obj/evo_dns.o"
# Nuvio's copy: 64-byte-aligned small blocks come from slabs (see its header).
"${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" -c "${APP_ROOT}/runtime/malloc_shim.c" -o "${BUILD}/obj/malloc_shim.o"
RUNTIME_OBJS=("${BUILD}/obj/malloc_shim.o" "${BUILD}/obj/app_crt.o" "${BUILD}/obj/app_cpp_runtime.o"
              "${BUILD}/obj/libc_ext.o" "${BUILD}/obj/evo_dns.o")

# --- PRX import stubs -------------------------------------------------------------
# System modules the SDK has no link stub for. Each becomes a positional
# DT_NEEDED, so a stub that nothing imports would make the loader refuse the
# app: only the symbols our objects really import go in.
log "PRX import stubs"
UNDEF="${BUILD}/obj-undef.txt"
for o in "${OBJS[@]}" "${RUNTIME_OBJS[@]}"; do
    llvm-nm -u "${o}" 2>/dev/null | grep -oE '\bsce[A-Za-z0-9_]+' || true
done | sort -u > "${UNDEF}"
PRX_STUB_SOS=()
for base in libSceAudiodec libSceAgc libSceAgcDriver libSceCommonDialog \
            libSceWebBrowserDialog; do
    csrc="${BUILD}/stubs/${base}.c"
    : > "${csrc}"
    while read -r s; do
        case "${base}" in
            libSceAgcDriver) [[ "${s}" == sceAgcDriver* ]] || continue ;;
            libSceAgc)       [[ "${s}" == sceAgc* && "${s}" != sceAgcDriver* ]] || continue ;;
            libSceAudiodec)  [[ "${s}" == sceAudiodec* ]] || continue ;;
            libSceCommonDialog) [[ "${s}" == sceCommonDialog* ]] || continue ;;
            libSceWebBrowserDialog) [[ "${s}" == sceWebBrowserDialog* ]] || continue ;;
        esac
        echo "void ${s}(void){}" >> "${csrc}"
    done < "${UNDEF}"
    [[ -s "${csrc}" ]] || continue
    so="${BUILD}/stubs/${base}.so"
    "${TCC}" -shared -nostdlib -nodefaultlibs -fPIC -Wl,-soname,"${base}.sprx" -o "${so}" "${csrc}"
    PRX_STUB_SOS+=("${so}")
    printf '     %-28s %s syms\n' "${base}.sprx" "$(wc -l < "${csrc}")"
done

# --- Link ---------------------------------------------------------------------------
log "linking"
ARCHIVES=()
# Text shaping, artwork and FFmpeg audio/network libraries.
for a in libavformat libavcodec libswresample libavutil libswscale; do
    need "${FF_LIB}/${a}.a" "FFmpeg archive missing: ${FF_LIB}/${a}.a"
    ARCHIVES+=("${FF_LIB}/${a}.a")
done
(( AV1_ON )) && ARCHIVES+=("${FF_LIB}/libdav1d.a")
for a in libharfbuzz libfribidi libfreetype \
         libpng16 libjpeg libwebp libsharpyuv \
         libcurl libpsl libssl libcrypto libiconv libxml2 libz libbz2 liblzma libzstd libm; do
    need "${HB}/lib/${a}.a" "sysroot archive missing: ${a}.a"
    archive="${HB}/lib/${a}.a"
    if [[ "${a}" == libcurl ]]; then
        # Only pinned/audited curl callers, never global variadic interception.
        python3 - "${archive}" <<'PY'
import hashlib, sys
assert hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest() == 'c29815b2e111e12c68975626b7d53418533d0718b18ee229943081068c11527d', 'curl trace audit pin changed'
PY
        trace_args=(--redefine-sym fcntl=navi5_curl_fcntl)
        if [[ "${NAVI5_DEV_TOOLS:-0}" == 1 ]] && (( ! RELEASE )); then
            for symbol in socket setsockopt connect getsockopt poll send recv read write; do
                trace_args+=(--redefine-sym "${symbol}=navi5_trace_${symbol}")
            done
        fi
        llvm-objcopy "${trace_args[@]}" "${archive}" "${BUILD}/obj/libcurl-trace.a"
        archive="${BUILD}/obj/libcurl-trace.a"
    fi
    ARCHIVES+=("${archive}")
done
CXX_RUNTIME=("${PS5_SYSROOT}/lib/libc++.a" "${PS5_SYSROOT}/lib/libc++abi.a" "${PS5_SYSROOT}/lib/libunwind.a")
STUBDIR="${PS5_SYSROOT}/lib"
# SDK link stubs, minus the WebKit-process and system-process kernel variants
# (apps on 11.60 import libkernel only; see src/jelly5_libc_stubs.c).
SDK_STUBS=()
for so in "${STUBDIR}"/*.so; do
    case "$(basename "${so}")" in
        libkernel_web.so|libScePosixForWebKit.so|libkernel_sys.so) continue ;;
    esac
    SDK_STUBS+=("${so}")
done
LINK_LOG="${BUILD}/link.log"
link_app() {
    # --allow-multiple-definition: evo_dns.o (first on the line) supplies
    # getaddrinfo & co. on the console resolver; libc.a's netdb.o still comes
    # in for gethostbyname/getservbyname, which the WebKit stubs used to cover.
    "${LLD}" --reproduce="${BUILD}/relink.tar" -Map="${BUILD}/link.map" \
        -T "${NATIVE}/ps5-pie.ld" --eh-frame-hdr --allow-multiple-definition \
        --version-script "${NATIVE}/app-symbols.map" --exclude-libs=ALL --error-limit=0 \
        -L "${BUILD}/obj" -e _start -o "${BUILD}/llvm-pie.elf" \
        "${RUNTIME_OBJS[@]}" "${OBJS[@]}" "$@" "${PRX_STUB_SOS[@]}" \
        --start-group "${ARCHIVES[@]}" --end-group "${CXX_RUNTIME[@]}" \
        --as-needed "${SDK_STUBS[@]}" "${PRX_STUB_SOS[@]}" \
        --start-group "${PS5_SYSROOT}/lib/libc.a" --end-group \
        2> "${LINK_LOG}"
}
if ! link_app; then
    grep -oE "undefined symbol: .*" "${LINK_LOG}" | sed 's/^undefined symbol: //' \
        | sort -u > "${BUILD}/undefined-symbols.txt" || true
    if [[ -s "${BUILD}/undefined-symbols.txt" ]]; then
        echo "   $(wc -l < "${BUILD}/undefined-symbols.txt") undefined symbols (build/undefined-symbols.txt):"
        head -60 "${BUILD}/undefined-symbols.txt" | sed 's/^/     /'
    else
        tail -30 "${LINK_LOG}"
    fi
    die "link failed"
fi

# Runtime import check: the loader binds an import the console does not
# export to NULL, silently (strcasestr did that). Relink with a table of every
# import so the app can log the NULL ones at start (nuvio_import_check).
IMPORTS="${BUILD}/imports.txt"
llvm-nm "${BUILD}/llvm-pie.elf" | awk '$1=="U"{print $2}' | sort -u > "${IMPORTS}"
python3 - "${IMPORTS}" "${BUILD}/obj/import_check.c" <<'PY'
import sys
names = [n for n in open(sys.argv[1]).read().split() if n and not n.startswith("nuvio_import")]
c = ["/* Generated by scripts/build.sh: every import of the eboot. */"]
c += ["extern void %s(void);" % n for n in names]
c += ["static const void *const s_addr[] = {"] + ["    (const void *)&%s," % n for n in names] + ["};"]
c += ["static const char *const s_name[] = {"] + ['    "%s",' % n for n in names] + ["};"]
c += ["int nuvio_import_count(void) { return %d; }" % len(names),
      "const char *nuvio_import_null(int i) { return s_addr[i] ? 0 : s_name[i]; }"]
open(sys.argv[2], "w").write("\n".join(c) + "\n")
PY
"${TCC}" -std=gnu11 -O1 -fPIC -w "${TFLAGS[@]}" -c "${BUILD}/obj/import_check.c" -o "${BUILD}/obj/import_check.o"
link_app "${BUILD}/obj/import_check.o" || { tail -30 "${LINK_LOG}"; die "relink with the import check failed"; }
ok "linked $(stat -c %s "${BUILD}/llvm-pie.elf") bytes"
# Working apps on 11.60 import libkernel only: a kernel variant (by any name) dies before main.
if command -v llvm-readelf >/dev/null; then
    if llvm-readelf -d "${BUILD}/llvm-pie.elf" | grep NEEDED | grep -E 'libkernel_|PosixForWebKit'; then
        die "the app imports a kernel variant (above): it would die before main"
    fi
else
    echo "  warning: llvm-readelf not found: the kernel-import check was skipped"
fi

# --- Convert, sign, assemble ------------------------------------------------------------
log "converting + signing"
CONV_STUB_ARGS=()
for so in "${PRX_STUB_SOS[@]}"; do CONV_STUB_ARGS+=(--stub "${so}"); done
"${TOOL}" link --in "${BUILD}/llvm-pie.elf" --out "${BUILD}/eboot.elf" \
    --stub-dir "${STUBDIR}" "${CONV_STUB_ARGS[@]}" \
    --module-sdk "${MODULE_SDK}" --companion-sdk "${COMPANION_SDK}" --file-name eboot.elf
# The read-only segment must start on a 64 KB file offset, or the console reads
# one 64 KB block of .rodata from the block before it (see ps5-pie.ld).
if command -v llvm-readelf >/dev/null; then
    RO_OFFSET="$(llvm-readelf -lW "${BUILD}/eboot.elf" | awk '$1=="LOAD" && $7=="R" && $8 ~ /^0x/ && !found {print $2; found=1}')"
    [[ -n "${RO_OFFSET}" ]] || die "no read-only segment found in eboot.elf"
    (( (RO_OFFSET & 0xFFFF) == 0 )) ||
        die "read-only segment at file offset ${RO_OFFSET}, not a multiple of 64 KB: the console misreads .rodata"
    ok "read-only segment at ${RO_OFFSET}"
else
    echo "  warning: llvm-readelf not found: the .rodata offset check was skipped"
fi
rm -rf -- "${APPDIR}"
mkdir -p "${APPDIR}/sce_sys" "${APPDIR}/sce_module"
"${TOOL}" self --sign --in "${BUILD}/eboot.elf" --out "${APPDIR}/eboot.bin" --magic "${FSELF_MAGIC}"
cp "${PARAM}" "${APPDIR}/sce_sys/param.json"
cp "${LIBC_PRX}" "${APPDIR}/sce_module/libc.prx"
# Public Mozilla roots only; checksum and provenance in assets/CA-BUNDLE.md.
python3 "${NUVIO_ROOT}/scripts/check-smoke-package.py" --ca-only
mkdir -p "${APPDIR}/assets"
cp "${APP_ROOT}/assets/ca-bundle.pem" "${APPDIR}/assets/"
cp "${APP_ROOT}/assets/CA-BUNDLE.md" "${APP_ROOT}/licenses/ca-bundle-MPL-2.0.txt" "${APPDIR}/assets/"
# pic0/pic1.dds: the home screen's backgrounds (scripts/make_dds.py); the PNGs are PS4-style leftovers
for asset in icon0.png pic0.png pic1.png pic0.dds pic1.dds; do
    [[ -f "${APP_ROOT}/sce_sys/${asset}" ]] && cp "${APP_ROOT}/sce_sys/${asset}" "${APPDIR}/sce_sys/"
done
# Notices and licence texts inside the app, so folder, .ffpfsc and zip all carry them
# (app/licenses/README.md lists each text's pinned source; check-smoke-package.py enforces).
LIC="${APPDIR}/licenses"
mkdir -p "${LIC}"
cp "${NUVIO_ROOT}/LICENSE" "${NUVIO_ROOT}/THIRD_PARTY_NOTICES.md" "${LIC}/"
cp "${APP_ROOT}"/assets/fonts/*.txt "${APP_ROOT}"/licenses/* "${LIC}/"
cp "${APP_ROOT}/src/app/CLDR-LICENSE.txt" "${LIC}/"
# Template stays media 65536 (release). DEV only: game category 0 for Remote Play.
if [[ "${NAVI5_DEV_TOOLS:-0}" == 1 ]] && (( ! RELEASE )); then
    python3 "${NUVIO_ROOT}/scripts/check-smoke-package.py" --mark-dev "${APPDIR}"
fi
python3 "${NUVIO_ROOT}/scripts/check-smoke-package.py" "${APPDIR}"
"${TOOL}" self --inspect --file "${APPDIR}/eboot.bin" | grep -E "integrity|digest" || true

if (( FFPFSC )); then
    log "PFS-packing ${TITLE_ID}.ffpfsc"
    MKPFS="$("${EVO_ROOT}/scripts/setup-pfs-tool.sh")"
    rm -f -- "${BUILD}/app/${TITLE_ID}.ffpfsc"
    "${MKPFS}" pack folder --no-adjust-output-file-extension --version PS5 --verify \
        "${APPDIR}" "${BUILD}/app/${TITLE_ID}.ffpfsc"
fi
if (( RELEASE )); then
    VER="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["contentVersion"])' "${PARAM}")"
    rm -f -- "${BUILD}/app/Navi5-${VER}.zip"
    # Same texts beside the app too, readable without unpacking the .ffpfsc.
    rm -rf -- "${BUILD}/app/licenses" && cp -R "${LIC}" "${BUILD}/app/licenses"
    (cd "${BUILD}/app" && zip -qr "Navi5-${VER}.zip" "${TITLE_ID}" "${TITLE_ID}.ffpfsc" licenses)
    # every entry 0777: an unpacker that keeps the stored modes would leave 0644, and the
    # console refuses to start the app (CE-107750-0)
    python3 "${APP_ROOT}/scripts/zip-open-modes.py" "${BUILD}/app/Navi5-${VER}.zip" >/dev/null
    ok "release: ${BUILD#"${NUVIO_ROOT}/"}/app/Navi5-${VER}.zip"
fi
ok "app: ${APPDIR#"${NUVIO_ROOT}/"}/"
