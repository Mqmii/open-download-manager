#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
#endif

#include "YtDlp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
  #include <windows.h>
#endif

namespace odm {
namespace ytdlp {

namespace fs = std::filesystem;

namespace {

// Format selection: highest-resolution video track plus the AAC audio track,
// with the progressive rungs as a tail for the uploads that offer nothing
// else. VP9 and AV1 are welcome — both stream-copy into .mp4 cleanly, so
// there is no reason to give up resolution for H.264.
//
// Both halves are the UNSTARRED "bv" and "ba", and that is the whole point.
// "bv*"/"ba*" also match progressive rungs, which already carry both streams;
// yt-dlp then refuses to merge two video (or two audio) streams and silently
// hands back that one rung instead. The failure has no error message and no
// obvious symptom beyond a download that is 360p, or silent, for no visible
// reason. Do not "simplify" these back to the starred forms.
std::string HeightFilter(int max_height) {
    if (max_height <= 0) return std::string();
    return "[height<=" + std::to_string(max_height) + "]";
}

std::string BuildFormat(int max_height) {
    const std::string h = HeightFilter(max_height);
    return "bv[protocol=https]" + h + "+ba[protocol=https][acodec^=mp4a]/"
           "bv[protocol=https]" + h + "+ba[protocol=https]/"
           "b[protocol=https]" + h + "[ext=mp4]/"
           "b[protocol=https]" + h + "/"
           "b[protocol=https]";
}

const char* kFormatAudioOnly = "ba[acodec^=mp4a]/ba";

#if defined(_WIN32)

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// CreateProcess takes one command line, not an argv, so every argument has to
// be quoted the way the CRT will parse it back apart again. Getting this wrong
// silently mangles URLs and output paths that contain spaces.
void AppendArg(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') cmd.append(slashes * 2 + 1, L'\\');
        else           cmd.append(slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}

std::string ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return std::string();
    return Narrow(fs::path(buf).parent_path().wstring());
}

#endif // _WIN32

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

bool StartsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

// Lowercased host of a URL, with any "www."/"m." prefix, credentials, port
// and path stripped — the same normalization IsSupportedUrl does inline.
std::string LowerHostOf(const std::string& url) {
    std::string u = url;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    const size_t p = u.find("://");
    if (p == std::string::npos) return std::string();
    std::string host = u.substr(p + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    const size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    host = host.substr(0, host.find(':'));
    if (StartsWith(host, "www.")) host = host.substr(4);
    if (StartsWith(host, "m."))   host = host.substr(2);
    return host;
}

// The '/'-separated segments of a URL path, empty ones skipped.
std::vector<std::string> PathSegments(const std::string& path) {
    std::vector<std::string> segs;
    size_t at = 0;
    while (at < path.size()) {
        const size_t next = path.find('/', at);
        const std::string seg = path.substr(
            at, (next == std::string::npos ? path.size() : next) - at);
        if (!seg.empty()) segs.push_back(seg);
        if (next == std::string::npos) break;
        at = next + 1;
    }
    return segs;
}

// A Vimeo video id: a run of digits, six or more. Ancient uploads have
// shorter ids, but confusing a feed page for a video is the worse failure.
bool LooksLikeVimeoId(const std::string& seg) {
    return seg.size() >= 6 && std::all_of(seg.begin(), seg.end(),
        [](unsigned char c) { return std::isdigit(c); });
}

} // namespace

// --- executable lookup ------------------------------------------------------

std::string ExecutablePath() {
#if defined(_WIN32)
    static std::string cached;
    static bool probed = false;
    if (probed) return cached;
    probed = true;

    const std::string dir = ExeDir();
    if (dir.empty()) return cached;
    std::error_code ec;
    for (const char* rel : {"yt-dlp.exe", "bin/yt-dlp.exe"}) {
        fs::path p = fs::u8path(dir) / rel;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            cached = p.u8string();
            break;
        }
    }
    return cached;
#else
    return std::string();
#endif
}

bool IsSupportedUrl(const std::string& url) {
    // Host match, not a substring search: "evil.com/?u=youtube.com" must not
    // be routed through the extractor.
    std::string u = url;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    size_t p = u.find("://");
    if (p == std::string::npos) return false;
    std::string host = u.substr(p + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    host = host.substr(0, host.find(':'));
    if (StartsWith(host, "www.")) host = host.substr(4);
    if (StartsWith(host, "m."))   host = host.substr(2);
    return host == "youtube.com" || host == "youtu.be" ||
           host == "music.youtube.com" || host == "youtube-nocookie.com" ||
           host == "vimeo.com" || host == "player.vimeo.com";
}

bool HasVideoId(const std::string& url) {
    if (!IsSupportedUrl(url)) return false;

    // Split off host, path and query without pulling in a URL parser: the
    // shapes accepted here are few and fixed.
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    std::string rest = url.substr(p + 3);
    const size_t slash = rest.find('/');
    std::string host = rest.substr(0, slash == std::string::npos ? rest.size() : slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash);
    std::string query;
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos) {
        if (path[q] == '?') query = path.substr(q + 1);
        path = path.substr(0, q);
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    auto looks_like_id = [](const std::string& s) {
        if (s.size() < 6) return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        });
    };

    if (host.find("youtu.be") != std::string::npos)
        return looks_like_id(path.size() > 1 ? path.substr(1) : "");

    // Vimeo: the numeric id is a path segment on every shape — /<id>,
    // /channels/x/<id>, /album/x/video/<id>, player.vimeo.com/video/<id>.
    // (Substring host match is safe here: IsSupportedUrl has already limited
    // the hosts to vimeo.com and player.vimeo.com plus their prefixes.)
    if (host.find("vimeo.com") != std::string::npos) {
        for (const std::string& seg : PathSegments(path))
            if (LooksLikeVimeoId(seg)) return true;
        return false;
    }

    if (path == "/watch") {
        // v=<id> anywhere in the query string.
        size_t at = 0;
        while (at < query.size()) {
            size_t amp = query.find('&', at);
            std::string kv = query.substr(at, amp == std::string::npos
                                                  ? std::string::npos : amp - at);
            if (StartsWith(kv, "v=")) return looks_like_id(kv.substr(2));
            if (amp == std::string::npos) break;
            at = amp + 1;
        }
        return false;
    }

    for (const char* pre : {"/shorts/", "/embed/", "/live/", "/v/"}) {
        if (StartsWith(path, pre)) {
            std::string id = path.substr(std::string(pre).size());
            id = id.substr(0, id.find('/'));
            return looks_like_id(id);
        }
    }
    return false;
}

std::string NormalizePageUrl(const std::string& page_url) {
    const std::string host = LowerHostOf(page_url);
    if (host != "vimeo.com" && host != "player.vimeo.com") return page_url;

    // Carve out the path; query and fragment are dropped either way.
    const size_t p = page_url.find("://");
    if (p == std::string::npos) return page_url;
    const size_t slash = page_url.find('/', p + 3);
    std::string path = slash == std::string::npos ? "" : page_url.substr(slash);
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path = path.substr(0, q);
    const std::vector<std::string> segs = PathSegments(path);

    // "/video/<id>" (embeds, albums, showcases) names the id directly; on
    // watch pages it is the numeric segment ("/33698814",
    // "/channels/staffpicks/33698814"). A non-numeric segment following the
    // id on a plain watch page is the unlisted hash, which the player takes
    // as ?h=.
    std::string id;
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        if ((segs[i] == "video" || segs[i] == "videos") &&
            LooksLikeVimeoId(segs[i + 1])) {
            id = segs[i + 1];
            break;
        }
    }
    if (id.empty()) {
        for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
            if (LooksLikeVimeoId(*it)) { id = *it; break; }
        }
    }
    if (id.empty()) return page_url;   // a Vimeo page, but not one video's

    std::string out = "https://player.vimeo.com/video/" + id;
    if (segs.size() == 2 && segs[0] == id && !LooksLikeVimeoId(segs[1]))
        out += "?h=" + segs[1];
    return out;
}

// --- process runner ---------------------------------------------------------

int Run(const std::vector<std::string>& args,
        const std::function<void(const std::string&)>& on_line,
        const std::atomic<bool>* cancel) {
#if defined(_WIN32)
    const std::string exe = ExecutablePath();
    if (exe.empty()) return -1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    // Only the write end may be inherited; leaving the read end inheritable
    // keeps a handle alive in the child and the pipe never reaches EOF.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    AppendArg(cmd, Widen(exe));
    for (const auto& a : args) AppendArg(cmd, Widen(a));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;   // CreateProcessW may write to this buffer
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);                  // our copy; the child owns the other one
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }

    std::string pending;
    char buf[4096];
    for (;;) {
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            // Nothing buffered: either the child is thinking, or it is gone
            // and the pipe is drained.
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) ||
                    avail == 0)
                    break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
        }
        DWORD got = 0;
        if (!ReadFile(rd, buf, (DWORD)std::min<DWORD>(avail, sizeof(buf)),
                      &got, nullptr) || got == 0)
            break;
        pending.append(buf, got);
        // yt-dlp's --newline progress uses \n; \r shows up in plain messages.
        size_t nl;
        while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && on_line) on_line(line);
        }
    }
    if (!pending.empty() && on_line) on_line(pending);

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return (int)code;
#else
    (void)args; (void)on_line; (void)cancel;
    return -1;
#endif
}

// --- resolve ----------------------------------------------------------------

bool Resolve(const std::string& page_url, MediaInfo* out, int max_height,
             const std::atomic<bool>* cancel) {
    if (!out) return false;
    if (ExecutablePath().empty()) {
        out->error = "yt-dlp.exe is missing from the ODM folder.";
        return false;
    }
    // Vimeo only: the watch page goes through an OAuth client Vimeo revoked,
    // its player embed page carries the same config with no token at all.
    const std::string url = NormalizePageUrl(page_url);
    // Refuse a feed/channel/home URL before it reaches yt-dlp: pointed at
    // those it happily starts extracting something, and "something" is not
    // what the user clicked download on.
    if (!HasVideoId(url)) {
        out->error = "This link does not point to a single video.";
        return false;
    }

    // One --print line carries the metadata behind a marker, a second one the
    // URLs (one per track). Parsing by marker instead of by line number keeps
    // a warning on stderr — or a title with a newline in it — from shifting
    // every field by one.
    const std::vector<std::string> args = {
        "--no-warnings", "--no-playlist", "--no-progress", "--encoding", "utf-8",
        "--no-check-formats", "--socket-timeout", "20",
        "-f", BuildFormat(max_height),
        "--print", "ODMINFO\x1f%(ext)s\x1f%(filesize_approx)s\x1f%(title)s",
        "--print", "%(urls)s",
        url
    };

    std::vector<std::string> urls;
    std::string meta, log_tail;
    const int code = Run(args, [&](const std::string& raw) {
        const std::string line = Trim(raw);
        if (line.empty()) return;
        if (StartsWith(line, "ODMINFO\x1f")) { meta = line; return; }
        if (StartsWith(line, "https://"))    { urls.push_back(line); return; }
        if (log_tail.size() < 400) log_tail += line + " ";
    }, cancel);

    if (!meta.empty()) out->identified = true;

    if (code != 0 || urls.empty()) {
        out->error = log_tail.empty()
            ? "yt-dlp could not read this video."
            : Trim(log_tail);
        return false;
    }

    if (!meta.empty()) {
        // ODMINFO \x1f ext \x1f size \x1f title  (the title may contain \x1f
        // itself, so split only the first three separators).
        std::vector<std::string> f;
        size_t pos = 0;
        for (int i = 0; i < 3; ++i) {
            size_t sep = meta.find('\x1f', pos);
            if (sep == std::string::npos) break;
            f.push_back(meta.substr(pos, sep - pos));
            pos = sep + 1;
        }
        f.push_back(meta.substr(pos));
        if (f.size() == 4) {
            if (f[1] != "NA" && !f[1].empty()) out->ext = f[1];
            if (f[2] != "NA" && !f[2].empty()) out->size = atof(f[2].c_str());
            out->title = f[3];
        }
    }

    out->video_url = urls[0];
    if (urls.size() > 1) out->audio_url = urls[1];
    return true;
}

// --- quality list -----------------------------------------------------------

bool ListHeights(const std::string& page_url, std::vector<int>* out) {
    if (!out) return false;
    out->clear();
    const std::string url = NormalizePageUrl(page_url);
    if (ExecutablePath().empty() || !HasVideoId(url)) return false;

    // One JSON object per format, holding only the three fields that decide
    // whether a rung belongs in the menu. Asking for the fields this way (and
    // not for the whole info dict) keeps the answer small enough to read with
    // a scan instead of a JSON parser.
    const std::vector<std::string> args = {
        "--no-warnings", "--no-playlist", "--no-progress", "--encoding", "utf-8",
        "--socket-timeout", "20",
        "--print", "%(formats.:.{height,vcodec,protocol})j",
        url
    };

    std::string json;
    Run(args, [&](const std::string& line) {
        if (!line.empty() && line[0] == '[') json = line;
    }, nullptr);
    if (json.empty()) return false;

    std::vector<int> heights;
    size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string::npos) {
        const size_t end = json.find('}', pos);
        if (end == std::string::npos) break;
        const std::string obj = json.substr(pos, end - pos);
        pos = end + 1;

        // Storyboards report vcodec "none" and an mhtml protocol; audio-only
        // rungs carry no height at all. Neither is a quality the user can pick.
        if (obj.find("\"vcodec\": \"none\"") != std::string::npos) continue;
        if (obj.find("\"protocol\": \"https\"") == std::string::npos) continue;
        const size_t hk = obj.find("\"height\": ");
        if (hk == std::string::npos) continue;
        const int h = atoi(obj.c_str() + hk + 10);
        if (h > 0 && std::find(heights.begin(), heights.end(), h) == heights.end())
            heights.push_back(h);
    }

    std::sort(heights.begin(), heights.end(), std::greater<int>());
    *out = std::move(heights);
    return !out->empty();
}

// --- self-update ------------------------------------------------------------

void SelfUpdateAsync() {
#if defined(_WIN32)
    const std::string exe = ExecutablePath();
    if (exe.empty()) return;

    std::thread([exe] {
        std::error_code ec;
        const fs::path stamp = fs::u8path(exe).parent_path() / "yt-dlp.stamp";
        const auto now = std::time(nullptr);
        if (fs::exists(stamp, ec)) {
            long long last = 0;
            FILE* f = nullptr;
            if (_wfopen_s(&f, stamp.c_str(), L"rb") == 0 && f) {
                char b[32] = {0};
                fread(b, 1, sizeof(b) - 1, f);
                fclose(f);
                last = atoll(b);
            }
            if (last > 0 && now - last < 7 * 24 * 3600) return;   // fresh enough
        }
        // Write the stamp first: a failing update (no write permission in
        // Program Files, no network) must not retry on every launch.
        FILE* f = nullptr;
        if (_wfopen_s(&f, stamp.c_str(), L"wb") == 0 && f) {
            fprintf(f, "%lld", (long long)now);
            fclose(f);
        }
        Run({"-U", "--no-warnings"}, nullptr, nullptr);
    }).detach();
#endif
}

std::string VideoOnlyFormat(int max_height) {
    return "bv" + HeightFilter(max_height) + "/bv";
}
const char* AudioOnlyFormat() { return kFormatAudioOnly; }

} // namespace ytdlp
} // namespace odm
