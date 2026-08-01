#include "HlsDownloader.h"
#include "Muxer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
#endif

// winsock2.h (pulled by curl headers) -> windows.h defines min/max macros
// that break std::max/std::min; kill them for this translation unit.
#undef max
#undef min


namespace odm {
namespace fs = std::filesystem;

namespace {

std::once_flag g_curl_once;

// Browser UA: many video CDNs answer 403 to curl's default UA. The browser
// hand-off (RequestContext::user_agent) overrides this when present.
const char* kDefaultUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

struct FetchBuf {
    std::vector<uint8_t>*   data;
    std::atomic<uint64_t>*  inflight;   // nullptr for non-worker fetches
};

size_t FetchWrite(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* fb = static_cast<FetchBuf*>(ud);
    const size_t n = size * nmemb;
    fb->data->insert(fb->data->end(), ptr, ptr + n);
    if (fb->inflight) fb->inflight->fetch_add(n);
    return n;
}

uint64_t HashUrl(const std::string& s) {   // FNV-1a 64
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    size_t b = s.find_last_not_of(ws);
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// RFC 3986-lite: resolve `rel` against `base` (playlist URLs are usually
// relative). Handles absolute, protocol-relative, root-relative, relative
// paths and ../ dot segments.
std::string ResolveUrl(const std::string& base_in, const std::string& rel_in) {
    const std::string rel = Trim(rel_in);
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0)
        return rel;
    // Strip query/fragment from base.
    std::string base = base_in;
    size_t q = base.find_first_of("?#");
    if (q != std::string::npos) base = base.substr(0, q);

    const size_t scheme_end = base.find("://");
    const std::string scheme =
        scheme_end == std::string::npos ? "https" : base.substr(0, scheme_end);
    if (rel.rfind("//", 0) == 0) return scheme + ":" + rel;

    const size_t auth_begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    size_t path_begin = base.find('/', auth_begin);
    if (path_begin == std::string::npos) path_begin = base.size();
    const std::string authority = base.substr(0, path_begin);
    if (!rel.empty() && rel[0] == '/') return authority + rel;

    // Merge: base directory + relative path, then dot-segment removal.
    const std::string base_path = base.substr(path_begin);
    const std::string base_dir =
        base_path.substr(0, base_path.find_last_of('/') + 1);
    const std::string merged = base_dir + rel;
    std::vector<std::string> out;
    size_t i = 0;
    while (i < merged.size()) {
        size_t j = merged.find('/', i);
        const std::string seg =
            merged.substr(i, (j == std::string::npos ? merged.size() : j) - i);
        if (seg == "..") { if (!out.empty()) out.pop_back(); }
        else if (!seg.empty() && seg != ".") out.push_back(seg);
        i = j == std::string::npos ? merged.size() : j + 1;
    }
    std::string path;
    for (const auto& s : out) { path += '/'; path += s; }
    if (path.empty()) path = "/";
    return authority + path;
}

// Parse "METHOD=AES-128,URI=\"...\",IV=0x..." style attribute lists.
std::vector<std::pair<std::string, std::string>> ParseAttrs(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string name = s.substr(i, eq - i);
        std::string val;
        size_t next;
        if (eq + 1 < s.size() && s[eq + 1] == '"') {
            size_t endq = s.find('"', eq + 2);
            val = s.substr(eq + 2, endq == std::string::npos
                                        ? std::string::npos : endq - (eq + 2));
            next = endq == std::string::npos ? s.size() : endq + 1;
        } else {
            size_t comma = s.find(',', eq + 1);
            val = s.substr(eq + 1, comma == std::string::npos
                                       ? std::string::npos : comma - (eq + 1));
            next = comma == std::string::npos ? s.size() : comma;
        }
        out.emplace_back(Trim(name), val);
        i = next + 1;
    }
    return out;
}

std::string AttrOf(const std::vector<std::pair<std::string, std::string>>& a,
                   const std::string& name) {
    for (const auto& kv : a) if (kv.first == name) return kv.second;
    return {};
}

bool HexToBytes(const std::string& hex_in, std::vector<uint8_t>& out) {
    std::string h = hex_in;
    if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
    if (h.empty() || h.size() > 32) return false;
    if (h.size() % 2) h = "0" + h;                    // left-pad to full bytes
    out.clear();
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HlsDownloader::HlsDownloader() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HlsDownloader::~HlsDownloader() {
    Stop();
    if (orchestrator_.joinable()) orchestrator_.join();
    if (progress_thread_.joinable()) progress_thread_.join();
    if (req_headers_) { curl_slist_free_all(req_headers_); req_headers_ = nullptr; }
}

void HlsDownloader::SetPartCount(int parts) { part_count_ = std::clamp(parts, 1, 16); }
void HlsDownloader::SetSpeedLimit(uint64_t bps) { speed_limit_ = bps; }
void HlsDownloader::SetProgressCallback(ProgressCallback cb) { progress_cb_ = std::move(cb); }
void HlsDownloader::SetCompletionCallback(CompletionCallback cb) { completion_cb_ = std::move(cb); }
void HlsDownloader::SetPathResolvedCallback(PathResolvedCallback cb) {
    path_resolved_cb_ = std::move(cb);
}

bool HlsDownloader::Start(const std::string& url, const std::string& output_path,
                          const std::string& id, const RequestContext& ctx) {
    std::lock_guard<std::mutex> lk(start_mtx_);
    if (running_) return false;
    if (orchestrator_.joinable()) orchestrator_.join();     // reap previous run
    if (progress_thread_.joinable()) progress_thread_.join();

    // Reset run state.
    url_ = url; playlist_url_.clear(); audio_url_.clear();
    output_path_ = output_path; id_ = id;
    req_ctx_ = ctx;
    if (req_headers_) { curl_slist_free_all(req_headers_); req_headers_ = nullptr; }
    for (const auto& h : req_ctx_.extra_headers)
        req_headers_ = curl_slist_append(req_headers_, h.c_str());

    segments_.clear(); init_url_.clear(); is_fmp4_ = false;
    init_off_ = init_len_ = 0; init_ranged_ = false;
    key_ = KeyInfo{}; key_iv_.clear(); key_bytes_.clear(); init_done_ = false;
    completed_.clear(); resuming_ = false;
    { std::lock_guard<std::mutex> fl(fail_mtx_); fail_reason_.clear(); }
    stop_requested_ = false; abort_ = false; progress_stop_ = false;
    next_seg_ = 0; segs_done_ = 0; bytes_done_ = 0; active_workers_ = 0;
    track_base_bytes_ = 0;
    result_ = DownloadResult{};

    running_ = true;
    orchestrator_ = std::thread(&HlsDownloader::Orchestrator, this);
    return true;
}

void HlsDownloader::Stop() { stop_requested_ = true; }

DownloadResult HlsDownloader::WaitForCompletion() {
    if (orchestrator_.joinable()) orchestrator_.join();
    return result_;
}

void HlsDownloader::ApplyRequestContext(CURL* curl, const std::string& url) const {
    odm::ApplyRequestContext(curl, req_ctx_, url, req_headers_);
}

// ---------------------------------------------------------------------------
// curl layer
// ---------------------------------------------------------------------------

// Shared low-level GET into memory. `worker_index` selects the in-flight
// counter (-1 = none). `range` is a curl "start-end" byte range (nullptr =
// whole resource). Aborts quickly when Stop/Fail is requested.
bool HlsDownloader::FetchToMemory(const std::string& url,
                                  std::vector<uint8_t>& out,
                                  long* out_http_code, int worker_index,
                                  const char* range) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    FetchBuf fb{ &out, worker_index >= 0 ? inflight_[worker_index].get() : nullptr };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (range) curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FetchWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   // hop cap in ApplyRequestContext
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10240L);  // stall watchdog:
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);      // <10KB/s for 20s
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kDefaultUA);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");      // gzip ok
    if (speed_limit_ > 0) {
        const auto per = static_cast<curl_off_t>(
            std::max<uint64_t>(speed_limit_ / std::max(part_count_, 1), 4096));
        curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, per);
    }
    ApplyRequestContext(curl, url);
    // Quick cancel: abort the transfer within ~100ms of Stop/Fail.
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            auto* self = static_cast<HlsDownloader*>(clientp);
            return (self->stop_requested_ || self->abort_) ? 1 : 0;
        });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);

    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (out_http_code) *out_http_code = code;
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

// Segment fetch with retry: flaky CDN tokens and 5xx are common; 404/410 is
// final (the playlist is stale). Honors Stop between attempts. `ranged`
// requests only the EXT-X-BYTERANGE window (CMAF: many segments live inside
// ONE .mp4 file) and rejects servers that ignore Range and send everything.
bool HlsDownloader::FetchSegment(const std::string& url,
                                 std::vector<uint8_t>& out, int worker_index,
                                 bool ranged, uint64_t off, uint64_t len) {
    char range[64] = {0};
    if (ranged)
        std::snprintf(range, sizeof(range), "%llu-%llu",
                      static_cast<unsigned long long>(off),
                      static_cast<unsigned long long>(off + len - 1));
    static const int kBackoffMs[3] = {400, 1200, 2500};
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (stop_requested_ || abort_) return false;
        out.clear();
        if (inflight_[worker_index]) inflight_[worker_index]->store(0);
        long code = 0;
        const bool net_ok = FetchToMemory(url, out, &code, worker_index,
                                          ranged ? range : nullptr);
        if (inflight_[worker_index]) inflight_[worker_index]->store(0);
        if (net_ok && code >= 200 && code < 300) {
            if (!ranged || out.size() == len) return true;
            // Range ignored (200 + full body) or short read — retry once,
            // then give up: concatenating the wrong window corrupts output.
        }
        if (code == 404 || code == 410) return false;
        for (int ms = kBackoffMs[attempt]; ms > 0 && !stop_requested_ && !abort_;
             ms -= 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool HlsDownloader::LoadPlaylist(const std::string& url, std::string& out_body,
                                 std::string& out_final_url) {
    std::vector<uint8_t> buf;
    long code = 0;
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    FetchBuf fb{ &buf, nullptr };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FetchWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   // hop cap in ApplyRequestContext
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kDefaultUA);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    ApplyRequestContext(curl, url);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    out_final_url = eff ? eff : url;
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code < 200 || code >= 300 || buf.empty()) return false;
    out_body.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
    return true;
}

// ---------------------------------------------------------------------------
// Playlist parsing
// ---------------------------------------------------------------------------

// Media playlist -> segments_ / key_ / init_url_. Fails (via Fail()) on live
// streams, DRM, key rotation, or empty playlists.
bool HlsDownloader::ParseMediaPlaylist(const std::string& body,
                                       const std::string& base_url) {
    std::istringstream in(body);
    std::string line;
    bool endlist = false;
    uint64_t seq = 0;
    KeyInfo cur_key;
    std::string first_key_uri;
    // EXT-X-BYTERANGE state: "n[@o]" — without @o the window starts right
    // after the previous segment's window of the same resource (RFC 8216).
    bool     have_range = false;
    uint64_t range_len = 0, range_off = 0, next_range_off = 0;

    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty()) continue;

        if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            seq = std::strtoull(line.c_str() + 22, nullptr, 10);
        } else if (line.rfind("#EXT-X-BYTERANGE:", 0) == 0) {
            const std::string v = line.substr(17);
            range_len = std::strtoull(v.c_str(), nullptr, 10);
            const size_t at = v.find('@');
            range_off = at != std::string::npos
                            ? std::strtoull(v.c_str() + at + 1, nullptr, 10)
                            : next_range_off;
            have_range = range_len > 0;
        } else if (line.rfind("#EXT-X-KEY:", 0) == 0) {
            const auto attrs = ParseAttrs(line.substr(11));
            cur_key.method = AttrOf(attrs, "METHOD");
            cur_key.uri = AttrOf(attrs, "URI");
            cur_key.iv_hex = AttrOf(attrs, "IV");
            if (cur_key.method == "SAMPLE-AES" ||
                AttrOf(attrs, "KEYFORMAT").find("com.apple") != std::string::npos) {
                Fail("DRM/SAMPLE-AES encryption is not supported.");
                return false;
            }
            // A second, DIFFERENT key mid-playlist = key rotation (rare on
            // VOD). Not supported this phase — fail loudly, not a corrupt file.
            if (!cur_key.uri.empty()) {
                if (first_key_uri.empty()) first_key_uri = cur_key.uri;
                else if (first_key_uri != cur_key.uri) {
                    Fail("Key rotation (multiple EXT-X-KEY) is not supported.");
                    return false;
                }
            }
        } else if (line.rfind("#EXT-X-MAP:", 0) == 0) {
            const auto attrs = ParseAttrs(line.substr(11));
            const std::string uri = AttrOf(attrs, "URI");
            if (!uri.empty()) { init_url_ = ResolveUrl(base_url, uri); is_fmp4_ = true; }
            // BYTERANGE="len@off": the init box shares the segment file.
            const std::string br = AttrOf(attrs, "BYTERANGE");
            if (!br.empty()) {
                init_len_ = std::strtoull(br.c_str(), nullptr, 10);
                const size_t at = br.find('@');
                init_off_ = at != std::string::npos
                                ? std::strtoull(br.c_str() + at + 1, nullptr, 10)
                                : 0;
                init_ranged_ = init_len_ > 0;
            }
        } else if (line == "#EXT-X-ENDLIST") {
            endlist = true;
        } else if (line[0] == '#') {
            continue;   // EXTINF, DISCONTINUITY, ... — not needed for VOD
        } else {
            Segment s;
            s.url = ResolveUrl(base_url, line);
            s.seq = seq++;
            if (have_range) {
                s.ranged = true;
                s.off = range_off;
                s.len = range_len;
                next_range_off = range_off + range_len;
                have_range = false;
            }
            segments_.push_back(std::move(s));
            if (key_.method.empty() && !cur_key.method.empty()) key_ = cur_key;
        }
    }

    if (segments_.empty()) { Fail("Playlist is empty or unrecognized."); return false; }
    if (!endlist) {
        Fail("Live stream (no EXT-X-ENDLIST) — not supported in this phase.");
        return false;
    }
    if (key_.method == "NONE") key_ = KeyInfo{};
    // fMP4 without EXT-X-MAP: sniff the first segment's extension.
    if (!is_fmp4_) {
        std::string u = segments_.front().url;
        size_t q = u.find_first_of("?#");
        if (q != std::string::npos) u = u.substr(0, q);
        const std::string ext = fs::path(u).extension().string();
        is_fmp4_ = ext == ".m4s" || ext == ".mp4" || ext == ".cmfv";
    }
    return true;
}

// ---------------------------------------------------------------------------
// AES-128 (Windows BCrypt)
// ---------------------------------------------------------------------------

bool HlsDownloader::FetchKey() {
    if (key_.method != "AES-128" || key_.uri.empty()) return true;
    const std::string key_url = ResolveUrl(playlist_url_, key_.uri);
    std::vector<uint8_t> buf;
    long code = 0;
    if (!FetchToMemory(key_url, buf, &code, -1) || buf.size() != 16) {
        Fail("Failed to fetch AES key: " + key_url);
        return false;
    }
    key_bytes_ = std::move(buf);
    if (!key_.iv_hex.empty()) {
        if (!HexToBytes(key_.iv_hex, key_iv_) || key_iv_.size() > 16) {
            Fail("Invalid EXT-X-KEY IV.");
            return false;
        }
        // Left-pad IV to 16 bytes (it is a 128-bit big-endian integer).
        if (key_iv_.size() < 16)
            key_iv_.insert(key_iv_.begin(), 16 - key_iv_.size(), 0);
    }
    return true;
}

bool HlsDownloader::DecryptSegment(std::vector<uint8_t>& data, uint64_t seq) {
#if defined(_WIN32)
    if (data.empty()) return true;
    if (data.size() % 16 != 0) return false;   // CBC requires full blocks

    // IV: explicit EXT-X-KEY IV, else the segment's media sequence number as
    // a 128-bit big-endian integer (RFC 8216 §5.2).
    uint8_t iv[16] = {0};
    if (key_iv_.size() == 16) {
        std::memcpy(iv, key_iv_.data(), 16);
    } else {
        for (int i = 0; i < 8; ++i)
            iv[8 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    bool ok = false;
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(BCRYPT_CHAIN_MODE_CBC),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) == 0) {
        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                       key_bytes_.data(), 16, 0) == 0) {
            ULONG out_len = 0;
            // HLS uses PKCS7 padding; BCrypt validates + strips it.
            if (BCryptDecrypt(hKey, data.data(), static_cast<ULONG>(data.size()),
                              nullptr, iv, 16, data.data(),
                              static_cast<ULONG>(data.size()), &out_len,
                              BCRYPT_BLOCK_PADDING) == 0) {
                data.resize(out_len);
                ok = true;
            }
            BCryptDestroyKey(hKey);
        }
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
#else
    (void)data; (void)seq;
    return false;   // HLS AES is wired for BCrypt (Windows-only build)
#endif
}

// ---------------------------------------------------------------------------
// Part files + resume sidecar
// ---------------------------------------------------------------------------

std::string HlsDownloader::SegPath(uint64_t idx) const {
    char name[32];
    std::snprintf(name, sizeof(name), "seg_%06llu.seg",
                  static_cast<unsigned long long>(idx));
    return (parts_dir_ / name).string();
}

// Sidecar layout: magic 'IDH1' | version u32 | url_hash u64 | seg_count u64 |
// is_fmp4 u8 | init_done u8 | completed bitmap (seg_count bytes).
void HlsDownloader::SaveMeta() {
    std::lock_guard<std::mutex> lk(meta_mtx_);
    if (segments_.empty() || meta_path_.empty()) return;
    std::vector<uint8_t> buf;
    auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    auto put32 = [&](uint32_t v) { put(&v, 4); };
    auto put64 = [&](uint64_t v) { put(&v, 8); };
    const uint32_t magic = 0x3148444Fu;   // 'ODH1'
    put32(magic); put32(1);
    put64(HashUrl(playlist_url_));
    put64(static_cast<uint64_t>(segments_.size()));
    const uint8_t fmp4 = is_fmp4_ ? 1 : 0, initb = init_done_ ? 1 : 0;
    put(&fmp4, 1); put(&initb, 1);
    buf.insert(buf.end(), completed_.begin(), completed_.end());

    const fs::path tmp = meta_path_.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    std::error_code ec;
    fs::rename(tmp, meta_path_, ec);
}

// Returns true when at least one segment was restored (i.e. this is a resume).
bool HlsDownloader::LoadMeta() {
    std::error_code ec;
    if (!fs::exists(meta_path_, ec)) return false;
    std::ifstream f(meta_path_, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    const size_t need = 4 + 4 + 8 + 8 + 2 + segments_.size();
    if (buf.size() < need) return false;
    const uint8_t* p = buf.data();
    auto get32 = [&] { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; };
    auto get64 = [&] { uint64_t v; std::memcpy(&v, p, 8); p += 8; return v; };
    if (get32() != 0x3148444Fu || get32() != 1) return false;
    if (get64() != HashUrl(playlist_url_)) return false;
    if (get64() != segments_.size()) return false;
    const uint8_t fmp4 = *p++, initb = *p++;
    if ((fmp4 != 0) != is_fmp4_) return false;

    uint64_t done = 0, bytes = 0;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (!p[i]) continue;
        // Trust the bitmap only if the part file is really there and non-empty.
        const auto sz = fs::file_size(SegPath(i), ec);
        if (ec || sz == 0) continue;
        completed_[i] = 1;
        ++done; bytes += sz;
    }
    if (initb && is_fmp4_ && !init_url_.empty()) {
        const fs::path init_path = parts_dir_ / "init.seg";
        if (fs::exists(init_path, ec) && fs::file_size(init_path, ec) > 0)
            init_done_ = true;
    }
    segs_done_ = done;
    bytes_done_ += bytes;   // cumulative: audio-track resume adds to video's
    return done > 0;
}

namespace {

// Highest-BANDWIDTH variant of a master playlist (falls back to the first
// listed when no BANDWIDTH attributes exist). Reports the winning variant's
// AUDIO group id too, so the matching rendition can be downloaded alongside.
std::string BestVariantUrl(const std::string& body, const std::string& base_url,
                           std::string* out_audio_group) {
    std::istringstream in(body);
    std::string line;
    long best_bw = -1, cur_bw = 0;
    std::string best_uri, cur_audio, best_audio;
    bool next_is_uri = false;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            const auto attrs = ParseAttrs(line.substr(18));
            cur_bw = 0;
            const std::string bw = AttrOf(attrs, "BANDWIDTH");
            if (!bw.empty()) cur_bw = std::atol(bw.c_str());
            cur_audio = AttrOf(attrs, "AUDIO");
            next_is_uri = true;
        } else if (next_is_uri && !line.empty() && line[0] != '#') {
            if (cur_bw > best_bw) {
                best_bw = cur_bw; best_uri = line; best_audio = cur_audio;
            }
            next_is_uri = false;
        }
    }
    if (out_audio_group) *out_audio_group = best_uri.empty() ? "" : best_audio;
    return best_uri.empty() ? std::string() : ResolveUrl(base_url, best_uri);
}

// EXT-X-MEDIA audio rendition URI for `group`: DEFAULT=YES wins, else the
// first listed. Empty when the group carries no URI (audio already muxed
// into the variant segments — nothing extra to download).
std::string AudioRenditionUrl(const std::string& body,
                              const std::string& base_url,
                              const std::string& group) {
    std::istringstream in(body);
    std::string line, first, preferred;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.rfind("#EXT-X-MEDIA:", 0) != 0) continue;
        const auto attrs = ParseAttrs(line.substr(13));
        if (AttrOf(attrs, "TYPE") != "AUDIO") continue;
        if (AttrOf(attrs, "GROUP-ID") != group) continue;
        const std::string uri = AttrOf(attrs, "URI");
        if (uri.empty()) continue;
        if (first.empty()) first = uri;
        if (preferred.empty() && AttrOf(attrs, "DEFAULT") == "YES")
            preferred = uri;
    }
    const std::string& pick = preferred.empty() ? first : preferred;
    return pick.empty() ? std::string() : ResolveUrl(base_url, pick);
}

// A finished track leaves its file behind and removes the workspace — that
// combination marks "already done" when a mux run is resumed.
bool TrackDone(const fs::path& f) {
    std::error_code ec;
    return fs::exists(f, ec) && fs::file_size(f, ec) > 0 &&
           !fs::exists(f.string() + ".hlsmeta", ec) &&
           !fs::exists(f.string() + ".hlsparts", ec);
}

} // namespace

// ---------------------------------------------------------------------------
// Orchestrator / workers
// ---------------------------------------------------------------------------

void HlsDownloader::Orchestrator() {
    result_ = DownloadResult{};
    result_.id = id_;

    // 1. Fetch playlist. A master yields the best video variant plus (CMAF)
    //    the separate audio rendition of that variant's AUDIO group.
    std::string body, final_url;
    if (!LoadPlaylist(url_, body, final_url)) {
        Fail("Failed to download playlist (network error or 403).");
        return Finish();
    }
    if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {
        std::string audio_group;
        const std::string variant = BestVariantUrl(body, final_url, &audio_group);
        if (variant.empty()) {
            Fail("No variant found in the master playlist.");
            return Finish();
        }
        playlist_url_ = variant;
        if (!audio_group.empty())
            audio_url_ = AudioRenditionUrl(body, final_url, audio_group);
        if (!LoadPlaylist(variant, body, final_url)) {
            Fail("Failed to download variant playlist.");
            return Finish();
        }
    } else {
        playlist_url_ = final_url;
    }
    if (!ParseMediaPlaylist(body, final_url)) return Finish();

    const bool have_audio = !audio_url_.empty();

    // 2. Correct the output extension: .mp4 for fMP4 or whenever a separate
    //    audio track will be muxed in; raw TS concat stays .ts.
    {
        fs::path p(output_path_);
        const std::string want = (is_fmp4_ || have_audio) ? ".mp4" : ".ts";
        if (_stricmp(p.extension().string().c_str(), want.c_str()) != 0) {
            p.replace_extension(want);
            output_path_ = p.string();
            if (path_resolved_cb_) path_resolved_cb_(id_, output_path_);
        }
        // The extension change can land on an existing finished file the
        // start-time guard could not see ("video" -> "video.mp4"). Same
        // rule: uniquify fresh jobs, resume workspaces pin the path.
        const std::string uniq = UniquifyPath(output_path_);
        if (uniq != output_path_) {
            output_path_ = uniq;
            if (path_resolved_cb_) path_resolved_cb_(id_, output_path_);
        }
    }

    // Splits either land directly in the output (single muxed track) or in
    // temp track files that the muxer combines at the end.
    const fs::path vtrack = have_audio ? fs::path(output_path_ + ".vtrk")
                                       : fs::path(output_path_);
    const fs::path atrack(output_path_ + ".atrk");

    // Reports Stop/Fail through result_ after an aborted track run.
    auto track_aborted = [this]() {
        std::lock_guard<std::mutex> lk(fail_mtx_);
        if (stop_requested_ && fail_reason_.empty()) {
            result_.status = DownloadStatus::Stopped;
            result_.file_path = output_path_;
        }
    };

    // 3. Video track (skipped when a resumed mux run already finished it).
    if (!(have_audio && TrackDone(vtrack))) {
        if (!RunParsedTrack(vtrack)) { track_aborted(); return Finish(); }
    }

    // 4. Audio track + lossless remux into the final .mp4.
    if (have_audio) {
        std::string abody, afinal;
        if (!LoadPlaylist(audio_url_, abody, afinal)) {
            Fail("Failed to download audio playlist.");
            return Finish();
        }
        // Per-track parse state reset (the video track owned it until now).
        segments_.clear(); init_url_.clear(); is_fmp4_ = false;
        init_off_ = init_len_ = 0; init_ranged_ = false;
        key_ = KeyInfo{}; key_iv_.clear(); key_bytes_.clear();
        init_done_ = false;
        playlist_url_ = afinal;
        if (!ParseMediaPlaylist(abody, afinal)) return Finish();
        if (!TrackDone(atrack)) {
            if (!RunParsedTrack(atrack)) { track_aborted(); return Finish(); }
        }

        std::string mux_err;
        if (!MuxAvTracks(vtrack.string(), atrack.string(), output_path_,
                         &mux_err)) {
            std::error_code ec;
            fs::remove(output_path_, ec);   // never leave a broken output
            Fail("Failed to mux audio and video: " + mux_err);
            return Finish();
        }
        std::error_code ec;
        fs::remove(vtrack, ec);
        fs::remove(atrack, ec);
    }

    result_.status = DownloadStatus::Completed;
    result_.file_path = output_path_;
    result_.total_bytes = static_cast<double>(bytes_done_.load());
    result_.downloaded_bytes = static_cast<double>(bytes_done_.load());
    Finish();
}

// Workspace + resume + key + init + workers + concat for the playlist that
// is currently parsed into segments_/key_/init_url_.
bool HlsDownloader::RunParsedTrack(const fs::path& track_file) {
    parts_dir_ = track_file.string() + ".hlsparts";
    meta_path_ = track_file.string() + ".hlsmeta";
    std::error_code ec;
    fs::create_directories(parts_dir_, ec);
    if (ec) { Fail("Failed to create the working directory."); return false; }
    next_seg_ = 0; segs_done_ = 0;
    track_base_bytes_ = bytes_done_.load();
    completed_.assign(segments_.size(), 0);
    if (LoadMeta()) resuming_ = true;

    // Encryption key (if any).
    if (!key_.method.empty() && key_.method != "NONE") {
        if (key_.method != "AES-128") {
            Fail("Unsupported encryption: " + key_.method);
            return false;
        }
        if (!FetchKey()) return false;
    }

    // In-flight counters must exist before any worker-indexed fetch.
    inflight_.clear();
    for (int i = 0; i < part_count_; ++i)
        inflight_.push_back(std::make_unique<std::atomic<uint64_t>>(0));

    // fMP4 init segment (small; fetch inline before workers start).
    if (is_fmp4_ && !init_url_.empty() && !init_done_) {
        std::vector<uint8_t> data;
        if (!FetchSegment(init_url_, data, 0, init_ranged_, init_off_, init_len_)) {
            Fail("Failed to download the fMP4 init segment.");
            return false;
        }
        if (!key_bytes_.empty() && !DecryptSegment(data, 0)) {
            Fail("Failed to decrypt the fMP4 init segment.");
            return false;
        }
        std::ofstream f(parts_dir_ / "init.seg", std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(data.data()),
                           static_cast<std::streamsize>(data.size()))) {
            Fail("Failed to write the fMP4 init segment to disk.");
            return false;
        }
        f.close();
        init_done_ = true;
        SaveMeta();
    }

    // Progress thread + segment workers (per track: the progress thread must
    // not observe segments_ while the next track re-parses it).
    if (progress_thread_.joinable()) progress_thread_.join();
    progress_stop_ = false;
    progress_thread_ = std::thread(&HlsDownloader::ProgressLoop, this);
    const int nw = std::max(1, std::min(part_count_,
                                        static_cast<int>(segments_.size())));
    for (int i = 0; i < nw; ++i)
        workers_.emplace_back(&HlsDownloader::Worker, this, i);
    for (auto& t : workers_) t.join();
    workers_.clear();
    progress_stop_ = true;
    if (progress_thread_.joinable()) progress_thread_.join();

    // Outcome.
    {
        std::lock_guard<std::mutex> lk(fail_mtx_);
        if (!fail_reason_.empty()) { SaveMeta(); return false; }
    }
    if (stop_requested_) { SaveMeta(); return false; }
    if (!ConcatTo(track_file)) {
        Fail("Failed to assemble the file (disk error?).");
        return false;
    }
    return true;
}

void HlsDownloader::Worker(int worker_index) {
    active_workers_++;
    for (;;) {
        if (stop_requested_ || abort_) break;
        const uint64_t idx = next_seg_++;
        if (idx >= segments_.size()) break;
        if (completed_[idx]) { segs_done_++; continue; }   // resume: skip

        std::vector<uint8_t> data;
        const Segment& seg = segments_[idx];
        if (!FetchSegment(seg.url, data, worker_index,
                          seg.ranged, seg.off, seg.len)) {
            if (!stop_requested_)
                Fail("Failed to download segment: " + seg.url);
            break;
        }
        if (!key_bytes_.empty() && !DecryptSegment(data, seg.seq)) {
            Fail("AES-128 decryption error (segment " + std::to_string(idx) + ").");
            break;
        }
        const std::string path = SegPath(idx);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(data.data()),
                           static_cast<std::streamsize>(data.size()))) {
            Fail("Failed to write segment to disk: " + path);
            break;
        }
        f.close();
        {
            std::lock_guard<std::mutex> lk(meta_mtx_);
            completed_[idx] = 1;
        }
        bytes_done_ += data.size();
        if ((++segs_done_) % 8 == 0) SaveMeta();
    }
    active_workers_--;
}

void HlsDownloader::ProgressLoop() {
    uint64_t last_bytes = 0;
    auto last_t = std::chrono::steady_clock::now();
    while (running_ && !progress_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!running_ || progress_stop_) break;
        uint64_t live = bytes_done_.load();
        for (const auto& c : inflight_) live += c->load();

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_t).count();
        // `live` may shrink when a retry resets its in-flight counter.
        const double speed =
            (dt > 0 && live >= last_bytes) ? (live - last_bytes) / dt : 0.0;
        last_bytes = live; last_t = now;

        // Total is an ESTIMATE: segment sizes are unknown until fetched, so
        // we extrapolate from the average of completed ones. Only the current
        // track's bytes feed the average (bytes_done_ spans all tracks).
        const uint64_t done = segs_done_.load();
        const uint64_t base = track_base_bytes_;
        double est = 0;
        if (done > 0)
            est = static_cast<double>(base) +
                  static_cast<double>(bytes_done_.load() - base) / done *
                      segments_.size();

        ProgressInfo pi;
        pi.id = id_;
        pi.total_bytes = est;
        pi.downloaded_bytes = static_cast<double>(live);
        pi.speed_bps = speed;
        pi.active_parts = active_workers_.load();
        pi.resuming = resuming_;
        if (progress_cb_) progress_cb_(pi);
    }
}

// ---------------------------------------------------------------------------
// Merge + result
// ---------------------------------------------------------------------------

bool HlsDownloader::ConcatTo(const fs::path& out_file) {
    std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::vector<char> buf(1 << 20);
    auto append = [&](const fs::path& p) -> bool {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got > 0 && !out.write(buf.data(), got)) return false;
        }
        return true;
    };
    if (is_fmp4_ && !init_url_.empty() && !append(parts_dir_ / "init.seg"))
        return false;
    for (size_t i = 0; i < segments_.size(); ++i)
        if (!append(SegPath(i))) return false;
    out.close();
    std::error_code ec;
    fs::remove_all(parts_dir_, ec);
    fs::remove(meta_path_, ec);
    return true;
}

void HlsDownloader::Fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(fail_mtx_);
        if (fail_reason_.empty()) fail_reason_ = msg;
    }
    abort_ = true;
}

void HlsDownloader::Finish() {
    {
        std::lock_guard<std::mutex> lk(fail_mtx_);
        result_.error_message = fail_reason_;
    }
    running_ = false;
    if (completion_cb_) completion_cb_(result_);
}

} // namespace odm
