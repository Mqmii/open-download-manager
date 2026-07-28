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
///   POST /add   -> JSON body {url, filename?, referrer?, cookies?,
///                             userAgent?, headers?{...}}
///                  requires X-ODM-Token; Origin (if present) must be a
///                  chrome-extension:// origin, otherwise 403.
///   OPTIONS *   -> CORS preflight (echoes chrome-extension:// origins only)
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
    bool        wsa_started_ = false;
};

} // namespace odm
