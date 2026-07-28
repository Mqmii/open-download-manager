'use strict';
/**
 * ODM — Download Interceptor + Media Sniffer (service worker, Manifest V3)
 *
 * Flow (file downloads):
 *  1. User starts a download in Chrome (or right-clicks a link -> "Download with ODM").
 *  2. We POST the URL (+ cookies / referrer / User-Agent / suggested filename)
 *     to the ODM desktop app listening on http://127.0.0.1:47923.
 *  3. Only AFTER the app answers {"ok":true} do we cancel Chrome's own
 *     download — so a failed hand-off never loses the user's download.
 *
 * Flow (video sniffer, Phase 1):
 *  1. webRequest observes media responses (mp4/webm/... + HLS/DASH manifests).
 *  2. Findings are kept per-tab (persisted in chrome.storage.session so the
 *     list survives service-worker restarts) and shown as a badge.
 *  3. The content script's floating "Download" panel asks for the tab's media
 *     list and hands the chosen URL to the desktop app via sendToOdm().
 */

const ODM_BASE  = 'http://127.0.0.1:47923';
const PING_URL  = ODM_BASE + '/ping';
const ADD_URL   = ODM_BASE + '/add';
const PING_ALARM = 'odm-ping';

// ---------------------------------------------------------------------------
// In-memory state (service workers are killed after ~30s idle, so anything
// read in a synchronous event gate — like captureEnabled — must be cached in
// memory and kept in sync via storage.onChanged).
// ---------------------------------------------------------------------------
let token = null;
let connected = false;
let captureEnabled = true;

async function loadState() {
  const data = await chrome.storage.local.get({
    odmToken: null,
    captureEnabled: true
  });
  token = data.odmToken;
  captureEnabled = !!data.captureEnabled;
}

chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== 'local') return;
  if (changes.captureEnabled) captureEnabled = !!changes.captureEnabled.newValue;
  if (changes.odmToken)       token = changes.odmToken.newValue;
});

// ---------------------------------------------------------------------------
// Ping / token handshake
// ---------------------------------------------------------------------------
async function ping() {
  try {
    const r = await fetch(PING_URL, { cache: 'no-store' });
    if (!r.ok) throw new Error('http ' + r.status);
    const j = await r.json();
    if (j && j.token) {
      token = j.token;
      await chrome.storage.local.set({ odmToken: token });
    }
    connected = true;
  } catch (e) {
    connected = false;
  }
  return connected;
}

// Keep the connection state (and the worker itself) fresh. chrome.alarms is
// the only MV3-safe periodic mechanism (setInterval dies with the worker).
chrome.alarms.create(PING_ALARM, { periodInMinutes: 0.5 });
chrome.alarms.onAlarm.addListener(alarm => {
  if (alarm.name === PING_ALARM) ping();
});
chrome.runtime.onStartup.addListener(ping);
chrome.runtime.onInstalled.addListener(async () => {
  await loadState();
  await ping();
  createMenus();
});

// Cold start of the service worker.
loadState().then(ping);

// ---------------------------------------------------------------------------
// Context menu: "Download with ODM" on links / media / pages
// ---------------------------------------------------------------------------
function createMenus() {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: 'odm-download',
      title: 'Download with ODM',
      contexts: ['link', 'video', 'audio', 'page']
    });
  });
}
createMenus(); // harmless if menus already exist (removeAll first)

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (info.menuItemId !== 'odm-download') return;
  const url = info.linkUrl || info.srcUrl || info.pageUrl || '';
  if (!/^https?:\/\//i.test(url)) {
    notify('ODM', 'This link type is not supported (http/https only).');
    return;
  }
  const ok = await sendToOdm({
    url,
    filename: filenameFromUrl(url),
    referrer: info.pageUrl || (tab && tab.url) || ''
  });
  if (!ok) {
    notify('ODM is not running',
           'Could not hand off the download. Please start the ODM app first.');
  }
});

// ---------------------------------------------------------------------------
// Download interception
// ---------------------------------------------------------------------------
// onDeterminingFilename fires BEFORE Chrome writes anything to disk, which
// makes it the correct interception point (onCreated would be too late and
// would leave partial files behind).
chrome.downloads.onDeterminingFilename.addListener((item /*, suggest */) => {
  // Synchronous gate only — no awaits allowed here.
  if (!captureEnabled) return;
  if (item.byExtId === chrome.runtime.id) return;   // loop prevention
  const url = item.finalUrl || item.url || '';
  if (!/^https?:\/\//i.test(url)) return;           // skip blob:, data:, file:
  // We never call suggest(): we either cancel (after a successful hand-off)
  // or leave Chrome's download completely untouched.
  takeOver(item, url);
});

async function takeOver(item, url) {
  const ok = await sendToOdm({
    url,
    filename: baseName(item.filename || '') || filenameFromUrl(url),
    referrer: item.referrer || ''
  });

  if (!ok) {
    // App not reachable: Chrome keeps downloading — no data loss.
    notify('ODM is not running',
           'Could not take over the download; Chrome keeps downloading. ' +
           'Start the ODM app to enable takeover.');
    return;
  }

  // Hand-off accepted — NOW cancel Chrome's download (in this order!).
  try { await chrome.downloads.cancel(item.id); } catch (e) {}
  try { await chrome.downloads.removeFile(item.id); } catch (e) {}
  try { await chrome.downloads.erase(item.id); } catch (e) {}
}

// ---------------------------------------------------------------------------
// Bridge communication
// ---------------------------------------------------------------------------
async function sendToOdm(payload) {
  if (!connected || !token) await ping();
  if (!connected || !token) return false;

  // One POST attempt; on 401 the app restarted with a new token -> refresh
  // once and retry.
  for (let attempt = 0; attempt < 2; ++attempt) {
    try {
      if (!payload.cookies)   payload.cookies   = await cookieHeader(payload.url);
      if (!payload.userAgent) payload.userAgent = navigator.userAgent;
      const r = await fetch(ADD_URL, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'X-ODM-Token': token
        },
        body: JSON.stringify(payload)
      });
      if (r.ok) return true;
      if (r.status === 401 && attempt === 0) {
        token = null;
        await ping();           // fetch the fresh token, then loop retries
        continue;
      }
      return false;
    } catch (e) {
      connected = false;
      return false;
    }
  }
  return false;
}

async function cookieHeader(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    return cookies.map(c => c.name + '=' + c.value).join('; ');
  } catch (e) {
    return '';
  }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
function filenameFromUrl(u) {
  try {
    const path = new URL(u).pathname;
    const base = path.substring(path.lastIndexOf('/') + 1);
    return decodeURIComponent(base) || 'download.bin';
  } catch (e) {
    return 'download.bin';
  }
}

function baseName(p) {
  if (!p) return '';
  p = String(p).replace(/\\/g, '/');
  return p.substring(p.lastIndexOf('/') + 1);
}

function notify(title, message) {
  try {
    chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title,
      message
    });
  } catch (e) { /* notifications permission missing or suppressed */ }
}

// ===========================================================================
// MEDIA SNIFFER (Phase 1)
// ---------------------------------------------------------------------------
// Observes every response via webRequest (non-blocking — the only mode MV3
// allows) and classifies media by extension + MIME. Segments (.ts/.m4s,
// video/mp2t) are HLS/DASH noise and skipped; only whole files and manifests
// are listed. HLS/DASH/YouTube entries are DETECTED (shown disabled in the
// panel as "coming soon") but only progressive video/audio is handed to the app —
// stream engines land in Phase 2/3.
// ===========================================================================

const MEDIA_PREFIX = 'media_';          // storage.session key prefix per tab
const MAX_MEDIA_PER_TAB = 100;

const VIDEO_EXT = new Set(
  ['mp4', 'webm', 'mkv', 'flv', 'avi', 'mov', 'm4v', '3gp', 'mpg', 'mpeg', 'wmv']);
const AUDIO_EXT = new Set(
  ['mp3', 'm4a', 'aac', 'ogg', 'oga', 'opus', 'wav', 'flac', 'wma']);
const SKIP_EXT = new Set(
  ['ts', 'm2ts', 'm4s', 'cmfv', 'cmfa', 'fmp4']);   // HLS/DASH segment noise

// tabId -> Map(dedupeKey -> MediaItem). chrome.storage.session is the source
// of truth across service-worker restarts; this is the hot cache.
const mediaCache = new Map();
const persistTimers = new Map();
const notifyTimers = new Map();
// tabId -> Map(stem -> 'video'|'audio'): track names harvested from enriched
// HLS masters, used to label raw CMAF track files (see enrichHls).
const trackStems = new Map();
// tabId -> Set(origin+dir of enriched masters): CMAF CDNs (v.redd.it & co.)
// keep ALL track files beside the master — same-directory progressive media
// is a track even when the master doesn't list its stem (preview streams).
const trackDirs = new Map();

// Origin + path up to the last '/' ("https://v.redd.it/abc/").
function urlDir(u) {
  try {
    const x = new URL(u);
    return x.origin + x.pathname.substring(0, x.pathname.lastIndexOf('/') + 1);
  } catch (e) {
    return '';
  }
}

// 'video' | 'audio' | undefined for a progressive item, per the tab's masters.
function trackTypeOf(tabId, url) {
  const stems = trackStems.get(tabId);
  const stem = urlStem(url);
  const byStem = stems && stems.get(stem);
  if (byStem) return byStem;
  const dirs = trackDirs.get(tabId);
  if (dirs && dirs.has(urlDir(url)))
    return /audio/i.test(stem) ? 'audio' : 'video';
  return undefined;
}

function extOfUrl(u) {
  try {
    const p = new URL(u).pathname;
    const i = p.lastIndexOf('.');
    if (i < 0) return '';
    return p.slice(i + 1).toLowerCase().replace(/[^a-z0-9].*$/, '');
  } catch (e) {
    return '';
  }
}

// Basename without extension ("…/CMAF_720.mp4" and "CMAF_720.m3u8" share the
// stem "CMAF_720" — that is how a track file is tied to its master).
function urlStem(u) {
  try {
    const p = new URL(u, 'https://x/').pathname;
    const base = p.substring(p.lastIndexOf('/') + 1);
    const dot = base.lastIndexOf('.');
    return dot > 0 ? base.slice(0, dot) : base;
  } catch (e) {
    return '';
  }
}

function headerValue(headers, name) {
  if (!headers) return '';
  const n = name.toLowerCase();
  for (const h of headers) {
    if (h.name && h.name.toLowerCase() === n) return h.value || '';
  }
  return '';
}

// Meta's CDNs stream DASH as ordinary .mp4 files: one file per quality rung
// plus a separate audio-only one, fetched with ?bytestart=&byteend= instead of
// Range headers and served as video/mp4. None of the generic rules below can
// tell those apart from a whole file, so one Instagram reel showed up as ten
// "MP4"s and downloading any of them produced a silent, fragmented VP9 file
// that players reject (verified: ftyp brands `dash vp09 cmfc`, one video trak,
// zero audio traks).
//
// The `efg` param carries base64 JSON describing the rung:
//   vencode_tag   "...dash_r2evevp9-..._q90" (video) | "...dash_..._audio"
//   xpv_asset_id  shared by every rung of the SAME video — the key that pairs
//                 a video rung with its audio track.
//
// Host-gated on purpose: no URL outside Meta's CDNs can reach this path, so
// HLS/CMAF handling for Reddit and streaming sites is untouched. Progressive
// files on the same CDNs (no range params, no dash_ tag) still classify as
// plain video below.
function fbDashInfo(url) {
  try {
    const u = new URL(url);
    if (!/(^|\.)(fbcdn\.net|cdninstagram\.com)$/.test(u.hostname)) return null;
    let tag = '', assetId = '', bitrate = 0, dur = 0;
    const efg = u.searchParams.get('efg');
    if (efg) {
      const j = JSON.parse(atob(efg));
      tag = String(j.vencode_tag || '');
      assetId = String(j.xpv_asset_id || '');
      bitrate = Number(j.bitrate) || 0;
      dur = Number(j.duration_s) || 0;
    }
    const ranged = u.searchParams.has('bytestart') || u.searchParams.has('byteend');
    if (!ranged && !/(^|\.)dash_/.test(tag)) return null;   // whole file
    // The player asks for slices via ?bytestart=&byteend=; without them the
    // same URL serves the whole representation, which is what we download.
    u.searchParams.delete('bytestart');
    u.searchParams.delete('byteend');
    return {
      assetId, tag,
      audio: /audio/.test(tag),
      bitrate: bitrate,
      dur: dur,
      clean: u.toString()
    };
  } catch (e) {
    return null;
  }
}

// Returns {kind, ext} for a media response, or null when not list-worthy.
function classifyMedia(url, mime) {
  mime = (mime || '').toLowerCase();
  const ext = extOfUrl(url);
  let host = '', path = '';
  try {
    const u = new URL(url);
    host = u.hostname;
    path = u.pathname;
  } catch (e) { /* leave empty */ }

  if (ext === 'm3u8' || mime.includes('mpegurl'))
    return { kind: 'hls', ext: ext || 'm3u8' };
  if (ext === 'mpd' || mime.includes('dash+xml'))
    return { kind: 'dash', ext: ext || 'mpd' };
  if (fbDashInfo(url)) return { kind: 'dash', ext: 'mp4' };
  if (host.includes('googlevideo.') && path.startsWith('/videoplayback'))
    return { kind: 'youtube', ext: 'mp4' };
  if (SKIP_EXT.has(ext) || mime === 'video/mp2t') return null;  // segment flood
  if (mime.startsWith('video/') || VIDEO_EXT.has(ext))
    return { kind: 'video', ext: ext || 'mp4' };
  if (mime.startsWith('audio/') || AUDIO_EXT.has(ext))
    return { kind: 'audio', ext: ext || 'mp3' };
  return null;
}

// Dedupe key: YouTube appends ?range= to every chunk — strip volatile params
// so one stream stays one entry (the cleaned URL is also the downloadable one).
function mediaKey(url, kind) {
  if (kind === 'dash') {
    // Collapse every quality rung (and the audio track) of one Meta video into
    // a single entry instead of listing ten of them.
    const info = fbDashInfo(url);
    if (info && info.assetId) return 'fbdash:' + info.assetId;
  }
  if (kind === 'youtube') {
    try {
      const u = new URL(url);
      ['range', 'rn', 'rbuf', 'ump', 'sr'].forEach(p => u.searchParams.delete(p));
      return u.toString();
    } catch (e) { /* fall through */ }
  }
  return url;
}

function addMedia(tabId, item) {
  let m = mediaCache.get(tabId);
  if (!m) {
    m = new Map();
    mediaCache.set(tabId, m);
  }
  const key = mediaKey(item.url, item.kind);
  if (item.kind === 'youtube') item.url = key;   // store the cleaned URL
  const prev = m.get(key);
  // Every rung of a Meta video shares one key, so each sighting folds into the
  // same entry. The raw URL must be read BEFORE mergeMetaDashRung rewrites
  // item.url to the chosen video rung.
  const rungUrl = item.url;
  if (item.kind === 'dash') {
    item.size = 0;              // a rung response is a slice; its length lies
    const changed = mergeMetaDashRung(prev || item, rungUrl);
    if (prev && changed) enrichMetaDash(tabId, prev);
  }
  if (prev) {
    prev.size = Math.max(prev.size || 0, item.size || 0);
    prev.timeStamp = item.timeStamp;
  } else {
    // A CMAF track file of an already-enriched master? Stamp it right away.
    if (item.kind === 'video' || item.kind === 'audio') {
      const t = trackTypeOf(tabId, item.url);
      if (t) item.trackOf = t;
    }
    m.set(key, item);
    if (m.size > MAX_MEDIA_PER_TAB) {
      let oldestKey = null, oldest = Infinity;
      for (const [k, v] of m) {
        if (v.timeStamp < oldest) { oldest = v.timeStamp; oldestKey = k; }
      }
      if (oldestKey) m.delete(oldestKey);
    }
    if (item.kind === 'hls') enrichHls(tabId, item);
    if (item.kind === 'dash') enrichMetaDash(tabId, item);
    updateBadge(tabId);
    notifyTabMedia(tabId);
  }
  persistTabMedia(tabId);
}

// Fold one observed rung into a paired entry: keep the highest-bitrate video
// rung and the audio rung. `src` may be a freshly seen item or an existing
// entry being updated. Returns true when the pairing changed (worth a
// re-measure of the size).
function mergeMetaDashRung(dst, rungUrl) {
  const info = fbDashInfo(rungUrl);
  if (!info) return false;
  let changed = false;
  if (info.audio) {
    if (dst.audioUrl !== info.clean) { dst.audioUrl = info.clean; changed = true; }
  } else if (info.bitrate >= (dst.vbitrate || 0)) {
    if (dst.videoUrl !== info.clean) {
      dst.videoUrl = info.clean;
      dst.vbitrate = info.bitrate;
      changed = true;
    }
  }
  // Clip length, used by the panel to tell WHICH feed video an entry belongs
  // to: a reels page preloads its neighbours, so several unrelated videos are
  // in flight at once and the first one seen is rarely the one on screen.
  if (info.dur) dst.dur = info.dur;
  // Carried to the panel so an entry can be matched against the video element
  // the MSE probe bound to this asset.
  if (info.assetId) dst.asset = info.assetId;
  // The entry downloads as its video rung; the audio rung rides along in the
  // hand-off payload and the desktop app muxes the two.
  if (dst.videoUrl) dst.url = dst.videoUrl;
  dst.paired = !!(dst.videoUrl && dst.audioUrl);
  return changed;
}

// The wire never reveals a rung's real size: every response is a slice, so
// Content-Length is the slice's. Ask the CDN for the whole-file sizes once the
// pair is known, so the panel shows what the download will actually cost.
// url -> Promise<bytes>, so each rung is measured exactly once. Without this
// every ABR step to a better rung re-measured the pair, and a scroll through a
// feed turned into a hundred extra HEAD requests.
const rungSize = new Map();
const MAX_RUNG_SIZES = 200;

function sizeOfRung(u) {
  if (!u) return Promise.resolve(0);
  const hit = rungSize.get(u);
  if (hit) return hit;
  const pending = (async () => {
    try {
      const r = await fetch(u, { method: 'HEAD', cache: 'no-store' });
      return r.ok ? Number(r.headers.get('content-length')) || 0 : 0;
    } catch (e) {
      rungSize.delete(u);       // transient failure: allow a later retry
      return 0;
    }
  })();
  if (rungSize.size >= MAX_RUNG_SIZES) rungSize.delete(rungSize.keys().next().value);
  rungSize.set(u, pending);
  return pending;
}

async function enrichMetaDash(tabId, item) {
  if (!item.paired || item.sizing) return;
  item.sizing = true;
  const [v, a] = await Promise.all([sizeOfRung(item.videoUrl), sizeOfRung(item.audioUrl)]);
  item.sizing = false;
  if (!v) return;
  item.size = v + a;
  persistTabMedia(tabId);
  notifyTabMedia(tabId);
}

// ---------------------------------------------------------------------------
// HLS enrichment: a bare m3u8 entry is unusable UX ("HLS · 447 B" is the
// PLAYLIST's size, not the video's!). Fetch the manifest text (tiny; host
// permission <all_urls> bypasses CORS) and derive:
//   master : has #EXT-X-STREAM-INF — the only entry worth offering
//   res    : best variant's height ("720p") for a human label
//   stems  : variant/audio-rendition file stems, so the raw CMAF track
//            files (silent video / audio-only) can be labelled honestly.
// ---------------------------------------------------------------------------
async function enrichHls(tabId, item) {
  if (item.enriched) return;
  item.enriched = true;                 // single attempt per entry
  let body = '';
  try {
    const r = await fetch(item.url, { credentials: 'include' });
    if (!r.ok) return;
    body = await r.text();
  } catch (e) { return; }

  item.master = body.includes('#EXT-X-STREAM-INF');
  if (item.master) {
    let stems = trackStems.get(tabId);
    if (!stems) { stems = new Map(); trackStems.set(tabId, stems); }
    let dirs = trackDirs.get(tabId);
    if (!dirs) { dirs = new Set(); trackDirs.set(tabId, dirs); }
    const dir = urlDir(item.url);
    if (dir) dirs.add(dir);
    let bestBw = -1, bestH = 0;
    const lines = body.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
      const ln = lines[i].trim();
      if (ln.startsWith('#EXT-X-STREAM-INF:')) {
        const bw = +(ln.match(/BANDWIDTH=(\d+)/) || [])[1] || 0;
        // "p" naming uses the SMALLER dimension: a portrait 1080x1920 video
        // is 1080p, not 1920p (Reddit is full of vertical videos).
        const rm = ln.match(/RESOLUTION=(\d+)x(\d+)/);
        const h = rm ? Math.min(+rm[1], +rm[2]) : 0;
        if (bw > bestBw) { bestBw = bw; bestH = h; }
        for (let j = i + 1; j < lines.length; j++) {   // URI line follows
          const u = lines[j].trim();
          if (!u) continue;
          if (u[0] !== '#') stems.set(urlStem(u), 'video');
          break;
        }
      } else if (ln.startsWith('#EXT-X-MEDIA:') && ln.includes('TYPE=AUDIO')) {
        const mu = ln.match(/URI="([^"]+)"/);
        if (mu) stems.set(urlStem(mu[1]), 'audio');
      }
    }
    if (bestH) item.res = bestH + 'p';
    markTracks(tabId);
  }
  persistTabMedia(tabId);
  notifyTabMedia(tabId);
}

// Stamp already-listed progressive entries that are really CMAF track files
// of a known master (they download fine but are silent / audio-only).
function markTracks(tabId) {
  const m = mediaCache.get(tabId);
  if (!m) return;
  for (const it of m.values()) {
    if (it.kind !== 'video' && it.kind !== 'audio') continue;
    const t = trackTypeOf(tabId, it.url);
    if (t) it.trackOf = t;
  }
}

// Debounced write-through to session storage (survives SW restarts).
function persistTabMedia(tabId) {
  clearTimeout(persistTimers.get(tabId));
  persistTimers.set(tabId, setTimeout(() => {
    persistTimers.delete(tabId);
    const m = mediaCache.get(tabId);
    const items = m ? [...m.values()] : [];
    try {
      chrome.storage.session.set({ [MEDIA_PREFIX + tabId]: items });
    } catch (e) { /* storage.session unavailable (old Chrome) */ }
  }, 300));
}

async function getTabMedia(tabId) {
  if (mediaCache.has(tabId)) return [...mediaCache.get(tabId).values()];
  try {
    const data = await chrome.storage.session.get({ [MEDIA_PREFIX + tabId]: [] });
    const items = data[MEDIA_PREFIX + tabId] || [];
    const m = new Map();
    for (const it of items) m.set(mediaKey(it.url, it.kind), it);
    mediaCache.set(tabId, m);
    return items;
  } catch (e) {
    return [];
  }
}

function clearTabMedia(tabId) {
  mediaCache.delete(tabId);
  trackStems.delete(tabId);
  trackDirs.delete(tabId);
  try { chrome.storage.session.remove(MEDIA_PREFIX + tabId); } catch (e) {}
  updateBadge(tabId);
}

chrome.tabs.onRemoved.addListener(clearTabMedia);

function updateBadge(tabId) {
  const m = mediaCache.get(tabId);
  const n = m ? m.size : 0;
  try {
    const p1 = chrome.action.setBadgeBackgroundColor({ tabId, color: '#6a5cff' });
    if (p1 && p1.catch) p1.catch(() => {});
    const p2 = chrome.action.setBadgeText({ tabId, text: n ? String(n) : '' });
    if (p2 && p2.catch) p2.catch(() => {});
  } catch (e) { /* tab already gone */ }
}

// Tell content scripts a new stream was found (throttled; panels re-query).
function notifyTabMedia(tabId) {
  if (notifyTimers.has(tabId)) return;
  notifyTimers.set(tabId, setTimeout(() => {
    notifyTimers.delete(tabId);
    try {
      chrome.tabs.sendMessage(tabId, { t: 'ODM_MEDIA_REFRESH' },
        () => chrome.runtime.lastError);
    } catch (e) { /* no content script in this tab */ }
  }, 800));
}

// --- The observer ----------------------------------------------------------
chrome.webRequest.onHeadersReceived.addListener(
  details => {
    if (details.tabId < 0) return;          // not a real tab (worker/prerender)
    if (details.type === 'main_frame') {
      // New page -> old findings are stale. The request itself may still be
      // a media document (user opened an .mp4 link directly), so keep going.
      clearTabMedia(details.tabId);
    }
    const mime = headerValue(details.responseHeaders, 'content-type');
    const cls = classifyMedia(details.url, mime);
    if (!cls) return;

    // Total size: Content-Range ("bytes 0-524287/10485760") beats Length.
    // Manifests get NO size: a playlist's own byte count ("447 B") reads
    // like a video size in the UI and only confuses.
    let size = 0;
    if (cls.kind !== 'hls' && cls.kind !== 'dash') {
      const cr = headerValue(details.responseHeaders, 'content-range');
      const rm = /\/(\d+)\s*$/.exec(cr);
      if (rm) size = parseInt(rm[1], 10) || 0;
      if (!size) {
        size = parseInt(headerValue(details.responseHeaders, 'content-length'), 10) || 0;
      }
    }

    addMedia(details.tabId, {
      url: details.url,
      kind: cls.kind,
      ext: cls.ext,
      mime: mime || '',
      size,
      frameId: details.frameId,
      timeStamp: Date.now()
    });
  },
  {
    urls: ['<all_urls>'],
    // media          : <video>/<audio> element fetches
    // xmlhttprequest : hls.js/dash.js players fetch manifests via XHR/fetch
    // other          : fetch()-based players, site service workers
    // main_frame/sub_frame: direct media documents (tab/iframe pointing at a file)
    types: ['media', 'xmlhttprequest', 'other', 'main_frame', 'sub_frame']
  },
  ['responseHeaders']
);

// --- Content-script / popup messaging --------------------------------------
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || typeof msg !== 'object') return undefined;

  // SPA navigation (no main_frame request): the top-frame content script
  // saw location.href change — everything sniffed so far is stale.
  if (msg.t === 'ODM_MEDIA_SPA_NAV') {
    const tabId = sender.tab && sender.tab.id;
    if (tabId != null && sender.frameId === 0) clearTabMedia(tabId);
    sendResponse({ ok: true });
    return undefined;
  }

  if (msg.t === 'ODM_MEDIA_QUERY') {
    const tabId = sender.tab && sender.tab.id;
    if (tabId == null) { sendResponse({ items: [] }); return undefined; }
    const frameId = sender.frameId;
    getTabMedia(tabId).then(items => {
      // Prefer streams from the panel's own frame (iframe players); fall back
      // to everything seen in the tab (embeds fetch from sibling frames).
      const local = items.filter(i => i.frameId === frameId);
      sendResponse({ items: local.length ? local : items });
    });
    return true;   // async sendResponse
  }

  if (msg.t === 'ODM_MEDIA_QUERY_TAB') {       // popup asks for the active tab
    const tabId = Number(msg.tabId);
    if (!Number.isFinite(tabId)) { sendResponse({ items: [] }); return undefined; }
    getTabMedia(tabId).then(items => sendResponse({ items }));
    return true;
  }

  if (msg.t === 'ODM_MEDIA_DOWNLOAD') {
    handleMediaDownload(msg, sender).then(ok => sendResponse({ ok }));
    return true;
  }

  return undefined;
});

async function handleMediaDownload(msg, sender) {
  const url = String(msg.url || '');
  if (!/^https?:\/\//i.test(url)) return false;
  const pageUrl = sender.url || (sender.tab && sender.tab.url) || '';
  const kind = String(msg.kind || '');
  const payload = {
    url,
    filename: suggestMediaFilename(msg.title, url, msg.ext, kind),
    referrer: pageUrl
  };
  // Stream types route to a different engine in the desktop app.
  if (kind === 'hls') payload.type = 'hls';
  // Paired-track DASH: the app downloads both rungs and muxes them.
  if (/^https?:\/\//i.test(String(msg.audioUrl || ''))) {
    payload.type = 'dash';
    payload.audioUrl = String(msg.audioUrl);
  } else if (fbDashInfo(url)) {
    // A Meta DASH rung on its own is a silent, fragmented file — exactly the
    // thing the sniffer stopped offering. Reachable via the context menu
    // ("Download with ODM" on a video), which knows nothing about rung pairing,
    // so refuse instead of handing over a broken download.
    notify('ODM',
           'This Instagram/Facebook video streams audio and video separately. ' +
           'Use the "Download" button on the video.');
    return false;
  }
  const ok = await sendToOdm(payload);
  if (!ok) {
    notify('ODM is not running',
           'Could not hand off the video. Please start the ODM app first.');
  }
  return ok;
}

// Page title is the friendliest file name ("Big Buck Bunny.mp4"); fall back
// to the URL's own basename when the tab has no usable title. For HLS the
// container is decided by the app after parsing the playlist (.ts/.mp4), so
// the name goes extension-less.
function suggestMediaFilename(title, url, ext, kind) {
  let base = String(title || '')
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  if (base.length > 120) base = base.slice(0, 120).trim();
  if (!base) return filenameFromUrl(url);
  if (kind === 'hls')
    return base.replace(/\.(m3u8|mp4|ts|mkv|webm|mov)$/i, '');
  let e = String(ext || extOfUrl(url) || '').toLowerCase();
  if (!e) e = 'mp4';
  if (base.toLowerCase().endsWith('.' + e)) return base;
  return base + '.' + e;
}
