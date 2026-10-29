#!/usr/bin/env bash

set -euo pipefail
umask 077

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ -f "$root/../../.env.local" ]]; then
    source "$root/../../.env.local"
fi

if [[ "${1:-}" == "--prepare" ]]; then
    pin=""
    if [[ -n "${OTA_SIGNING_CERT:-}" ]]; then
        pin=$(printf '%s\n' "$OTA_SIGNING_CERT" \
            | openssl x509 -pubkey -noout \
            | openssl pkey -pubin -outform DER \
            | sha1sum | awk '{print $1}')
    fi
    {
        printf '#pragma once\n#include <array>\n#include <cstdint>\nnamespace ota {\n'
        printf 'inline constexpr std::array<std::uint8_t, 20> kPinnedPublicKeySha1 = {'
        printf '%s' "$pin" | sed -E 's/(..)/0x\1, /g'
        printf '};\n}\n'
    } >"$root/build/ota_public_key.local.h"
    exit 0
fi

if (($#)); then
    echo "usage: $0 [--prepare]" >&2
    exit 1
fi
if [[ -z "${OTA_SIGNING_KEY:-}" && -z "${OTA_SIGNING_CERT:-}" ]]; then
    exit 0
fi
if [[ -z "${OTA_SIGNING_KEY:-}" || -z "${OTA_SIGNING_CERT:-}" ]]; then
    echo "OTA_SIGNING_KEY and OTA_SIGNING_CERT must both be set" >&2
    exit 1
fi

if ! command -v osslsigncode >/dev/null 2>&1; then
    echo "osslsigncode is required for release signing" >&2
    exit 1
fi

output_root="$root/bin/spice2x"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

key="$work/signing-key.pem"
certificate="$work/signing-cert.pem"
printf '%s\n' "$OTA_SIGNING_KEY" >"$key"
printf '%s\n' "$OTA_SIGNING_CERT" >"$certificate"

signed_sources=()
signed_outputs=()
sign_one() {
    local source=$1
    [[ -f "$source" ]] || return 0

    local output="$work/signed-${#signed_sources[@]}-$(basename "$source")"
    if ! osslsigncode sign -certs "$certificate" -key "$key" -pass "" \
            -h sha1 -in "$source" -out "$output" >/dev/null; then
        echo "could not sign $(basename "$source")" >&2
        exit 1
    fi
    signed_sources+=("$source")
    signed_outputs+=("$output")
}

sign_one "$output_root/spice.exe"
sign_one "$output_root/spice64.exe"
sign_one "$output_root/spicecfg.exe"
sign_one "$output_root/extras/largeaddressaware/spice.exe"
sign_one "$output_root/extras/winxp/spice.exe"
sign_one "$output_root/extras/winxp/spice64.exe"
sign_one "$output_root/extras/winxp/spicecfg.exe"
sign_one "$output_root/extras/linux/spice.exe"
sign_one "$output_root/extras/linux/spice64.exe"
sign_one "$output_root/extras/linux/spicecfg.exe"

if ((${#signed_sources[@]} == 0)); then
    echo "no Windows release binaries found under $output_root" >&2
    exit 1
fi
for index in "${!signed_sources[@]}"; do
    chmod --reference="${signed_sources[$index]}" "${signed_outputs[$index]}"
    mv -f "${signed_outputs[$index]}" "${signed_sources[$index]}"
done

archive_has_entry() {
    local archive=$1
    local entry=$2
    unzip -Z1 "$archive" | awk -v wanted="$entry" '$0 == wanted { found = 1 } END { exit !found }'
}

update_archive() {
    local archive=$1
    local source=$2
    local entry=$3
    [[ -f "$source" ]] || return 0
    archive_has_entry "$archive" "$entry" || return 0

    local staged="$work/archive/$entry"
    mkdir -p "$(dirname "$staged")"
    cp -p "$source" "$staged"
    (cd "$work/archive" && zip -q "$archive" "$entry")
}

shopt -s nullglob
for archive in "$root"/dist/*.zip; do
    update_archive "$archive" "$output_root/spice.exe" spice.exe
    update_archive "$archive" "$output_root/spice64.exe" spice64.exe
    update_archive "$archive" "$output_root/spicecfg.exe" spicecfg.exe
    update_archive "$archive" "$output_root/extras/largeaddressaware/spice.exe" \
        extras/largeaddressaware/spice.exe
    update_archive "$archive" "$output_root/extras/winxp/spice.exe" extras/winxp/spice.exe
    update_archive "$archive" "$output_root/extras/winxp/spice64.exe" extras/winxp/spice64.exe
    update_archive "$archive" "$output_root/extras/winxp/spicecfg.exe" extras/winxp/spicecfg.exe
    update_archive "$archive" "$output_root/extras/linux/spice.exe" extras/linux/spice.exe
    update_archive "$archive" "$output_root/extras/linux/spice64.exe" extras/linux/spice64.exe
    update_archive "$archive" "$output_root/extras/linux/spicecfg.exe" extras/linux/spicecfg.exe
done
