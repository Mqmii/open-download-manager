#include "UpdateCheck.h"

#include "BridgeServer.h"   // ParseSimpleJson
#include "Downloader.h"     // ApplyTlsPolicy

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#endif

#ifndef ODM_VERSION
  #define ODM_VERSION "0.0.0"
#endif

namespace odm {
namespace updates {

namespace fs = std::filesystem;

const char* const kReleasesUrl =
    "https://github.com/Mqmii/open-download-manager/releases/latest";

namespace {

constexpr const char* kApiUrl =
    "https://api.github.com/repos/Mqmii/open-download-manager/releases/latest";
constexpr long long kCheckInterval = 24 * 60 * 60;   // seconds
constexpr size_t    kMaxBody = 512 * 1024;

// "v0.2.4" / "0.2.4" -> {0, 2, 4}. Empty on anything that isn't a plain
// dotted number, which is what keeps a moved branch tag or a "nightly" out.
std::vector<long> VersionParts(const std::string& v) {
    std::vector<long> parts;
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    if (i >= v.size()) return {};
    while (i < v.size()) {
        size_t start = i;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
        if (i == start) return {};                    // empty component
        parts.push_back(std::atol(v.substr(start, i - start).c_str()));
        if (i == v.size()) break;
        if (v[i] != '.') return {};                   // "-rc1", junk, ...
        ++i;
        if (i == v.size()) return {};                 // trailing dot
    }
    return parts;
}

size_t Collect(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    const size_t n = size * nmemb;
    if (out->size() + n > kMaxBody) return 0;         // absurd body: give up
    out->append(ptr, n);
    return n;
}

std::string ExeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return {};
    std::error_code ec;
    return fs::path(buf).parent_path().string();
#else
    return {};
#endif
}

// True when a check is due. Writes the stamp as it says yes, so a failing
// check costs one request a day rather than one per launch.
bool DueForCheck() {
    const std::string dir = ExeDir();
    if (dir.empty()) return true;         // no stamp possible: check anyway
    std::error_code ec;
    const fs::path stamp = fs::path(dir) / "update.stamp";
    const auto now = std::time(nullptr);
    if (fs::exists(stamp, ec)) {
        long long last = 0;
#if defined(_WIN32)
        FILE* f = nullptr;
        if (_wfopen_s(&f, stamp.c_str(), L"rb") == 0 && f) {
            char b[32] = {0};
            fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            last = atoll(b);
        }
#endif
        if (last > 0 && now - last < kCheckInterval) return false;
    }
#if defined(_WIN32)
    FILE* f = nullptr;
    if (_wfopen_s(&f, stamp.c_str(), L"wb") == 0 && f) {
        fprintf(f, "%lld", static_cast<long long>(now));
        fclose(f);
    }
#endif
    return true;
}

}  // namespace

bool IsNewerVersion(const std::string& tag, const std::string& current) {
    const std::vector<long> a = VersionParts(tag);
    const std::vector<long> b = VersionParts(current);
    if (a.empty() || b.empty()) return false;
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const long x = i < a.size() ? a[i] : 0;   // 0.2 and 0.2.0 are the same
        const long y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

bool ParseLatestTag(const std::string& json, std::string* out_tag) {
    std::map<std::string, std::string> m;
    if (!ParseSimpleJson(json, m)) return false;
    auto it = m.find("tag_name");
    if (it == m.end() || it->second.empty()) return false;
    *out_tag = it->second;
    return true;
}

void CheckAsync(std::function<void(const std::string&)> on_newer) {
    if (!on_newer) return;
    std::thread([on_newer = std::move(on_newer)] {
        if (!DueForCheck()) return;

        CURL* curl = curl_easy_init();
        if (!curl) return;
        ApplyTlsPolicy(curl);
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        // GitHub answers 403 to a request without a User-Agent.
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "ODM/" ODM_VERSION);
        curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Accept: application/vnd.github+json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

        const CURLcode rc = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(hdrs);
        if (rc != CURLE_OK) return;

        std::string tag;
        if (!ParseLatestTag(body, &tag)) return;
        if (!IsNewerVersion(tag, ODM_VERSION)) return;

        // Hand back the bare number: the tag's 'v' is a git convention, not
        // something to put in front of a user.
        on_newer(tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag);
    }).detach();
}

}  // namespace updates
}  // namespace odm
