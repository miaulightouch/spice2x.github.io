#!/usr/bin/env bash

set -euo pipefail
umask 077

if [[ -z "${OTA_SIGNING_PFX:-}" ]]; then
    exit 0
fi
if [[ -z "${OTA_SIGNING_PASSWORD+x}" ]]; then
    echo "OTA_SIGNING_PFX is set but OTA_SIGNING_PASSWORD is missing" >&2
    exit 1
fi

if ! command -v osslsigncode >/dev/null 2>&1; then
    echo "OTA_SIGNING_PFX is set but osslsigncode is unavailable" >&2
    exit 1
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
output_root="$root/bin/spice2x"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pfx="$work/signing.pfx"
if ! printf '%s' "$OTA_SIGNING_PFX" | base64 --decode >"$pfx"; then
    echo "OTA_SIGNING_PFX is not valid base64" >&2
    exit 1
fi
password_file="$work/password"
printf '%s\n' "$OTA_SIGNING_PASSWORD" >"$password_file"

# Fail before touching a release binary when the PFX is malformed, the password
# is wrong, or its public key is not the key pinned by the native verifier.
pinned_key_sha1=9ad5080b3935e589f1df340da93b7fa99de6409d
certificate="$work/signing-cert.pem"
if ! openssl pkcs12 -in "$pfx" -clcerts -nokeys -passin "file:$password_file" \
        -out "$certificate" 2>/dev/null; then
    openssl pkcs12 -legacy -in "$pfx" -clcerts -nokeys -passin "file:$password_file" \
        -out "$certificate" 2>/dev/null || {
        echo "could not read OTA signing PFX (check OTA_SIGNING_PASSWORD)" >&2
        exit 1
    }
fi
actual_key_sha1=$(openssl x509 -in "$certificate" -pubkey -noout 2>/dev/null \
    | openssl pkey -pubin -outform DER 2>/dev/null \
    | sha1sum | awk '{print $1}') || {
    echo "could not read OTA signing PFX (check OTA_SIGNING_PASSWORD)" >&2
    exit 1
}
if [[ "$actual_key_sha1" != "$pinned_key_sha1" ]]; then
    echo "OTA signing PFX public key does not match the pinned release key" >&2
    exit 1
fi

sign_help=$(osslsigncode sign --help 2>&1 || true)
if [[ "$sign_help" != *"-readpass"* ]]; then
    echo "osslsigncode must support -readpass to protect OTA_SIGNING_PASSWORD" >&2
    exit 1
fi
sign_password_args=(-readpass "$password_file")

signed_sources=()
signed_outputs=()
sign_one() {
    local source=$1
    [[ -f "$source" ]] || return 0

    local output="$work/signed-${#signed_sources[@]}-$(basename "$source")"
    if ! osslsigncode sign -pkcs12 "$pfx" "${sign_password_args[@]}" \
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
