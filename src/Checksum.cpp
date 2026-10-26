#include "Checksum.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
#endif

namespace odm::checksum {

namespace fs = std::filesystem;

namespace {

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Algorithm AlgoFromHexLength(size_t n) {
    switch (n) {
        case 32:  return Algorithm::MD5;
        case 40:  return Algorithm::SHA1;
        case 64:  return Algorithm::SHA256;
        case 128: return Algorithm::SHA512;
        default:  return Algorithm::None;
    }
}

size_t ExpectedHexLength(Algorithm algo) {
    switch (algo) {
        case Algorithm::MD5:    return 32;
        case Algorithm::SHA1:   return 40;
        case Algorithm::SHA256: return 64;
        case Algorithm::SHA512: return 128;
        default:                return 0;
    }
}

Algorithm ParsePrefixAlgo(const std::string& p) {
    const std::string low = ToLower(Trim(p));
    if (low == "md5")                       return Algorithm::MD5;
    if (low == "sha1"   || low == "sha-1")   return Algorithm::SHA1;
    if (low == "sha256" || low == "sha-256") return Algorithm::SHA256;
    if (low == "sha512" || low == "sha-512") return Algorithm::SHA512;
    return Algorithm::None;
}

bool IsHex(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

} // namespace

const char* AlgorithmName(Algorithm algo) {
    switch (algo) {
        case Algorithm::MD5:    return "MD5";
        case Algorithm::SHA1:   return "SHA-1";
        case Algorithm::SHA256: return "SHA-256";
        case Algorithm::SHA512: return "SHA-512";
        default:                return "";
    }
}

ParsedExpectation ParseExpected(const std::string& raw) {
    ParsedExpectation out;
    std::string s = Trim(raw);
    if (s.empty()) {
        out.error = "No checksum provided.";
        return out;
    }

    Algorithm explicit_algo = Algorithm::None;

    // Strip optional "algo:" or "algo=" prefix (e.g. "sha256:e3b0...",
    // "SHA-256 = e3b0...").
    const size_t sep = s.find_first_of(":=");
    if (sep != std::string::npos) {
        const Algorithm cand = ParsePrefixAlgo(s.substr(0, sep));
        if (cand != Algorithm::None) {
            explicit_algo = cand;
            s = Trim(s.substr(sep + 1));
        } else {
            out.error = "Unsupported checksum algorithm prefix.";
            return out;
        }
    }

    // Handle sha256sum / md5sum line format ("<hex>  filename" or "<hex> *filename").
    const size_t space = s.find_first_of(" \t");
    if (space != std::string::npos) {
        s = s.substr(0, space);
    }

    // Strip optional leading "0x".
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }

    if (!IsHex(s)) {
        out.error = "Checksum must be a hexadecimal string (MD5, SHA-1, SHA-256 or SHA-512).";
        return out;
    }

    const Algorithm by_len = AlgoFromHexLength(s.size());
    if (explicit_algo != Algorithm::None) {
        if (s.size() != ExpectedHexLength(explicit_algo)) {
            out.error = std::string(AlgorithmName(explicit_algo)) +
                        " checksum must be " +
                        std::to_string(ExpectedHexLength(explicit_algo)) +
                        " hex characters.";
            return out;
        }
        out.algo = explicit_algo;
    } else {
        if (by_len == Algorithm::None) {
            out.error = "Checksum length (" + std::to_string(s.size()) +
                        " chars) does not match MD5 (32), SHA-1 (40), SHA-256 (64) or SHA-512 (128).";
            return out;
        }
        out.algo = by_len;
    }

    out.hex   = ToLower(s);
    out.valid = true;
    return out;
}

bool ComputeFileHash(const std::string& file_path, Algorithm algo,
                     std::string* out_hex, std::string* out_error) {
    if (out_hex) out_hex->clear();
    if (file_path.empty()) {
        if (out_error) *out_error = "File path is empty.";
        return false;
    }

    std::error_code ec;
    const fs::path p = fs::u8path(file_path);
    if (!fs::exists(p, ec) || fs::is_directory(p, ec)) {
        if (out_error) *out_error = "File does not exist on disk.";
        return false;
    }

#if defined(_WIN32)
    LPCWSTR bcrypt_id = nullptr;
    switch (algo) {
        case Algorithm::MD5:    bcrypt_id = BCRYPT_MD5_ALGORITHM;    break;
        case Algorithm::SHA1:   bcrypt_id = BCRYPT_SHA1_ALGORITHM;   break;
        case Algorithm::SHA256: bcrypt_id = BCRYPT_SHA256_ALGORITHM; break;
        case Algorithm::SHA512: bcrypt_id = BCRYPT_SHA512_ALGORITHM; break;
        default:
            if (out_error) *out_error = "Unsupported hash algorithm.";
            return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, bcrypt_id, nullptr, 0) != 0 || !hAlg) {
        if (out_error) *out_error = "Failed to initialize Windows crypto provider.";
        return false;
    }

    DWORD obj_len = 0, cb_data = 0, hash_len = 0;
    bool ok = false;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<UCHAR> hash_obj;
    std::vector<UCHAR> digest;

    do {
        if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len),
                              &cb_data, 0) != 0 || obj_len == 0) {
            if (out_error) *out_error = "Failed to query hash object size.";
            break;
        }
        if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len),
                              &cb_data, 0) != 0 || hash_len == 0) {
            if (out_error) *out_error = "Failed to query hash digest size.";
            break;
        }

        hash_obj.resize(obj_len);
        digest.resize(hash_len);

        if (BCryptCreateHash(hAlg, &hHash, hash_obj.data(), obj_len,
                             nullptr, 0, 0) != 0 || !hHash) {
            if (out_error) *out_error = "Failed to create hash state.";
            break;
        }

        std::ifstream f(p, std::ios::binary);
        if (!f) {
            if (out_error) *out_error = "Could not open file for reading.";
            break;
        }

        // Stream in 1 MB chunks so multi-GB ISOs and archives never spike RAM.
        std::vector<char> buf(1024 * 1024);
        bool read_err = false;
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = f.gcount();
            if (got > 0) {
                if (BCryptHashData(hHash, reinterpret_cast<PUCHAR>(buf.data()),
                                   static_cast<ULONG>(got), 0) != 0) {
                    read_err = true;
                    break;
                }
            }
        }
        if (read_err || (!f.eof() && f.fail())) {
            if (out_error) *out_error = "Error while reading file from disk.";
            break;
        }

        if (BCryptFinishHash(hHash, digest.data(), hash_len, 0) != 0) {
            if (out_error) *out_error = "Failed to finalize hash digest.";
            break;
        }

        static const char kHexDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(digest.size() * 2);
        for (UCHAR b : digest) {
            hex.push_back(kHexDigits[(b >> 4) & 0x0F]);
            hex.push_back(kHexDigits[b & 0x0F]);
        }
        if (out_hex) *out_hex = std::move(hex);
        ok = true;
    } while (false);

    if (hHash) BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
#else
    (void)algo;
    if (out_error) *out_error = "Checksum verification is only implemented on Windows.";
    return false;
#endif
}

Result VerifyFile(const std::string& file_path, const std::string& expected_raw) {
    Result res;
    const std::string trimmed = Trim(expected_raw);

    if (trimmed.empty()) {
        // On-demand calculation with no expected value: default to SHA-256.
        res.algo = Algorithm::SHA256;
        if (!ComputeFileHash(file_path, res.algo, &res.actual_hex, &res.error)) {
            res.ok = false;
            return res;
        }
        res.ok      = true;
        res.matched = true;
        return res;
    }

    const ParsedExpectation exp = ParseExpected(trimmed);
    if (!exp.valid) {
        res.ok    = false;
        res.error = exp.error;
        return res;
    }

    res.algo         = exp.algo;
    res.expected_hex = exp.hex;
    if (!ComputeFileHash(file_path, res.algo, &res.actual_hex, &res.error)) {
        res.ok = false;
        return res;
    }

    res.ok      = true;
    res.matched = (res.actual_hex == res.expected_hex);
    return res;
}

} // namespace odm::checksum
