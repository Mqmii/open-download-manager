#pragma once

#include <AppCore/App.h>
#include <AppCore/JSHelpers.h>
#include <AppCore/Overlay.h>
#include <AppCore/Window.h>
#include <Ultralight/ConsoleMessage.h>
#include <Ultralight/String.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "BridgeServer.h"
#include "Downloader.h"
#include "DashDownloader.h"
#include "HlsDownloader.h"
#include "YtDlpDownloader.h"

namespace odm {

///
/// Main application: creates the Ultralight window, loads the UI, and bridges
/// JavaScript calls to the Downloader engine.
///
class ODMApp : public ultralight::WindowListener,
               public ultralight::LoadListener,
               public ultralight::ViewListener,
               public ultralight::AppListener {
public:
    ODMApp();
    virtual ~ODMApp();

    void Run();

    // ultralight::WindowListener
    void OnClose(ultralight::Window* window) override;
    void OnResize(ultralight::Window* window,
                  uint32_t width, uint32_t height) override;

    // ultralight::ViewListener
    void OnChangeCursor(ultralight::View* caller,
                        ultralight::Cursor cursor) override;
    // console.log() from the UI. Silent unless ODM_CONSOLE_LOG=1 is set in the
    // environment, in which case every message is appended to
    // odm-console.log next to the executable — the only way to see JS output
    // from the packaged app (there is no attached console and no inspector).
    void OnAddConsoleMessage(ultralight::View* caller,
                             const ultralight::ConsoleMessage& msg) override;

    // ultralight::LoadListener
    void OnDOMReady(ultralight::View* caller,
                    uint64_t frame_id,
                    bool is_main_frame,
                    const ultralight::String& url) override;

    // ultralight::AppListener — runs on the main thread, used to flush the
    // pending JS queue populated by download worker threads.
    void OnUpdate() override;

private:
    // ---- JavaScript entry points (bound from OnDOMReady) ----
    void JS_StartDownload(const ultralight::JSObject& thisObj,
                          const ultralight::JSArgs& args);
    void JS_StopDownload(const ultralight::JSObject& thisObj,
                         const ultralight::JSArgs& args);
    void JS_ApplySettings(const ultralight::JSObject& thisObj,
                          const ultralight::JSArgs& args);
    ultralight::JSValue JS_PickSavePath(const ultralight::JSObject& thisObj,
                                        const ultralight::JSArgs& args);
    ultralight::JSValue JS_PickFolder(const ultralight::JSObject& thisObj,
                                      const ultralight::JSArgs& args);
    ultralight::JSValue JS_GetDiskSpace(const ultralight::JSObject& thisObj,
                                        const ultralight::JSArgs& args);
    ultralight::JSValue JS_OpenFile(const ultralight::JSObject& thisObj,
                                    const ultralight::JSArgs& args);
    // Windows' own "Open with" chooser for a finished download. Runs the
    // modal on a worker thread so the UI keeps painting while it is up.
    ultralight::JSValue JS_OpenWith(const ultralight::JSObject& thisObj,
                                    const ultralight::JSArgs& args);
    ultralight::JSValue JS_OpenFolder(const ultralight::JSObject& thisObj,
                                      const ultralight::JSArgs& args);
    ultralight::JSValue JS_OpenDownloadsFolder(const ultralight::JSObject& thisObj,
                                               const ultralight::JSArgs& args);
    // Open the releases page in the default browser. Takes no argument: the
    // URL is a native constant, so the interface cannot ask the shell to open
    // something else.
    ultralight::JSValue JS_OpenReleasesPage(const ultralight::JSObject& thisObj,
                                            const ultralight::JSArgs& args);
    ultralight::JSValue JS_DeleteFile(const ultralight::JSObject& thisObj,
                                      const ultralight::JSArgs& args);
    // Remove the partial file of an unfinished download together with every
    // resume sidecar written beside it.
    ultralight::JSValue JS_DeletePartial(const ultralight::JSObject& thisObj,
                                         const ultralight::JSArgs& args);
    // Analyze a file on disk with MediaInfoLib on a worker thread; the result
    // is delivered asynchronously via UI.onMediaInfo(requestId, text).
    void JS_RequestMediaInfo(const ultralight::JSObject& thisObj,
                             const ultralight::JSArgs& args);
    // Probe a URL (HEAD, falling back to a 1-byte ranged GET) on a worker
    // thread so the Add-Download modal can show file type / size / name
    // BEFORE the user commits. Result arrives via UI.onUrlProbe(id, json).
    void JS_ProbeUrl(const ultralight::JSObject& thisObj,
                     const ultralight::JSArgs& args);
    // Per-part snapshot of the running download (JSON string) for the
    // download-detail modal.
    ultralight::JSValue JS_GetPartInfo(const ultralight::JSObject& thisObj,
                                       const ultralight::JSArgs& args);
    // Compute and/or verify a file's checksum on a worker thread; the result
    // is delivered asynchronously via UI.onChecksum(requestId, json).
    void JS_VerifyChecksum(const ultralight::JSObject& thisObj,
                           const ultralight::JSArgs& args);

    // One independent download slot per active job, so several jobs can run
    // simultaneously and any individual job can be paused without stopping
    // the rest.
    struct JobSlot {
        std::string                        id;
        std::string                        out_path;
        std::atomic<bool>                  suppress_callbacks{false};
        std::shared_ptr<std::atomic<bool>> resolve_cancel;
        std::unique_ptr<Downloader>        downloader;
        std::unique_ptr<HlsDownloader>     hls;
        std::unique_ptr<DashDownloader>    dash;
        std::unique_ptr<YtDlpDownloader>   ytdlp;

        bool IsRunning() const {
            if (resolve_cancel && !resolve_cancel->load()) return true;
            if (downloader && downloader->IsRunning()) return true;
            if (hls && hls->IsRunning()) return true;
            if (dash && dash->IsRunning()) return true;
            if (ytdlp && ytdlp->IsRunning()) return true;
            return false;
        }

        void Stop() {
            if (resolve_cancel) resolve_cancel->store(true);
            if (downloader) downloader->Stop();
            if (hls) hls->Stop();
            if (dash) dash->Stop();
            if (ytdlp) ytdlp->Stop();
        }

        void WaitForCompletion() {
            if (downloader) downloader->WaitForCompletion();
            if (hls) hls->WaitForCompletion();
            if (dash) dash->WaitForCompletion();
            if (ytdlp) ytdlp->WaitForCompletion();
        }
    };

    // Tail of JS_StartDownload: resolve the output path, announce the job and
    // route it to an engine. Split out because a YouTube job cannot get here
    // synchronously — its file name and media URLs only exist after yt-dlp has
    // answered, so that path re-enters through here from the main thread.
    // `resume_key` binds the resume sidecar to something steadier than the
    // media URL. It is empty for everything whose URL is stable, and the watch
    // page for a YouTube job, whose signed rung URLs are re-resolved (and so
    // differ) on every launch.
    void BeginDownload(std::shared_ptr<JobSlot> slot,
                       std::string url, std::string out_path, std::string id,
                       RequestContext ctx, std::string suggested_name,
                       std::string job_type, std::string audio_url,
                       int max_height = 0,
                       std::string resume_key = std::string());

    // Ask yt-dlp for the media URLs of a watch page on a worker thread, then
    // continue in BeginDownload on the main thread.
    void StartYouTubeDownload(std::shared_ptr<JobSlot> slot,
                              const std::string& page_url,
                              const std::string& out_path,
                              const std::string& id,
                              const std::string& suggested_name,
                              int max_height);

    // Divide the global speed limit across all currently running jobs.
    void UpdateActiveSpeedLimits();

    // ---- Engine callbacks ----
    void OnProgress(const ProgressInfo& info);
    void OnComplete(const DownloadResult& result);

    // ---- Browser bridge ----
    // Called from the BridgeServer listener thread when the Chrome extension
    // hands us a download. Marshals to the main thread via the native task
    // queue, brings the window forward, and notifies the UI.
    void OnExternalDownload(const BridgePayload& payload);
    void BringWindowToFront();

    // Push a JS snippet to be evaluated on the main thread during OnUpdate.
    // Safe to call from any thread.
    void PostJS(const std::string& script);

    // Push a native callable to run on the main thread during OnUpdate.
    // Safe to call from any thread.
    void PostNativeTask(std::function<void()> fn);

    ultralight::RefPtr<ultralight::App>    app_;
    ultralight::RefPtr<ultralight::Window> window_;
    ultralight::RefPtr<ultralight::Overlay> overlay_;

    std::map<std::string, std::shared_ptr<JobSlot>> jobs_;
    int      part_count_  = 8;
    uint64_t threshold_   = 1 * 1024 * 1024;
    uint64_t speed_limit_ = 0;
    ultralight::RefPtr<ultralight::JSContext> js_ctx_;

    // Localhost bridge to the browser extension (127.0.0.1:47923).
    BridgeServer bridge_;
    void*        hwnd_ = nullptr;   // cached native window handle (Windows)

    // Dedicated thread owning the system-tray icon and its message loop.
    std::thread  tray_thread_;

    // Work handed back to the main thread. This lives in its own shared object
    // rather than as plain members because background jobs run detached:
    // the URL probe, MediaInfo, checksum verification, the YouTube resolver
    // and the "Open with" dialog. Nothing joins them — the dialog in particular
    // stays open as long as the user leaves it open — so one can still be
    // mid-flight when the window closes. Those workers capture the mailbox
    // instead of `this`, so a late result posts into an object guaranteed to
    // outlive the app, and Close() turns such posts into no-ops rather than a
    // use-after-free.
    struct Mailbox {
        std::mutex                        mtx;
        std::queue<std::string>           js;
        std::queue<std::function<void()>> tasks;
        bool                              open = true;

        // All three are safe to call from any thread, at any time.
        void PostJS(const std::string& script);
        void PostTask(std::function<void()> fn);
        // Stop accepting work and discard whatever is still queued; nothing
        // will run it now that the main thread is on its way out.
        void Close();
    };
    std::shared_ptr<Mailbox> mail_ = std::make_shared<Mailbox>();

    // The update check runs off-thread and can answer either side of the page
    // being ready, so the answer is parked here and delivered by whichever of
    // the two happens second. UI.onUpdateAvailable tolerates being told twice.
    //
    // Shared, not a plain member: the worker sits in a socket timeout for up
    // to fifteen seconds, which is long enough for someone to quit. A lambda
    // holding `this` would then write into a destroyed app — the same reason
    // the mailbox is a shared_ptr.
    struct UpdateSlot {
        std::mutex  mtx;
        std::string version;
    };
    std::shared_ptr<UpdateSlot> update_slot_ = std::make_shared<UpdateSlot>();
};

} // namespace odm
