#include "ota_signature.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "build/ota_public_key.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>

#include "external/hash-library/sha1.h"
#include "external/hash-library/sha256.h"
#endif

namespace ota {
namespace {

Signature make_signature(std::string status, std::string detail,
                         std::string signer = {}) {
    return {std::move(status), std::move(signer), std::move(detail)};
}

#if defined(_WIN32)

constexpr DWORD kPkcsSignedData = 2;
constexpr std::size_t kPeSecurityDirectory = 4;
constexpr std::size_t kPeDirectoryCountOffset32 = 92;
constexpr std::size_t kPeDirectoryCountOffset64 = 108;
constexpr std::size_t kPeDirectoriesOffset32 = 96;
constexpr std::size_t kPeDirectoriesOffset64 = 112;
constexpr std::size_t kPeChecksumOffset = 64;

bool range_inside(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}

std::optional<std::uint16_t> read_u16(const std::vector<std::uint8_t> &bytes,
                                      std::size_t offset) {
    if (!range_inside(offset, sizeof(std::uint16_t), bytes.size())) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::optional<std::uint32_t> read_u32(const std::vector<std::uint8_t> &bytes,
                                      std::size_t offset) {
    if (!range_inside(offset, sizeof(std::uint32_t), bytes.size())) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[offset + 2]) << 16 |
           static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

struct PeLayout {
    std::size_t checksum_offset{};
    std::size_t security_directory_offset{};
    std::size_t security_directory_size{};
    std::size_t security_directory_field{};
};

enum class PeParseResult {
    valid,
    not_pe,
    malformed,
};

PeParseResult parse_pe(const std::vector<std::uint8_t> &bytes, PeLayout &layout,
                       std::string &detail) {
    if (!range_inside(0, 2, bytes.size()) || bytes[0] != 'M' || bytes[1] != 'Z') {
        detail = "file is not a PE image";
        return PeParseResult::not_pe;
    }

    const auto nt_offset_value = read_u32(bytes, 0x3c);
    if (!nt_offset_value || *nt_offset_value > bytes.size() ||
        !range_inside(*nt_offset_value, 4 + 20, bytes.size())) {
        detail = "truncated PE header";
        return PeParseResult::malformed;
    }
    const std::size_t nt_offset = *nt_offset_value;
    const auto signature = read_u32(bytes, nt_offset);
    if (!signature || *signature != 0x00004550) {
        detail = "invalid PE signature";
        return PeParseResult::not_pe;
    }

    const auto optional_size = read_u16(bytes, nt_offset + 4 + 16);
    if (!optional_size || !range_inside(nt_offset + 4 + 20, *optional_size, bytes.size())) {
        detail = "truncated PE optional header";
        return PeParseResult::malformed;
    }
    const std::size_t optional_offset = nt_offset + 4 + 20;
    const auto magic = read_u16(bytes, optional_offset);
    if (!magic || (*magic != 0x10b && *magic != 0x20b)) {
        detail = "unsupported PE optional header";
        return PeParseResult::malformed;
    }

    const std::size_t directory_count_offset =
        optional_offset + (*magic == 0x10b ? kPeDirectoryCountOffset32
                                           : kPeDirectoryCountOffset64);
    const std::size_t directories_offset =
        optional_offset + (*magic == 0x10b ? kPeDirectoriesOffset32
                                           : kPeDirectoriesOffset64);
    if (!range_inside(directory_count_offset, sizeof(std::uint32_t),
                      optional_offset + *optional_size) ||
        !range_inside(directories_offset,
                      (kPeSecurityDirectory + 1) * 2 * sizeof(std::uint32_t),
                      optional_offset + *optional_size)) {
        detail = "PE optional header has no security directory";
        return PeParseResult::malformed;
    }
    const auto directory_count = read_u32(bytes, directory_count_offset);
    if (!directory_count || *directory_count <= kPeSecurityDirectory) {
        layout.checksum_offset = optional_offset + kPeChecksumOffset;
        if (!range_inside(layout.checksum_offset, sizeof(std::uint32_t), bytes.size())) {
            detail = "truncated PE checksum";
            return PeParseResult::malformed;
        }
        layout.security_directory_field = 0;
        return PeParseResult::valid;
    }

    layout.checksum_offset = optional_offset + kPeChecksumOffset;
    layout.security_directory_field = directories_offset + kPeSecurityDirectory * 8;
    if (!range_inside(layout.checksum_offset, sizeof(std::uint32_t), bytes.size()) ||
        !range_inside(layout.security_directory_field, 2 * sizeof(std::uint32_t),
                      bytes.size())) {
        detail = "truncated PE security directory";
        return PeParseResult::malformed;
    }
    const auto security_offset = read_u32(bytes, layout.security_directory_field);
    const auto security_size = read_u32(bytes, layout.security_directory_field + 4);
    if (!security_offset || !security_size ||
        ((*security_offset == 0) != (*security_size == 0))) {
        detail = "invalid PE security directory";
        return PeParseResult::malformed;
    }
    layout.security_directory_offset = *security_offset;
    layout.security_directory_size = *security_size;
    if (layout.security_directory_size != 0 &&
        !range_inside(layout.security_directory_offset, layout.security_directory_size,
                      bytes.size())) {
        detail = "PE security directory is outside the file";
        return PeParseResult::malformed;
    }
    return PeParseResult::valid;
}

bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes,
               std::string &detail) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        detail = "could not read file size";
        return false;
    }
    if (file_size > std::numeric_limits<DWORD>::max() ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        detail = "file is too large for the Authenticode verifier";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        detail = "could not open file";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream && !stream.eof()) {
        detail = "could not read file";
        return false;
    }
    return true;
}

bool hash_image(const std::vector<std::uint8_t> &bytes, const PeLayout &layout,
                bool sha256, std::vector<std::uint8_t> &digest, std::string &detail) {
    if (!range_inside(layout.checksum_offset, sizeof(std::uint32_t), bytes.size())) {
        detail = "PE checksum is outside the file";
        return false;
    }
    if (layout.security_directory_size != 0 &&
        (layout.security_directory_field == 0 ||
         layout.security_directory_offset <
             layout.security_directory_field + 2 * sizeof(std::uint32_t))) {
        detail = "invalid PE security directory placement";
        return false;
    }

    const std::size_t image_end = layout.security_directory_size == 0
                                      ? bytes.size()
                                      : layout.security_directory_offset;
    if (image_end > bytes.size() || layout.security_directory_field > image_end) {
        detail = "invalid PE image range";
        return false;
    }
    if (layout.security_directory_field <
        layout.checksum_offset + sizeof(std::uint32_t)) {
        detail = "invalid PE directory placement";
        return false;
    }

    auto add_range = [&](auto &hasher, std::size_t offset, std::size_t length) {
        if (length != 0) {
            hasher.add(bytes.data() + offset, length);
        }
    };

    if (sha256) {
        SHA256 hasher;
        add_range(hasher, 0, layout.checksum_offset);
        add_range(hasher, layout.checksum_offset + sizeof(std::uint32_t),
                  layout.security_directory_field -
                      (layout.checksum_offset + sizeof(std::uint32_t)));
        add_range(hasher, layout.security_directory_field + 2 * sizeof(std::uint32_t),
                  image_end - (layout.security_directory_field + 2 * sizeof(std::uint32_t)));
        if (layout.security_directory_size != 0) {
            add_range(hasher, layout.security_directory_offset + layout.security_directory_size,
                      bytes.size() - (layout.security_directory_offset + layout.security_directory_size));
        }
        digest.resize(SHA256::HashBytes);
        hasher.getHash(digest.data());
    } else {
        SHA1 hasher;
        add_range(hasher, 0, layout.checksum_offset);
        add_range(hasher, layout.checksum_offset + sizeof(std::uint32_t),
                  layout.security_directory_field -
                      (layout.checksum_offset + sizeof(std::uint32_t)));
        add_range(hasher, layout.security_directory_field + 2 * sizeof(std::uint32_t),
                  image_end - (layout.security_directory_field + 2 * sizeof(std::uint32_t)));
        if (layout.security_directory_size != 0) {
            add_range(hasher, layout.security_directory_offset + layout.security_directory_size,
                      bytes.size() - (layout.security_directory_offset + layout.security_directory_size));
        }
        digest.resize(SHA1::HashBytes);
        hasher.getHash(digest.data());
    }
    return true;
}

std::string signer_name(PCCERT_CONTEXT certificate) {
    char name[256]{};
    const DWORD length = CertGetNameStringA(
        certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, sizeof(name));
    return length > 1 ? std::string(name, length - 1) : std::string();
}

bool public_key_matches(PCCERT_CONTEXT certificate, std::string &detail) {
    BYTE *encoded = nullptr;
    DWORD encoded_size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             &certificate->pCertInfo->SubjectPublicKeyInfo,
                             CRYPT_ENCODE_ALLOC_FLAG, nullptr, &encoded,
                             &encoded_size)) {
        detail = "could not encode signer public key";
        return false;
    }
    SHA1 hash;
    hash.add(encoded, encoded_size);
    std::array<std::uint8_t, SHA1::HashBytes> digest{};
    hash.getHash(digest.data());
    LocalFree(encoded);
    if (digest != kPinnedPublicKeySha1) {
        detail = "signer public key does not match the pinned release key";
        return false;
    }
    return true;
}

struct CertificateEntry {
    const BYTE *data{};
    DWORD size{};
};

std::vector<CertificateEntry> certificate_entries(const std::vector<std::uint8_t> &bytes,
                                                  const PeLayout &layout,
                                                  std::string &detail) {
    std::vector<CertificateEntry> entries;
    if (layout.security_directory_size == 0) {
        return entries;
    }
    std::size_t cursor = layout.security_directory_offset;
    const std::size_t end = cursor + layout.security_directory_size;
    while (cursor < end) {
        if (end - cursor < 8) {
            for (std::size_t i = cursor; i < end; ++i) {
                if (bytes[i] != 0) {
                    detail = "truncated PE certificate table";
                    return {};
                }
            }
            break;
        }
        const auto length = read_u32(bytes, cursor);
        if (!length || *length < 8 || *length > end - cursor) {
            detail = "invalid PE certificate length";
            return {};
        }
        const std::size_t aligned_length = (static_cast<std::size_t>(*length) + 7u) & ~7u;
        if (aligned_length > end - cursor) {
            detail = "unaligned PE certificate exceeds table";
            return {};
        }
        const auto revision = read_u16(bytes, cursor + 4);
        const auto type = read_u16(bytes, cursor + 6);
        if (!revision || !type) {
            detail = "invalid PE certificate header";
            return {};
        }
        if (*type == kPkcsSignedData && *revision != 0x0100 && *revision != 0x0200) {
            detail = "unsupported PKCS#7 certificate revision";
            return {};
        }
        if (*type == kPkcsSignedData) {
            entries.push_back({bytes.data() + cursor + 8, *length - 8});
        }
        cursor += aligned_length;
    }
    return entries;
}

Signature verify_pkcs7(const CertificateEntry &entry,
                      const std::vector<std::uint8_t> &image_digest) {
    if (entry.size == 0) {
        return make_signature("invalid", "empty PKCS#7 certificate");
    }

    CRYPT_DATA_BLOB blob{};
    blob.pbData = const_cast<BYTE *>(entry.data);
    blob.cbData = entry.size;
    DWORD message_encoding = 0;
    DWORD content_type = 0;
    DWORD format_type = 0;
    HCERTSTORE certificate_store = nullptr;
    HCRYPTMSG message = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &blob,
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &message_encoding,
                          &content_type, &format_type, &certificate_store,
                          &message, nullptr)) {
        return make_signature("invalid", "embedded certificate is not valid PKCS#7");
    }

    auto cleanup = [&] {
        if (message) {
            CryptMsgClose(message);
        }
        if (certificate_store) {
            CertCloseStore(certificate_store, 0);
        }
    };

    LONG signer_count = CryptGetMessageSignerCount(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, entry.data, entry.size);
    if (signer_count <= 0) {
        cleanup();
        return make_signature("invalid", "PKCS#7 message has no signer");
    }

    CRYPT_VERIFY_MESSAGE_PARA verify_parameters{};
    verify_parameters.cbSize = sizeof(verify_parameters);
    verify_parameters.dwMsgAndCertEncodingType =
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    DWORD decoded_size = 0;
    PCCERT_CONTEXT signer_certificate = nullptr;
    if (!CryptVerifyMessageSignature(&verify_parameters, 0, entry.data, entry.size,
                                     nullptr, &decoded_size, &signer_certificate) ||
        !signer_certificate) {
        if (signer_certificate) {
            CertFreeCertificateContext(signer_certificate);
        }
        cleanup();
        return make_signature("invalid", "PKCS#7 signer signature is invalid");
    }

    const std::string signer = signer_name(signer_certificate);
    std::string key_detail;
    const bool key_matches = public_key_matches(signer_certificate, key_detail);

    DWORD inner_type_size = 0;
    if (!CryptMsgGetParam(message, CMSG_INNER_CONTENT_TYPE_PARAM, 0, nullptr,
                          &inner_type_size) || inner_type_size == 0) {
        CertFreeCertificateContext(signer_certificate);
        cleanup();
        return make_signature("invalid", "PKCS#7 message has no Authenticode content type", signer);
    }
    std::vector<char> inner_type(static_cast<std::size_t>(inner_type_size) + 1);
    if (!CryptMsgGetParam(message, CMSG_INNER_CONTENT_TYPE_PARAM, 0, inner_type.data(),
                          &inner_type_size) ||
        std::strcmp(inner_type.data(), SPC_INDIRECT_DATA_OBJID) != 0) {
        CertFreeCertificateContext(signer_certificate);
        cleanup();
        return make_signature("invalid", "PKCS#7 message is not an Authenticode signature", signer);
    }

    DWORD content_size = 0;
    if (!CryptMsgGetParam(message, CMSG_CONTENT_PARAM, 0, nullptr, &content_size) ||
        content_size == 0) {
        CertFreeCertificateContext(signer_certificate);
        cleanup();
        return make_signature("invalid", "PKCS#7 message has no Authenticode content", signer);
    }
    std::vector<BYTE> content(content_size);
    if (!CryptMsgGetParam(message, CMSG_CONTENT_PARAM, 0, content.data(), &content_size)) {
        CertFreeCertificateContext(signer_certificate);
        cleanup();
        return make_signature("invalid", "could not read PKCS#7 content", signer);
    }

    PSPC_INDIRECT_DATA_CONTENT indirect = nullptr;
    DWORD indirect_size = 0;
    if (!CryptDecodeObjectEx(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            SPC_INDIRECT_DATA_CONTENT_STRUCT, content.data(), content_size,
            CRYPT_DECODE_ALLOC_FLAG, nullptr, &indirect, &indirect_size) ||
        !indirect || !indirect->Digest.pbData || indirect->Digest.cbData == 0) {
        if (indirect) {
            LocalFree(indirect);
        }
        CertFreeCertificateContext(signer_certificate);
        cleanup();
        return make_signature("invalid", "PKCS#7 content is not Authenticode data", signer);
    }
    if (!indirect->Data.pszObjId ||
        std::strcmp(indirect->Data.pszObjId, SPC_PE_IMAGE_DATA_OBJID) != 0 ||
        !indirect->Data.Value.pbData || indirect->Data.Value.cbData == 0) {
        LocalFree(indirect);
        CertFreeCertificateContext(signer_certificate);
        cleanup();
        return make_signature("invalid", "PKCS#7 content is not a PE image signature", signer);
    }

    bool digest_algorithm_supported = false;
    bool digest_matches = false;
    if (indirect->DigestAlgorithm.pszObjId &&
        std::strcmp(indirect->DigestAlgorithm.pszObjId, szOID_OIWSEC_sha1) == 0) {
        digest_algorithm_supported = true;
        digest_matches = image_digest.size() == SHA1::HashBytes &&
                         indirect->Digest.cbData == SHA1::HashBytes &&
                         std::equal(image_digest.begin(), image_digest.end(),
                                    indirect->Digest.pbData);
    } else if (indirect->DigestAlgorithm.pszObjId &&
               std::strcmp(indirect->DigestAlgorithm.pszObjId, szOID_NIST_sha256) == 0) {
        digest_algorithm_supported = true;
        digest_matches = image_digest.size() == SHA256::HashBytes &&
                         indirect->Digest.cbData == SHA256::HashBytes &&
                         std::equal(image_digest.begin(), image_digest.end(),
                                    indirect->Digest.pbData);
    }
    LocalFree(indirect);
    CertFreeCertificateContext(signer_certificate);
    cleanup();

    if (digest_algorithm_supported && !digest_matches) {
        return make_signature("invalid", "PE image digest does not match the signature", signer);
    }
    if (!digest_algorithm_supported) {
        return make_signature("unsupported", "signature digest algorithm is unsupported", signer);
    }
    if (!key_matches) {
        return make_signature("untrusted", key_detail, signer);
    }
    return make_signature("verified", "embedded signature, signer key, and PE image digest verified",
                          signer);
}

#endif

} // namespace

Signature verify_signature(const std::filesystem::path &path) {
#if !defined(_WIN32)
    (void) path;
    return make_signature("unsupported", "Authenticode verification requires Windows");
#else
    std::vector<std::uint8_t> bytes;
    std::string detail;
    if (!read_file(path, bytes, detail)) {
        return make_signature("invalid", std::move(detail));
    }

    PeLayout layout;
    const auto parse_result = parse_pe(bytes, layout, detail);
    if (parse_result == PeParseResult::not_pe) {
        return make_signature("unsupported", std::move(detail));
    }
    if (parse_result == PeParseResult::malformed) {
        return make_signature("invalid", std::move(detail));
    }
    if (layout.security_directory_size == 0) {
        return make_signature("unsigned", "PE image has no embedded certificate");
    }

    const auto entries = certificate_entries(bytes, layout, detail);
    if (!detail.empty() && entries.empty()) {
        return make_signature("invalid", std::move(detail));
    }
    if (entries.empty()) {
        return make_signature("unsupported", "PE certificate table has no PKCS#7 signature");
    }

    // The digest algorithm is selected from the signed Authenticode content. Try
    // SHA-1 first for XP compatibility, then SHA-256 for current releases.
    std::vector<std::uint8_t> sha1_digest;
    if (!hash_image(bytes, layout, false, sha1_digest, detail)) {
        return make_signature("invalid", std::move(detail));
    }
    std::vector<std::uint8_t> sha256_digest;
    if (!hash_image(bytes, layout, true, sha256_digest, detail)) {
        return make_signature("invalid", std::move(detail));
    }

    Signature first_failure = make_signature("invalid", "no valid embedded signature");
    for (const auto &entry : entries) {
        auto result = verify_pkcs7(entry, sha1_digest);
        if (result.status == "unsupported" || result.status == "invalid") {
            auto sha256_result = verify_pkcs7(entry, sha256_digest);
            if (sha256_result.status == "verified" || sha256_result.status == "untrusted") {
                result = std::move(sha256_result);
            }
        }
        if (result.status == "verified") {
            return result;
        }
        if (first_failure.status == "invalid" || result.status == "untrusted") {
            first_failure = std::move(result);
        }
    }
    return first_failure;
#endif
}

} // namespace ota
