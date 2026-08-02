#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace odm {

/// The one extension this bridge answers to.
///
/// Chrome derives an extension's id from the public key in its manifest, and
/// an unpacked extension without one is identified by the PATH it was loaded
/// from — a different id on every machine, which is why the origin check used
/// to accept `chrome-extension://` wholesale. extension/manifest.json now
/// carries a fixed `key`, so the id below is the same everywhere and can be
/// named. Anything else claiming to be an extension is somebody else's.
///
/// Keep this in step with the `key` field: the id is the first 16 bytes of
/// SHA-256 over the key's DER bytes, each nibble mapped 0-15 to 'a'-'p'.
constexpr const char* kExtensionId = "pmhdndfledfenepnlhddknpkihaiiaeh";

/// True when `origin` is exactly our extension's origin.
///
/// This is a real gate against the ordinary case — an extension that has not
/// asked for loopback host permission is subject to CORS, so Chrome both sends
/// the Origin header and enforces the answer. It is NOT a gate against an
/// extension that declares `http://127.0.0.1/*` itself: Chrome exempts those
/// from CORS and may omit Origin entirely, which is also why a request with no
/// Origin at all is still tolerated (that is what our own extension sends).
bool OriginIsOurExtension(const std::string& origin);

/// May `name: value` be put on the wire as one request header?
///
/// Extra headers arrive as JSON — from the bridge's POST body, or from the
/// request context the UI hands to StartDownload — and JSON string escapes
/// can spell out a carriage return. libcurl does not police the header lines
/// it is given, so a value containing CRLF does not stay a value: everything
/// after it becomes further headers, and a body after a blank line becomes a
/// second request, sent to the same host with the same cookies attached.
///
/// The rules are RFC 7230's: a name is a token, and a value carries no CR, LF
/// or NUL. Length is capped too, so a hand-off cannot push a megabyte of
/// header at whatever it is downloading from.
bool HeaderPairSafe(const std::string& name, const std::string& value);

/// Minimal flat-JSON parser used by both the bridge (POST bodies) and the
/// UI bridge (context JSON from JavaScript). Supports string values only;
/// nested objects are flattened to "key.subkey"; numbers/bools/null are
/// skipped. Returns false on malformed input.
bool ParseSimpleJson(const std::string& json,
                     std::map<std::string, std::string>& out);

/// Payload delivered by POST /add (a browser hand-off request).
struct BridgePayload {
    std::string url;
    std::string filename;     // server/browser-suggested file name (may be "")
    std::string referrer;
    std::string cookies;      // "k=v; k2=v2" header form
    // The same cookies with the scope the browser holds them under, one
    // Netscape cookie-file line each, newline separated. This is what the
    // downloader actually sends: the header form above cannot say which host
    // a cookie belongs to, so it cannot survive a redirect safely.
    std::string cookie_jar;
    std::string user_agent;
    std::vector<std::pair<std::string, std::string>> headers; // extra headers
    // Second track for paired-track DASH (Instagram/Facebook): the audio-only
    // representation that belongs to `url`'s video rung. Empty otherwise.
    std::string audio_url;
    // "" (plain HTTP) | "hls" | "dash" | "ytdlp" (a watch page the app
    // resolves with yt-dlp before any byte is fetched).
    std::string type;
    // "ytdlp" only: the quality the user picked from the panel's menu, as a
    // video height ("1080"). Empty means best available.
    std::string height;
};

///
/// Tiny HTTP/1.1 server bound to 127.0.0.1 only, so the Chrome extension can
/// hand downloads to this running instance. No external dependencies — pure
/// WinSock on Windows.
///
/// Endpoints:
///   GET  /ping  -> {"app":"odm","version":"...","token":"..."}
///                  the token is withheld from a foreign extension origin.
///   POST /add   -> JSON body {url, filename?, referrer?, cookies?,
///                             cookieJar?, userAgent?, headers?{...}}
///                  requires X-ODM-Token; Origin (if present) must be our own
///                  extension's origin, otherwise 403.
///   OPTIONS *   -> CORS preflight (echoes our extension's origin only)
///
/// Every request must carry a Host naming this server's own loopback address;
/// anything else is a DNS-rebinding attempt and gets 403 before it is routed.
///
/// Connections are handled one request per connection and closed
/// ("Connection: close"), which keeps the parser simple and robust.
///
class BridgeServer {
public:
    using AddCallback = std::function<void(const BridgePayload&)>;

    BridgeServer() = default;
    ~BridgeServer();

    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    /// Bind 127.0.0.1:port and start the listener thread. Returns false when
    /// the port is unavailable or WinSock fails — the app then runs without
    /// the bridge (degraded, never fatal).
    bool Start(uint16_t port, AddCallback cb);

    /// Stop the listener thread and release the port. Safe to call twice.
    void Stop();

    bool IsRunning() const { return running_.load(); }
    const std::string& Token() const { return token_; }

private:
    void ListenLoop();
    void HandleClient(uintptr_t client);

    std::atomic<bool> running_{false};
    uintptr_t  listen_sock_ = static_cast<uintptr_t>(~0ull); // SOCKET
    std::thread thread_;
    AddCallback cb_;
    std::string token_;
    uint16_t    port_ = 0;      // the bound port, checked against Host
    bool        wsa_started_ = false;
};

} // namespace odm
