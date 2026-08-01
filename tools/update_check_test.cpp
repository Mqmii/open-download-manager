// Update-check logic: version comparison and reading GitHub's answer.
//
// The network call is not exercised here — what can actually go wrong is
// deciding that a release is newer when it is not (nagging someone who is
// already current, or worse, pointing them at a downgrade), and choking on a
// response shape. Both are pure functions.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#include <cstdio>
#include <string>

#include "UpdateCheck.h"

namespace {

int g_failures = 0;

void Check(const std::string& what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

void Newer(const char* tag, const char* current, bool expected) {
    const bool got = odm::updates::IsNewerVersion(tag, current);
    Check(std::string(tag) + " over " + current + " -> " +
              (expected ? "newer" : "not newer"),
          got == expected);
}

}  // namespace

int main() {
    std::printf("version comparison\n");
    Newer("v0.2.4", "0.2.3", true);
    Newer("v0.3.0", "0.2.9", true);
    Newer("v1.0.0", "0.9.9", true);
    Newer("v0.2.4.1", "0.2.4", true);      // a re-tag with a fourth component
    Newer("0.2.4",  "0.2.3", true);        // tag without the 'v'

    std::printf("same version is not an update\n");
    Newer("v0.2.3", "0.2.3", false);
    Newer("v0.2.3", "v0.2.3", false);
    Newer("v0.2",   "0.2.0", false);       // 0.2 and 0.2.0 are one version
    Newer("v0.2.0", "0.2",   false);

    std::printf("never point at a downgrade\n");
    Newer("v0.2.2", "0.2.3", false);
    Newer("v0.2.9", "0.3.0", false);
    Newer("v0.9.9", "1.0.0", false);
    Newer("v0.2.4", "0.2.4.1", false);

    std::printf("numbers, not text\n");
    Newer("v0.2.10", "0.2.9", true);       // 10 > 9, though "10" < "9" sorted
    Newer("v0.2.9",  "0.2.10", false);

    std::printf("anything unparsable stays quiet\n");
    for (const char* tag : {"", "v", "nightly", "v0.2.4-rc1", "latest",
                            "v0.2.", "v..2", "v0.2.x", "release-2024"}) {
        Check(std::string("tag \"") + tag + "\" -> no notice",
              !odm::updates::IsNewerVersion(tag, "0.2.3"));
    }
    Check("unparsable current version -> no notice",
          !odm::updates::IsNewerVersion("v0.2.4", "dev"));

    std::printf("reading GitHub's answer\n");
    {
        // Trimmed to the shape that matters: a nested object and an array of
        // objects on either side of the field we want.
        const std::string body = R"({
            "url": "https://api.github.com/repos/x/y/releases/1",
            "author": {"login": "Mqmii", "id": 1},
            "tag_name": "v0.2.4",
            "draft": false,
            "prerelease": false,
            "assets": [{"name": "ODM-v0.2.4-win-x64.zip", "size": 49500000}],
            "body": "## Notes\nline two \"quoted\"\n"
        })";
        std::string tag;
        Check("tag_name is picked out of a full release object",
              odm::updates::ParseLatestTag(body, &tag) && tag == "v0.2.4");
    }
    {
        std::string tag;
        Check("a rate-limit reply yields no tag",
              !odm::updates::ParseLatestTag(
                  R"({"message":"API rate limit exceeded"})", &tag));
    }
    {
        std::string tag;
        Check("an empty tag_name is refused",
              !odm::updates::ParseLatestTag(R"({"tag_name":""})", &tag));
    }
    for (const char* body : {"", "not json", "[]", "{\"tag_name\":", "null"}) {
        std::string tag;
        Check(std::string("malformed body \"") + body + "\" -> no tag",
              !odm::updates::ParseLatestTag(body, &tag));
    }

    std::printf("the page we send people to\n");
    {
        const std::string url = odm::updates::kReleasesUrl;
        Check("is https", url.rfind("https://", 0) == 0);
        Check("is on github.com",
              url.rfind("https://github.com/", 0) == 0);
        Check("points at this repository's releases",
              url.find("/Mqmii/open-download-manager/releases") !=
                  std::string::npos);
    }

    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
