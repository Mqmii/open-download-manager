// Checksum verification and digest computation.
//
// Asserts that MD5, SHA-1, SHA-256, and SHA-512 hashes are parsed in raw hex,
// prefixed ("sha256:..."), and sha256sum ("<hex>  <filename>") formats, and
// that VerifyFile distinguishes matching files, corrupted files, invalid
// expectations, and missing files.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "Checksum.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(const std::string& what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

void Write(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / "odm_checksum_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path operator/(const char* name) const { return dir / name; }
};

// Known NIST / standard test vectors for "abc" (3 bytes):
constexpr const char* kAbcMd5 =
    "900150983cd24fb0d6963f7d28e17f72";
constexpr const char* kAbcSha1 =
    "a9993e364706816aba3e25717850c26c9cd0d89d";
constexpr const char* kAbcSha256 =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr const char* kAbcSha512 =
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39"
    "a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";

}  // namespace

int main() {
    Scratch scratch;
    const fs::path abc = scratch / "abc.bin";
    Write(abc, "abc");
    const std::string abc_path = abc.u8string();

    std::printf("algorithm detection and parsing\n");
    {
        auto p_md5 = odm::checksum::ParseExpected(kAbcMd5);
        Check("32-hex auto-detects MD5",
              p_md5.valid && p_md5.algo == odm::checksum::Algorithm::MD5 &&
              p_md5.hex == kAbcMd5);

        auto p_sha1 = odm::checksum::ParseExpected(
            "SHA-1: A9993E364706816ABA3E25717850C26C9CD0D89D");
        Check("prefixed uppercase SHA-1 normalizes to lowercase",
              p_sha1.valid && p_sha1.algo == odm::checksum::Algorithm::SHA1 &&
              p_sha1.hex == kAbcSha1);

        auto p_sum = odm::checksum::ParseExpected(
            std::string(kAbcSha256) + "  *release.zip");
        Check("sha256sum line strips trailing filename",
              p_sum.valid && p_sum.algo == odm::checksum::Algorithm::SHA256 &&
              p_sum.hex == kAbcSha256);

        auto p_bad = odm::checksum::ParseExpected("sha256:12345");
        Check("wrong hex length is rejected",
              !p_bad.valid && !p_bad.error.empty());
    }

    std::printf("digest computation and verification across algorithms\n");
    {
        auto r_md5 = odm::checksum::VerifyFile(abc_path, kAbcMd5);
        Check("MD5 matches 'abc'",
              r_md5.ok && r_md5.matched &&
              r_md5.algo == odm::checksum::Algorithm::MD5);

        auto r_sha1 = odm::checksum::VerifyFile(abc_path, kAbcSha1);
        Check("SHA-1 matches 'abc'",
              r_sha1.ok && r_sha1.matched &&
              r_sha1.algo == odm::checksum::Algorithm::SHA1);

        auto r_sha256 = odm::checksum::VerifyFile(abc_path, kAbcSha256);
        Check("SHA-256 matches 'abc'",
              r_sha256.ok && r_sha256.matched &&
              r_sha256.algo == odm::checksum::Algorithm::SHA256);

        auto r_sha512 = odm::checksum::VerifyFile(abc_path, kAbcSha512);
        Check("SHA-512 matches 'abc'",
              r_sha512.ok && r_sha512.matched &&
              r_sha512.algo == odm::checksum::Algorithm::SHA512);
    }

    std::printf("mismatch and error reporting\n");
    {
        std::string wrong = kAbcSha256;
        wrong.back() = '0';
        auto r_mis = odm::checksum::VerifyFile(abc_path, wrong);
        Check("corrupted digest reports mismatch",
              r_mis.ok && !r_mis.matched && r_mis.actual_hex == kAbcSha256);

        auto r_empty = odm::checksum::VerifyFile(abc_path, "");
        Check("empty expected hash computes SHA-256 on demand",
              r_empty.ok && r_empty.expected_hex.empty() &&
              r_empty.algo == odm::checksum::Algorithm::SHA256 &&
              r_empty.actual_hex == kAbcSha256);

        auto r_missing = odm::checksum::VerifyFile(
            (scratch / "nope.bin").u8string(), kAbcSha256);
        Check("missing file reports I/O error", !r_missing.ok);
    }

    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
