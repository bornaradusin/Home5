#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds Home5 for the PS5 and packages it.
#
#   tools/build.sh
#
# Into release/ (or $HOME5_RELEASE_DIR):
#   PPSA11019.zip              the app folder, for the PS5
#   PPSA11019-sources.tar.xz   everything it is built from (Home5, the boilerplate, mbedTLS) and where the rest comes from
#   Home5-<version>-source.zip Home5's own source, as in its repository
#   SHA256SUMS.txt
#
# Built on the PS5 Native App Boilerplate (pinned in tools/pins.sh), which brings
# the PS5 payload SDK, the console's start-up code and the signing. Needs Linux with
# git, curl, wget, unzip, zip, tar, xz, make, clang-18 and llvm-18, and python3 with
# Pillow and NumPy. Every download is pinned and checked. The logs are in .build/logs/.
set -euo pipefail
here=$(realpath -- "$(dirname -- "$0")/..")
source "$here/tools/pins.sh"
B=$here/.build
bp=$B/bp
out=${HOME5_RELEASE_DIR:-$here/release}
version=$(sed -n 's/^#define H5_VERSION "\(.*\)"$/\1/p' "$here/src/h5.h")
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "src/h5.h has no H5_VERSION" >&2; exit 2; }
for command in git curl wget unzip zip tar xz make clang-18 llvm-ar-18 llvm-readelf-18 python3; do
    command -v "$command" >/dev/null || { echo "missing: $command" >&2; exit 2; }
done
python3 -c 'import PIL, numpy' 2>/dev/null || { echo "missing: python3 Pillow and NumPy" >&2; exit 2; }
mkdir -p "$B/downloads" "$B/logs" "$out"

# A repository at one commit.
fetch() { # dir url commit
    [[ -d $1/.git ]] || git init -q "$1"
    if ! git -C "$1" cat-file -e "$3^{commit}" 2>/dev/null; then
        git -C "$1" fetch -q --depth 1 "$2" "$3" || git -C "$1" fetch -q "$2"
    fi
    git -C "$1" -c advice.detachedHead=false checkout -q --force "$3"
}
# A file downloaded once and checked against its SHA-256.
download() { # file url sha256
    if [[ ! -f $1 ]] || ! echo "$3  $1" | sha256sum --check --status; then
        curl -fsSL --retry 3 -o "$1.part" "$2"
        echo "$3  $1.part" | sha256sum --check --status || { echo "$2 does not match its pinned SHA-256" >&2; rm -f "$1.part"; return 1; }
        mv "$1.part" "$1"
    fi
}
# One step: its output in its log, one line on screen, the log's end when it fails.
run() { # step command...
    local step=$1 started=$SECONDS status=0; shift
    printf '==> %-10s ' "$step"
    set +e
    ( set -euo pipefail; "$@" ) > "$B/logs/$step.log" 2>&1
    status=$?
    set -e
    if (( status == 0 )); then
        printf 'done (%ds)\n' $(( SECONDS - started ))
    else
        printf 'FAILED, the end of .build/logs/%s.log:\n' "$step"
        tail -25 "$B/logs/$step.log"
        exit 1
    fi
}
sdk=$bp/.deps/native/ps5-payload-sdk
clang_ps5() { PS5_PAYLOAD_SDK="$sdk" sh "$bp/tooling/prospero-clang18" "$@"; }

# ---- the steps ---------------------------------------------------------------------------------------------------

# The boilerplate at its pin, clean (its downloaded SDK and generated runtime kept), and its SDK.
step_boilerplate() {
    fetch "$bp" "$BOILERPLATE_URL" "$BOILERPLATE_COMMIT"
    git -C "$bp" clean -q -fdx -e .deps/ -e runtime/libc.prx
    bash "$bp/tools/setup-native-dependencies.sh"
    git -C "$bp" log -1 --format='boilerplate %H %s'
}

# mbedTLS for the console, with Home5's settings (src/h5_mbedtls_config.h).
step_mbedtls() {
    local tarball=$B/downloads/mbedtls-$MBEDTLS_VERSION.tar.bz2 mbedtls=$B/mbedtls-$MBEDTLS_VERSION obj=$B/mbedtls-ps5
    local config='-DMBEDTLS_USER_CONFIG_FILE="h5_mbedtls_config.h"'
    download "$tarball" "$MBEDTLS_URL" "$MBEDTLS_SHA256"
    [[ -f $mbedtls/include/mbedtls/ssl.h ]] || tar -xjf "$tarball" -C "$B"
    local stamp
    stamp=$(cat "$here/src/h5_mbedtls_config.h" "$here/tools/pins.sh" "$bp/tooling/prospero-clang18" | sha256sum | cut -c1-16)
    if [[ ! -f $obj/libmbedtls.a || $(cat "$obj/stamp" 2>/dev/null) != "$stamp" ]]; then
        rm -rf "$obj" && mkdir -p "$obj"
        for source in "$mbedtls"/library/*.c; do
            clang_ps5 -std=c99 -O2 -ffunction-sections -fdata-sections -I"$mbedtls/include" -I"$mbedtls/library" \
                -I"$here/src" "$config" -c "$source" -o "$obj/$(basename "$source" .c).o"
        done
        llvm-ar-18 rcs "$obj/libmbedtls.a" "$obj"/*.o
        echo "$stamp" > "$obj/stamp"
    fi
    mkdir -p "$bp/vendor/lib" "$bp/vendor/mbedtls"
    cp "$obj/libmbedtls.a" "$bp/vendor/lib/libmbedtls.a"
    rm -rf "$bp/vendor/mbedtls/include" && cp -r "$mbedtls/include" "$bp/vendor/mbedtls/include"
}

# Home5 in the boilerplate's place: its code, its files, its artwork and its param.json (under Media).
step_app() {
    rm -rf "$bp/src" "$bp/assets" && mkdir -p "$bp/src" "$bp/assets"
    cp "$here"/src/*.c "$here"/src/*.h "$bp/src/"
    python3 "$here/tools/make-font.py" "$bp/assets/fonts.bin"
    # Mozilla's root certificates, as the build computer's ca-certificates package has them, after the console's own.
    local roots=(/usr/share/ca-certificates/mozilla/*.crt)
    (( ${#roots[@]} > 100 )) || { echo "no Mozilla root certificates in /usr/share/ca-certificates/mozilla" >&2; return 1; }
    for root in "${roots[@]}"; do cat "$root"; echo; done > "$bp/assets/ca-certificates.crt"
    echo "certificates: ${#roots[@]} from ca-certificates $(dpkg-query -W -f='${Version}' ca-certificates 2>/dev/null || echo '?')"
    # The boilerplate's own pictures and sound go; Home5's come in.
    rm -f "$bp"/sce_sys/*.png "$bp"/sce_sys/*.dds "$bp"/sce_sys/snd0.at9
    python3 "$here/art/make-art.py" "$bp/sce_sys"
    # Home5's own param.json (sce_sys/), checked: its title, Media, and the version in src/h5.h.
    cp "$here/sce_sys/param.json" "$bp/sce_sys/param.json"
    python3 - "$bp/sce_sys/param.json" "$version" "$TITLE_ID" <<'PY'
import json, sys
path, version, title_id = sys.argv[1:4]
param = json.load(open(path, encoding="utf-8"))
major, minor, patch = (int(n) for n in version.split("."))
want = f"{major:02d}.{minor:03d}.{patch:03d}"
assert param["titleId"] == title_id, f"sce_sys/param.json: titleId {param['titleId']}, not {title_id}"
assert param["contentVersion"] == want, f"sce_sys/param.json: contentVersion {param['contentVersion']}, src/h5.h says {want}"
assert param["applicationCategoryType"] == 65536 and param["contentBadgeType"] == 2 and "gameIntent" not in param, \
    "sce_sys/param.json: Home5 belongs under Media"
print(json.dumps(param, indent=2))
PY
}

# libSceCommonDialog's sceCommonDialogInitialize (the PS5's keyboard needs it), which the SDK has no
# stub for: a stub of Home5's own, and the boilerplate's link told about it.
step_stub() {
    local stubs=$bp/vendor/stubs work=$B/stub
    rm -rf "$stubs" "$work" && mkdir -p "$stubs" "$work"
    printf 'int sceCommonDialogInitialize(void);\nint sceCommonDialogInitialize(void) { return 0; }\n' > "$work/common_dialog.c"
    clang_ps5 -fPIC -c "$work/common_dialog.c" -o "$work/common_dialog.o"
    "$sdk/bin/prospero-lld" --no-pie -shared -soname libSceCommonDialog.sprx -o "$stubs/libSceCommonDialog.so" "$work/common_dialog.o"
    llvm-readelf-18 -d --dyn-syms "$stubs/libSceCommonDialog.so" | grep -E 'SONAME|sceCommonDialogInitialize'
    local script=$bp/tools/build.sh
    sed -i 's#--as-needed "$sdk_root"/target/lib/\*\.so$#--as-needed "$sdk_root"/target/lib/*.so "$root"/vendor/stubs/*.so#' "$script"
    sed -i 's#--stub-dir "$sdk_root/target/lib" --module-sdk#--stub-dir "$sdk_root/target/lib" --stub "$root/vendor/stubs/libSceCommonDialog.so" --module-sdk#' "$script"
    [[ $(grep -c 'vendor/stubs' "$script") == 2 ]] || { echo "the boilerplate's link was not changed as expected" >&2; return 1; }
    git -C "$bp" diff --stat
}

step_compile() {
    (cd "$bp" && make app TITLE_ID="$TITLE_ID" APP_INCLUDE_PATHS="src vendor/mbedtls/include" \
        APP_STATIC_ARCHIVES=vendor/lib/libmbedtls.a)
}

# What the app asks of the system: only these modules (a wrong stub would pull in another).
step_check() {
    local app=$bp/dist/$TITLE_ID needed
    needed=$(llvm-readelf-18 -d "$bp/build/llvm-pie.elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort)
    echo "$needed"
    local allowed=" libkernel.prx libSceLibcInternal.prx libSceVideoOut.prx libScePad.prx libSceUserService.prx \
libSceSystemService.prx libSceNet.prx libSceImeDialog.prx libSceCommonDialog.prx libSceSysmodule.prx "
    for module in $needed; do
        module=${module%.sprx}; module=${module%.prx}.prx
        [[ $allowed == *" $module "* ]] || { echo "unexpected module: $module" >&2; return 1; }
    done
    # Functions the console's libraries may not have, which Clang or mbedTLS would otherwise call.
    local imports
    imports=$(llvm-nm-18 -D --undefined-only "$bp/build/llvm-pie.elf" | awk '{print $2}' | sort)
    echo "imports: $(echo $imports)"
    for symbol in sincos sincosf __inet_pton inet_pton getaddrinfo __udivti3 __divti3 __umodti3 __modti3; do
        ! grep -qx "$symbol" <<< "$imports" || { echo "imports $symbol" >&2; return 1; }
    done
    for file in eboot.bin sce_module/libc.prx sce_sys/param.json sce_sys/icon0.png sce_sys/pic0.dds sce_sys/pic1.dds \
        assets/fonts.bin assets/ca-certificates.crt; do
        [[ -s $app/$file ]] || { echo "missing from the app: $file" >&2; return 1; }
    done
    python3 - "$app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
assert (p["applicationCategoryType"], p["contentBadgeType"]) == (65536, 2), "not under Media"
print("param.json:", p["titleId"], p["contentId"], p["contentVersion"], "Media",
      p["localizedParameters"][p["localizedParameters"]["defaultLanguage"]]["titleName"])
PY
    find "$app" -type f -printf '%10s %P\n' | sort -k2
}

step_package() {
    local stage=$B/package name=Home5-$version
    rm -rf "$stage" && mkdir -p "$stage"
    rm -f "$out/$TITLE_ID.zip" "$out/$TITLE_ID-sources.tar.xz" "$out/$name-source.zip" "$out/SHA256SUMS.txt"
    # The app folder, as it goes on the PS5.
    (cd "$bp/dist" && zip -qrX "$out/$TITLE_ID.zip" "$TITLE_ID")
    # Home5's source: what its repository has (what git would commit, without the build).
    mkdir -p "$stage/$name"
    (cd "$here" && if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files -z --cached --others --exclude-standard
    else   # from a source package: all but what the build makes
        find . -type f -not -path './.build/*' -not -path './release/*' -not -path '*/__pycache__/*' -printf '%P\0'
    fi | xargs -0 cp --parents -t "$stage/$name")
    (cd "$stage" && zip -qrX "$out/$name-source.zip" "$name")
    # Everything the app is built from.
    local src=$stage/$TITLE_ID-sources
    mkdir -p "$src"
    cp -a "$stage/$name" "$src/$name"
    git -C "$bp" archive --format=tar --prefix="ps5-native-app-boilerplate-${BOILERPLATE_COMMIT:0:7}/" "$BOILERPLATE_COMMIT" \
        | tar -x -C "$src"
    cp "$B/downloads/mbedtls-$MBEDTLS_VERSION.tar.bz2" "$src/"
    local sdk_line zlib_line
    sdk_line=$(sed -n 's/^sdk_url="\(.*\)"$/\1/p; s/^sdk_hash="\(.*\)"$/  sha256 \1/p' "$bp/tools/setup-native-dependencies.sh" | paste -sd' ')
    zlib_line=$(sed -n 's/^zlib_version="\(.*\)"$/zlib \1/p; s/^zlib_hash="\(.*\)"$/  sha256 \1/p' "$bp/tools/setup-native-dependencies.sh" | paste -sd' ')
    cat > "$src/PARTS.txt" <<EOF
Home5 $version ($TITLE_ID) for the PS5: what it is built from.

$name/                          Home5 (GPL-3.0-or-later). tools/build.sh builds it all.
ps5-native-app-boilerplate-${BOILERPLATE_COMMIT:0:7}/   PS5 Native App Boilerplate, $BOILERPLATE_URL
                                commit $BOILERPLATE_COMMIT (GPL-3.0-or-later): the start-up code,
                                the runtime module (sce_module/libc.prx) and the signing tools.
mbedtls-$MBEDTLS_VERSION.tar.bz2          Mbed TLS (Apache-2.0 OR GPL-2.0-or-later), $MBEDTLS_URL
                                sha256 $MBEDTLS_SHA256

Downloaded by the boilerplate (tools/setup-native-dependencies.sh) and used to compile, not in the app:
  PS5 payload SDK (headers, link stubs): $sdk_line
  $zlib_line (for the boilerplate's signing tool)

In the app's assets/:
  fonts.bin                 Inter (SIL Open Font License 1.1), from $name/fonts/ by tools/make-font.py
  ca-certificates.crt       Mozilla's root certificates (MPL-2.0), from the build computer's
                            ca-certificates $(dpkg-query -W -f='${Version}' ca-certificates 2>/dev/null || echo '?') package
EOF
    (cd "$stage" && tar --sort=name --owner=0 --group=0 --numeric-owner -cJf "$out/$TITLE_ID-sources.tar.xz" "$TITLE_ID-sources")
    (cd "$out" && sha256sum "$TITLE_ID.zip" "$TITLE_ID-sources.tar.xz" "$name-source.zip" > SHA256SUMS.txt && cat SHA256SUMS.txt)
}

echo "Home5 $version ($TITLE_ID, under Media)"
run boilerplate step_boilerplate
run mbedtls step_mbedtls
run app step_app
run stub step_stub
run compile step_compile
run check step_check
run package step_package
ls -l "$out"
