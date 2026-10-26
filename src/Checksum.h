#pragma once

#include <string>

namespace odm::checksum {

enum class Algorithm {
    None,
    MD5,
    SHA1,
    SHA256,
    SHA512
};

struct ParsedExpectation {
    bool        valid = false;
    Algorithm   algo  = Algorithm::None;
    std::string hex;     // normalized lowercase hex digest
    std::string error;   // non-empty when the input is malformed
};

struct Result {
    bool        ok      = false;
    Algorithm   algo    = Algorithm::None;
    std::string actual_hex;
    std::string expected_hex;
    bool        matched = false;
    std::string error;
};

/// Human-readable algorithm label ("MD5", "SHA-1", "SHA-256", "SHA-512").
const char* AlgorithmName(Algorithm algo);

/// Parse a user-supplied checksum string. Accepts raw hex (algorithm inferred
/// from length: 32=MD5, 40=SHA-1, 64=SHA-256, 128=SHA-512), algorithm prefixes
/// ("sha256:...", "SHA-256=...", "md5:..."), and sha256sum-style lines
/// ("<hex>  <filename>" or "<hex> *<filename>").
ParsedExpectation ParseExpected(const std::string& raw);

/// Stream `file_path` off disk and compute its lowercase hex digest.
bool ComputeFileHash(const std::string& file_path, Algorithm algo,
                     std::string* out_hex, std::string* out_error = nullptr);

/// Verify `file_path` against `expected_raw`. When `expected_raw` is empty or
/// whitespace-only, computes the file's SHA-256 digest and marks `matched`
/// true so callers (like the Properties dialog) can display the hash on demand.
Result VerifyFile(const std::string& file_path, const std::string& expected_raw);

} // namespace odm::checksum
