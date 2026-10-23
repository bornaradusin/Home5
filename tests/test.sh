#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Home5's host checks, no console or PS5 SDK needed: the network code, the Home
# Assistant client, the layout and the screens against tests/fake_ha.py, over
# HTTP and HTTPS (a test CA), with the sanitizers on.
#
#   tests/test.sh [--shots]      --shots: also draws the screens into .build/tests/shots/
set -euo pipefail
here=$(realpath -- "$(dirname -- "$0")/..")
out=$here/.build/tests
source "$here/tools/pins.sh"
cd "$here"
rm -rf "$out" && mkdir -p "$out/work" "$out/tls" "$out/mbedtls"

# mbedTLS, as the app pins it, built for this computer.
tarball=$here/.build/downloads/mbedtls-$MBEDTLS_VERSION.tar.bz2
if [[ ! -f $tarball ]]; then
    mkdir -p "$(dirname "$tarball")"
    curl -fsSL -o "$tarball.part" "$MBEDTLS_URL" && mv "$tarball.part" "$tarball"
fi
echo "$MBEDTLS_SHA256  $tarball" | sha256sum --check --quiet
mbedtls=$here/.build/mbedtls-$MBEDTLS_VERSION
[[ -f $mbedtls/include/mbedtls/ssl.h ]] || tar -xjf "$tarball" -C "$here/.build"
config='-DMBEDTLS_USER_CONFIG_FILE="h5_mbedtls_config.h"'
host_lib=$here/.build/host-mbedtls/libmbedtls.a
stamp=$(cat src/h5_mbedtls_config.h tools/pins.sh | sha256sum | cut -c1-16)
if [[ ! -f $host_lib || $(cat "$here/.build/host-mbedtls/stamp" 2>/dev/null) != "$stamp" ]]; then
    rm -rf "$here/.build/host-mbedtls"
    mkdir -p "$here/.build/host-mbedtls/obj"
    for source in "$mbedtls"/library/*.c; do
        cc -std=c99 -O1 -I"$mbedtls/include" -I"$mbedtls/library" -Isrc "$config" -c "$source" \
            -o "$here/.build/host-mbedtls/obj/$(basename "$source" .c).o"
    done
    ar rcs "$host_lib" "$here"/.build/host-mbedtls/obj/*.o
    echo "$stamp" > "$here/.build/host-mbedtls/stamp"
fi

python3 tools/make-font.py "$out/fonts.bin" >/dev/null

# A test CA and a certificate from it for localhost.
(
    cd "$out/tls"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ca.key -out ca.pem -days 2 \
        -subj "/CN=Home5 Test CA" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign" 2>/dev/null
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout server.key -out server.csr \
        -subj "/CN=localhost" 2>/dev/null
    printf "subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n" > ext.cnf
    openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out server.pem -days 2 \
        -extfile ext.cnf 2>/dev/null
)

cc -std=c11 -Wall -Wextra -Wpedantic -Werror -D_DEFAULT_SOURCE -O1 -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -DH5_HEAP_ONLY -Isrc -I"$mbedtls/include" "$config" \
    tests/test_home5.c src/h5_base.c src/h5_draw.c src/h5_ha.c src/h5_json.c src/h5_layout.c src/h5_net.c src/h5_ui.c \
    "$host_lib" -lm -o "$out/test_home5"

plain=18123 tls=18443
python3 tests/fake_ha.py $plain "$out/ready-plain" &
server_plain=$!
python3 tests/fake_ha.py $tls "$out/ready-tls" "$out/tls/server.pem" "$out/tls/server.key" &
server_tls=$!
trap 'kill $server_plain $server_tls 2>/dev/null || true' EXIT
for _ in $(seq 100); do [[ -f $out/ready-plain && -f $out/ready-tls ]] && break; sleep 0.1; done

shots=-
if [[ ${1:-} == --shots ]]; then shots=$out/shots; mkdir -p "$shots"; fi
"$out/test_home5" "$out/work" "$out/fonts.bin" $plain $tls "$out/tls/ca.pem" "$shots"
"$out/test_home5" "$out/work" "$out/fonts.bin" $plain $tls "$out/tls/ca.pem" - --tls
if [[ $shots != - ]]; then
    python3 -c "
import pathlib, sys
from PIL import Image
for p in sorted(pathlib.Path(sys.argv[1]).glob('*.ppm')): Image.open(p).save(p.with_suffix('.png')); p.unlink()" "$shots"
fi
python3 -m py_compile tools/make-font.py tests/fake_ha.py art/make-art.py
bash -n tools/build.sh
echo "host checks passed"
