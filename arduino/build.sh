#!/bin/sh
# Builds the Arduino library from the ESP-IDF component, so the two never
# drift apart:
#
#   arduino/build.sh <output dir>
#
# One library for two architectures: the ESP32 (this component, as below) and
# the Raspberry Pi Pico W / Pico 2 W (arduino/rp2040, its own implementation
# of the same protocol and API). The Arduino IDE and PlatformIO compile every
# source in src/ whatever the board, so each file is wrapped in its
# architecture's #if; pairble.h picks the header. Shared by both: the text
# helpers, Improv's framing and the setup page.
#
# Arduino has no menuconfig, no EMBED_FILES and an older
# espressif/network_provisioning (1.0.2) precompiled into arduino-esp32 3.x.
# The library therefore carries:
#   - pairble_arduino_config.h: the menuconfig defaults, overridable by -D;
#   - the setup page as a source file;
#   - network_provisioning $NP_VERSION, whose definitions take precedence over
#     the precompiled copy when linked as object files (it has the connection
#     attempts setting pairble needs, and fixes for malformed messages).
set -eu

# 1.3.0 needs a newer json_generator than arduino-esp32 3.3 ships, and
# changes nothing pairble uses: 1.2.5 has every fix.
NP_VERSION=1.2.5
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
comp="$root/components/pairble"
out=${1:?usage: arduino/build.sh <output dir>}
mkdir -p "$out"
out=$(cd "$out" && pwd)
# Everything in it but .git is replaced: only an empty directory or an earlier
# output (a pairble-arduino checkout) will do.
if [ -n "$(ls -A "$out" | grep -v '^\.git$')" ] && ! grep -qs '^name=pairble$' "$out/library.properties"; then
    echo "refusing to clear $out: neither empty nor a pairble-arduino checkout" >&2
    exit 1
fi

version=$(sed -n 's/^version: *"\(.*\)"/\1/p' "$comp/idf_component.yml")
find "$out" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +  # keeps a checkout of pairble-arduino
mkdir -p "$out/src" "$out/examples"

# pairble itself
cp "$comp"/src/*.c "$comp"/src/*.h "$out/src/"
cp "$comp/include/pairble.h" "$out/src/pairble_esp32.h"
cp "$here"/src/* "$out/src/"   # pairble.h: picks pairble_esp32.h or pairble_rp2040.h
# NimBLE-Arduino's host, named outright. The component's __has_include picks
# it only once the library is on the include path, and the Arduino IDE and
# arduino-cli add a library when a file includes one of its headers - which
# pairble.c is compiled too early to see on the original ESP32, whose core has
# no NimBLE headers of its own to fall back to. nimconfig.h is at the top of
# NimBLE-Arduino's src/, so including it is what adds the library.
perl -0pi -e 's|#if __has_include\("nimble/nimble/host/include/host/ble_gap.h"\)\n(#include "nimble/nimble/host/include/host/ble_gap.h"[^\n]*)\n#else\n#include "host/ble_gap.h"\n#endif\n|#include "nimconfig.h"\n$1\n|' "$out/src/pairble.c"
grep -q '^#include "nimconfig.h"$' "$out/src/pairble.c" || { echo "pairble.c: NimBLE include block not found" >&2; exit 1; }
cp -R "$here/examples/." "$out/examples/"
cp "$root/LICENSE" "$root/NOTICE" "$here/README.md" "$out/"
printf '.pio/\n.DS_Store\n' > "$out/.gitignore"
for f in library.json library.properties; do
    sed "s/@VERSION@/$version/" "$here/$f" > "$out/$f"
done

# network_provisioning, from the ESP Component Registry
cache="${XDG_CACHE_HOME:-$HOME/.cache}/pairble"
zip="$cache/espressif__network_provisioning-v$NP_VERSION.zip"
if [ ! -f "$zip" ]; then
    mkdir -p "$cache"
    curl -fsSL -o "$zip.part" "https://components-file.espressif.com/components/espressif/network_provisioning/$NP_VERSION/espressif__network_provisioning-v$NP_VERSION.zip"
    mv "$zip.part" "$zip"
fi
np=$(mktemp -d)
trap 'rm -rf "$np"' EXIT
unzip -q "$zip" -d "$np"
for f in manager handlers network_config network_scan network_ctrl scheme_ble scheme_softap; do
    cp "$np/src/$f.c" "$out/src/np_$f.c"
done
cp "$np"/src/*.h "$np"/proto-c/* "$out/src/"
cp -R "$np/include/network_provisioning" "$out/src/"
cp "$np/LICENSE" "$out/LICENSE.network_provisioning"
# Quoted includes, resolved next to the including file first: never the
# precompiled 1.0.2 headers that arduino-esp32 puts on the include path.
perl -pi -e 's#<(network_provisioning/[a-z_]+\.h)>#"$1"#' "$out"/src/*.c "$out"/src/*.h
perl -pi -e 's#"network_provisioning/([a-z_]+\.h)"#"$1"#' "$out"/src/network_provisioning/*.h

# Bluetooth: protocomm's NimBLE transport from the same ESP-IDF, on
# NimBLE-Arduino's host, in place of the precompiled transport at link time.
# On the original ESP32 arduino-esp32 precompiles Bluedroid, which leaves too
# little RAM next to Wi-Fi (45 KB free with setup open, against 115 KB with
# NimBLE). Other chips have NimBLE precompiled, but NimBLE-Arduino - a
# dependency for the ESP32 - then replaces its host too, and the precompiled
# transport must not talk to a host it was not built against: so this
# transport everywhere, one combination on every chip.
IDF_COMMIT=b774170ff46   # arduino-esp32 3.3.x: ESP-IDF v5.5.5
for f in src/transports/protocomm_nimble.c src/common/protocomm_priv.h; do
    c="$cache/idf-$IDF_COMMIT-$(basename "$f")"
    if [ ! -f "$c" ]; then
        curl -fsSL -o "$c.part" "https://raw.githubusercontent.com/espressif/esp-idf/$IDF_COMMIT/components/protocomm/$f"
        mv "$c.part" "$c"
    fi
done
cp "$cache/idf-$IDF_COMMIT-protocomm_priv.h" "$out/src/protocomm_priv.h"
{
    printf '%s\n' \
        '/* protocomm'"'"'s BLE transport on NimBLE-Arduino, for every chip under Arduino' \
        ' * (see arduino/build.sh). ESP-IDF '"$IDF_COMMIT"', components/protocomm/' \
        ' * src/transports/protocomm_nimble.c, Apache-2.0, with NimBLE-Arduino'"'"'s include' \
        ' * paths. Generated - do not edit. */' \
        '#include "esp_idf_version.h"' \
        '#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)' \
        '#error "pairble needs arduino-esp32 3.3 or newer: this transport is ESP-IDF 5.5'"'"'s, and uses its private protocomm structures"' \
        '#endif' \
        '#include "nimconfig.h"' \
        '#include "esp_err.h"' \
        '/* ESP-IDF'"'"'s NimBLE config has it; NimBLE-Arduino'"'"'s does not, and without it' \
        ' * the GATT server and the advertised name are compiled out. */' \
        '#ifndef MYNEWT_VAL_BLE_GATTS' \
        '#define MYNEWT_VAL_BLE_GATTS (1)' \
        '#endif' \
        'esp_err_t pairble_nimble_port_init(void);  /* pairble_nimble.c */' \
        'esp_err_t pairble_nimble_port_deinit(void);' \
        'int pairble_nimble_port_stop(void);' \
        'void pairble_nimble_host_task(void *param);'
    sed -e 's#"nimble/nimble_port.h"#"nimble/porting/nimble/include/nimble/nimble_port.h"#' \
        -e 's#"nimble/nimble_port_freertos.h"#"nimble/porting/npl/freertos/include/nimble/nimble_port_freertos.h"#' \
        -e 's#"host/ble_hs.h"#"nimble/nimble/host/include/host/ble_hs.h"#' \
        -e 's#"host/ble_uuid.h"#"nimble/nimble/host/include/host/ble_uuid.h"#' \
        -e 's#"host/util/util.h"#"nimble/nimble/host/util/include/host/util/util.h"#' \
        -e 's#"services/gap/ble_svc_gap.h"#"nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"#' \
        -e 's#"services/gatt/ble_svc_gatt.h"#"nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"#' \
        -e 's#rc = nimble_port_init();#rc = pairble_nimble_port_init();#' \
        -e 's#nimble_port_deinit();#pairble_nimble_port_deinit();#' \
        -e 's#ret = nimble_port_stop();#ret = pairble_nimble_port_stop();#' \
        -e 's#nimble_port_freertos_init(nimble_host_task);#nimble_port_freertos_init(pairble_nimble_host_task);#' \
        "$cache/idf-$IDF_COMMIT-protocomm_nimble.c"
} > "$out/src/pairble_ble_nimble.c"

# The setup page the device serves on its own network (EMBED_FILES in ESP-IDF)
python3 - "$comp/web/setup.html.gz" "$out/src/pairble_setup_page.c" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
rows = ['    ".byte ' + ','.join(str(b) for b in data[i:i + 32]) + '\\n"'
        for i in range(0, len(data), 32)]
open(sys.argv[2], 'w').write(
    '/* The setup page (packages/web), in place of ESP-IDF\'s EMBED_FILES.\n'
    ' * Generated by arduino/build.sh - do not edit. */\n'
    '__asm__(\n'
    '    ".section .rodata.pairble_setup_page,\\"a\\"\\n"\n'
    '    ".global _binary_setup_html_gz_start\\n"\n'
    '    ".global _binary_setup_html_gz_end\\n"\n'
    '    "_binary_setup_html_gz_start:\\n"\n'
    + '\n'.join(rows) + '\n'
    '    "_binary_setup_html_gz_end:\\n"\n'
    '    ".byte 0\\n"\n'
    '    ".previous\\n");\n')
PY

# The Pico W
for f in "$here"/rp2040/*; do
    cp "$f" "$out/src/"
done

# Each source compiled only for its architecture; the shared three for both.
for f in "$out"/src/*.c "$out"/src/*.cpp; do
    name=$(basename "$f")
    case "$name" in
        pairble_text.c|pairble_improv_proto.c|pairble_setup_page.c) continue ;;
        pairble_pico*.cpp) cond='defined(ARDUINO_ARCH_RP2040)' ;;
        *) cond='defined(ESP_PLATFORM)' ;;
    esac
    { printf '#if %s  /* pairble: this architecture only (arduino/build.sh) */\n' "$cond"
      cat "$f"
      printf '\n#endif\n'; } > "$f.tmp"
    mv "$f.tmp" "$f"
done

echo "pairble $version for Arduino (ESP32, network_provisioning $NP_VERSION; Pico W) in $out"
