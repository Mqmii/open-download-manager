'use strict';
/**
 * MSE probe — runs in the PAGE's world (manifest `world: "MAIN"`) at
 * document_start, before the site's player exists.
 *
 * Why this file exists
 * -------------------
 * Sniffing the network tells you WHAT media is streaming but never WHICH
 * on-screen video it belongs to. Instagram plays through Media Source
 * Extensions, so every <video> carries a blob: src and a reels/feed page keeps
 * a dozen unrelated clips buffered at once. Picking by size, recency or clip
 * length is guesswork, and guessing wrong hands the user a different video.
 *
 * So instead of guessing, watch the player itself:
 *
 *   URL.createObjectURL(mediaSource)  -> which blob: url is which MediaSource
 *   MediaSource.addSourceBuffer(mime) -> that source's video / audio buffers
 *   SourceBuffer.appendBuffer(bytes)  -> the bytes actually fed to it
 *   fetch()                           -> where those bytes came from
 *
 * Correlating the last two binds a playing <video> to the exact CDN URLs
 * behind it. Measured on a live reels page (17 videos, 38 buffers): the
 * playing clip resolved to 4 video-rung URLs and 6 audio URLs, no ambiguity.
 *
 * The player reads responses as streams rather than ArrayBuffers, so the body
 * is proxied and each chunk tagged two ways: by the chunk buffer's identity,
 * and by the first bytes of the response (the player often concatenates chunks
 * into a fresh buffer, which destroys identity). Roughly a third of appends
 * match, which is far more than needed — one match per buffer is enough to
 * identify the clip.
 *
 * Findings are posted to the content script, which owns all extension APIs;
 * this file touches none.
 */
(() => {
  const TAG = 'odm-mse';
  const isMedia = (u) => /fbcdn\.net|cdninstagram\.com/.test(u || '');
  const MAX_PREFIXES = 300;      // bounded: this runs for the page's lifetime
  const MAX_URLS_PER_BUFFER = 6; // enough to identify; stop bookkeeping after
  const MAX_BLOBS = 64;          // FIFO cap on remembered MediaSources

  // blob: url -> Set(source urls). Kept here because this script starts at
  // document_start while the content script attaches at document_idle: every
  // binding made before that would otherwise be lost, and on a reels page the
  // clip is already playing by then. The content script asks for a snapshot
  // whenever it needs one.
  const bindings = new Map();

  const bufUrl = new WeakMap();  // chunk ArrayBuffer -> url
  // "<total length>:<first 64 bytes hex>" -> url (FIFO capped). 64 bytes, not
  // 16: every Meta init segment opens with the same ftyp box
  // (mp41 iso8 isom dash vp09 cmfc), so a short prefix matched ANY clip and
  // bound neighbouring videos to each other. Byte 0x38 onwards carries the
  // mvhd creation time, which is per-clip, and the length disambiguates the
  // rest. Entries are consumed on match: one response feeds one append.
  const prefixUrl = new Map();
  const msUrl = new WeakMap();   // MediaSource -> blob: url

  const hex = (u8, n) => {
    let s = '';
    const m = Math.min(n, u8.length);
    for (let i = 0; i < m; i++) s += u8[i].toString(16).padStart(2, '0');
    return s;
  };

  const remember = (key, url) => {
    if (prefixUrl.size >= MAX_PREFIXES) {
      const oldest = prefixUrl.keys().next().value;
      prefixUrl.delete(oldest);
    }
    prefixUrl.set(key, url);
  };

  const report = (blob, mime, url) => {
    if (!blob) return;
    let set = bindings.get(blob);
    if (!set) {
      // Bounded: a long feed session creates a MediaSource per clip and none
      // of them are ever revoked by us.
      if (bindings.size >= MAX_BLOBS) bindings.delete(bindings.keys().next().value);
      set = new Set();
      bindings.set(blob, set);
    }
    set.add(url);
    try {
      window.postMessage({ source: TAG, blob, mime, url }, location.origin);
    } catch (e) { /* page may be sandboxed; nothing to do */ }
  };

  // Snapshot protocol: the content script asks, we answer with everything
  // bound so far.
  window.addEventListener('message', (e) => {
    if (e.source !== window || !e.data || e.data.source !== TAG + '-req') return;
    const data = [];
    for (const [blob, set] of bindings) data.push([blob, [...set]]);
    try {
      window.postMessage({ source: TAG + '-snapshot', data }, location.origin);
    } catch (err) { /* nothing to do */ }
  });

  // --- fetch: proxy the body stream and tag the chunks ----------------------
  const origFetch = window.fetch;
  window.fetch = async function (...args) {
    const res = await origFetch.apply(this, args);
    let url = '';
    try {
      url = typeof args[0] === 'string' ? args[0] : (args[0] && args[0].url) || '';
    } catch (e) { /* leave empty */ }
    if (!isMedia(url)) return res;
    try {
      const body = res.body;
      if (!body) return res;
      const reader = body.getReader();
      let head = '', total = 0;
      const proxied = new ReadableStream({
        async pull(ctrl) {
          const { done, value } = await reader.read();
          if (done) {
            // The key is only complete now: the player appends the whole
            // response, so the append's length must equal the total.
            if (head) remember(total + ':' + head, url);
            ctrl.close();
            return;
          }
          try {
            bufUrl.set(value.buffer, url);
            total += value.byteLength;
            if (!head) head = hex(value, 64);
          } catch (e) { /* tagging is best-effort */ }
          ctrl.enqueue(value);
        },
        cancel(reason) { try { reader.cancel(reason); } catch (e) {} }
      });
      Object.defineProperty(res, 'body', { get: () => proxied });
    } catch (e) { /* a failed hook must never break playback */ }
    return res;
  };

  // --- MSE: blob url <-> MediaSource <-> its buffers ------------------------
  const origCreateURL = URL.createObjectURL;
  URL.createObjectURL = function (obj) {
    const u = origCreateURL.call(this, obj);
    try { if (obj instanceof MediaSource) msUrl.set(obj, u); } catch (e) {}
    return u;
  };

  const origAddSB = MediaSource.prototype.addSourceBuffer;
  MediaSource.prototype.addSourceBuffer = function (mime) {
    const sb = origAddSB.call(this, mime);
    try { sb.__odm = { blob: msUrl.get(this) || '', mime: String(mime || ''), urls: [] }; } catch (e) {}
    return sb;
  };

  const origAppend = SourceBuffer.prototype.appendBuffer;
  SourceBuffer.prototype.appendBuffer = function (data) {
    try {
      const rec = this.__odm;
      if (rec && rec.urls.length < MAX_URLS_PER_BUFFER) {
        const u8 = data instanceof ArrayBuffer
          ? new Uint8Array(data)
          : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        let url = bufUrl.get(u8.buffer);
        if (!url) {
          const key = u8.byteLength + ':' + hex(u8, 64);
          url = prefixUrl.get(key);
          if (url) prefixUrl.delete(key);      // one response, one append
        }
        if (url && !rec.urls.includes(url)) {
          rec.urls.push(url);
          // The blob url is only known once createObjectURL ran; re-read it.
          report(rec.blob, rec.mime, url);
        }
      }
    } catch (e) { /* never block the append */ }
    return origAppend.call(this, data);
  };
})();
