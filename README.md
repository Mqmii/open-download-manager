# ODM — Open Download Manager

A multi-segment download manager for Windows with a browser extension that
captures video from the page. Written in C++17, with the interface built as a
web page and rendered by [Ultralight](https://ultralig.ht).

![Main window](docs/screenshot-main.png)

---

## What it does

- **Multi-segment HTTP downloads.** The file is pre-allocated and split into
  many small chunks; connections work on different regions at once and a
  connection that runs dry steals pending work, so no single slow chunk holds
  up the tail.
- **Resume that survives a restart.** Progress is kept in a sidecar next to the
  file, so an interrupted download continues where it stopped rather than
  starting over.
- **Video capture from the browser.** The extension watches page media and
  offers a download button over the video you are actually watching — plain
  files, HLS/CMAF streams, and the paired video/audio streams Instagram and
  Facebook use.
- **YouTube.** The extraction step — and only that step — is delegated to
  `yt-dlp`, which ships inside the release ZIP as a single executable, so
  nothing is installed by hand. The bytes themselves are fetched by ODM's own
  multi-segment engine.
- **Lossless remux.** When a stream ships video and audio separately, both
  tracks are downloaded and merged with libavformat. Codecs are copied, never
  re-encoded.
- **Nothing is lost on hand-off.** The extension only cancels Chrome's own
  download *after* the app confirms it accepted the job.

---

## Browser integration

The app runs a small HTTP bridge on `127.0.0.1:47923`, bound to loopback only
and authenticated with a token that is regenerated on every launch. The
extension talks to it over that bridge.

```
┌──────────────────────── Chrome ────────────────────────┐
│                                                        │
│  downloads.onDeterminingFilename ──┐                   │
│  (a normal browser download)       │                   │
│                                    ├──► background.js  │
│  webRequest media sniffer ─────────┤   (service worker)│
│  (mp4 / m3u8 / DASH rungs)         │         │         │
│                                    │         │         │
│  content.js  ── floating panel ────┘         │         │
│  over the video on screen                    │         │
│                                              │         │
│  mse_probe.js (page world)                   │         │
│  binds the playing <video> to its real URLs  │         │
└──────────────────────────────────────────────┼─────────┘
                                               │
                       POST /add  +  X-ODM-Token, cookies,
                       referrer, User-Agent, suggested name
                                               │
                                               ▼
┌──────────────────────────── ODM ───────────────────────┐
│  BridgeServer ──► Add-URL dialog, prefilled             │
│                        │                                │
│                        ▼        route by job type       │
│         Downloader ── HlsDownloader ── DashDownloader   │
│         (plain HTTP)   (m3u8/CMAF)     (paired tracks)  │
│                        └────────┬──────────┘            │
│                                 ▼                       │
│                          Muxer (libavformat)            │
└─────────────────────────────────────────────────────────┘
```

**Two ways a download reaches the app.** A normal browser download is
intercepted before Chrome writes anything to disk and handed over with the
cookies, referrer and User-Agent that made it work in the browser — the reason
a link that needs a session still downloads correctly outside it. A page video
is offered through a floating panel over the player.

![Download panel over a video player](docs/screenshot-extension.png)

The panel names what you actually get: a plain file by size, an HLS master as
"video + audio" with its resolution, and a silent CMAF track labelled as such,
so a stream is never handed over as if it were a finished file. Clicking it
sends the URL to ODM, which opens its Add-URL dialog prefilled.

**Picking the right video.** On sites that stream through Media Source
Extensions, every `<video>` carries a `blob:` source and a feed keeps a dozen
unrelated clips buffered at once, so nothing in the network traffic says which
clip is on screen. Ranking candidates by size or recency is a guess, and
guessing wrong hands you a different video. `mse_probe.js` instead watches the
player itself — `createObjectURL`, `addSourceBuffer`, `appendBuffer` and the
`fetch` responses feeding them — and binds the element you are watching to the
exact URLs behind it. On a page with 17 buffered clips this resolves to a
single candidate.

**YouTube.** A watch page is not a file, and its media URLs are signed by a
player script that changes every few weeks — re-implementing that in C++ is a
standing invitation to be broken. So the panel hands over the *page* URL, and
the app asks the bundled `yt-dlp.exe` for the video and audio URLs behind it.
What comes back are ordinary Range-capable links, which go into the same
multi-segment engine as everything else and are muxed by the same libavformat
code; the download is ours, only the extraction is delegated. When a video has
no such link, yt-dlp fetches it directly instead — slower, one connection, but
it always has an answer. The panel's dropdown lists the qualities this
particular video actually has, highest first, so a 720p upload is never
offered as 4K; the list comes from the app over the same bridge and is cached
per video, because answering it means running the extractor. ODM refreshes `yt-dlp.exe` in the background at most
once a week, which is what keeps this working long after a release was cut.

**Installing the extension.** It is not on the Chrome Web Store, so load it
unpacked: open `chrome://extensions`, turn on *Developer mode*, choose *Load
unpacked* and pick the `extension/` folder. Start ODM first — the extension
polls the bridge and picks up its token automatically; the popup shows whether
it is connected.

---

## Screenshots

Per-connection view of a running download, with the segment map and the
resume state reported by the engine:

![Download detail](docs/screenshot-detail.png)

Settings — download location, connection count, the size below which
segmenting is skipped, and a global speed cap:

![Options](docs/screenshot-options.png)

Per-download actions:

![Context menu](docs/screenshot-context-menu.png)

---

## Building

**Requirements**

- Windows, Visual Studio 2022 (x64)
- CMake 3.20+
- [vcpkg](https://vcpkg.io) for the third-party libraries
- The Ultralight SDK — **not included in this repository**

**Dependencies** (via vcpkg)

```bash
vcpkg install curl ffmpeg libmediainfo
```

**Ultralight SDK.** Download the SDK from [ultralig.ht](https://ultralig.ht)
and place it at `ultralight-sdk/` in the repository root, so that
`ultralight-sdk/include/` and `ultralight-sdk/lib/` exist. It carries its own
licence and is deliberately kept out of version control.

**Configure and build**

```bash
cmake --preset default
```
```bash
cmake --build build --config Release
```

**yt-dlp.** Not in the repository: it is a 17 MB binary with its own release
cadence. `tools/package.ps1` downloads it when building a release ZIP; for a
local build, drop `yt-dlp.exe` into `tools/` and the build copies it next to
`ODM.exe`. Everything else works without it — only YouTube links do not.

The executable lands in `build/Release/ODM.exe`, with `assets/` copied beside
it. Ultralight loads the interface from that folder at startup, so changing a
file under `assets/` only needs a restart, not a rebuild.

---

## Project layout

```
src/
  main.cpp              entry point (single-instance mutex)
  ODMApp.{h,cpp}        window, JS bindings, tray icon, bridge glue
  Downloader.{h,cpp}    multi-segment HTTP engine (libcurl)
  HlsDownloader.{h,cpp} HLS/CMAF: playlists, byte ranges, AES-128, dual tracks
  DashDownloader.{h,cpp} paired-track DASH (Meta CDNs): video rung + audio rung
  YtDlp.{h,cpp}         yt-dlp wrapper: page URL -> direct media URLs
  YtDlpDownloader.{h,cpp} fallback engine for videos with no plain media URL
  Muxer.{h,cpp}         libavformat stream-copy remux into one .mp4
  BridgeServer.{h,cpp}  loopback HTTP bridge (WinSock)
assets/                 the interface: index.html, app.css, app.js
extension/              Chrome extension (Manifest V3)
```

The interface is an ordinary web page, so `assets/index.html` can be opened in
a browser to work on the layout without launching the app. Everything native
is exposed to it as a handful of JavaScript functions.

---

## Roadmap

- [x] Multi-segment HTTP downloads with resume
- [x] Browser extension with download interception
- [x] Video sniffing with an in-page download panel
- [x] HLS/CMAF engine with separate-audio remux
- [x] Paired-track DASH (Instagram / Facebook)
- [x] Identity-based capture on MSE players
- [x] YouTube support (yt-dlp resolves, ODM downloads)
- [x] Download queue — jobs wait their turn instead of interrupting each other
- [ ] Several simultaneous jobs (the engine still runs one at a time)
- [ ] Pause per job, not just a global stop
- [ ] DASH manifest (`.mpd`) engine
- [ ] Long VODs (Twitch / Kick): streaming concat so a 30 GB recording does not
      need twice its size in free space, and playlist refresh for URLs that
      expire mid-download
- [ ] Scheduler and category rules
- [ ] Checksum verification

---

## Notes

The bridge listens on loopback only and rejects requests without the current
token, so nothing outside the machine can queue a download.

## Licence

Licensed under the GNU Affero General Public License v3.0 — see
[`LICENSE`](LICENSE).

In short: fork it, change it, ship it. If you distribute your version, or run
it as a service, your version has to be open source under the same licence.
Closed-source forks are not permitted.

`LICENSE` also carries an **additional permission under AGPL section 7** for
linking against the Ultralight SDK, which is proprietary. Without it the
combined work could not be distributed at all.

Third-party components keep their own terms. `yt-dlp` is in the public domain
(Unlicense) and is redistributed unmodified. The Ultralight SDK is licensed
separately and must be obtained from its vendor. FFmpeg (libavformat /
libavcodec), libcurl, MediaInfoLib, tinyxml2 and zlib carry their own licences,
which matter in particular if you distribute a compiled binary rather than
source.

The copyright is held by a single author, so commercial licences on different
terms can be granted on request.
