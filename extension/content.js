'use strict';
/**
 * ODM — Floating Video Panel (content script, injected into every frame)
 *
 * ODM-style behaviour: hover a <video> (or press play) and a small dark
 * "Download" pill appears at its top-right corner. The stream URL comes from
 * the background sniffer (webRequest) — this script never talks to the
 * desktop bridge directly (the background relays for us).
 *
 * The panel lives in a Shadow DOM tree so the site's CSS cannot break it.
 *
 * HARD-LEARNED FIXES (do not regress):
 *  - NEVER put `all: initial !important` on the host: the browser expands
 *    `all` into `left/top: initial !important` longhands, which then beat
 *    any non-!important left/top we assign later -> panel never positions.
 *    Set left/top with setProperty(..., 'important') instead.
 *  - The panel must STAY visible (loading state) while the sniffer has not
 *    captured a stream yet — HLS manifests only appear once playback
 *    starts; hiding on an empty list made the panel vanish on real sites.
 *  - Reddit (and other web-component players) bury the <video> inside open
 *    Shadow DOM trees: document.querySelectorAll('video') finds NOTHING and
 *    media events are composed:false so 'play' never reaches the document,
 *    even in the capture phase. Discovery must pierce open shadow roots and
 *    'play' must be hooked directly on each discovered video.
 */
(function () {
  if (window.__odmPanelLoaded) return;
  window.__odmPanelLoaded = true;

  const MIN_W = 200, MIN_H = 120;          // ignore tiny/tracker videos
  const DL_KINDS = new Set(['video', 'audio', 'hls']);
  // Single source of truth for "can this entry be handed to the app". A Meta
  // DASH entry qualifies once both rungs are known: the app downloads video +
  // audio and muxes them. Used by the main button, its click handler AND the
  // list rows — they disagreed once, which left the button dead while the
  // entry was perfectly downloadable.
  const canDownload = it => !!it && (DL_KINDS.has(it.kind) ||
                                     (it.kind === 'dash' && it.paired));
  const KIND_TAGS = { dash: 'DASH · coming soon', youtube: 'YouTube · Phase 3' };
  // Nicer display names than raw extensions for stream types.
  const KIND_LABEL = { hls: 'HLS', dash: 'DASH', youtube: 'YT' };

  // blob: url -> Set(xpv_asset_id) reported by mse_probe.js. This is what
  // makes "which video did the user click" a fact instead of a guess: the
  // probe watches the player feed bytes into each <video>'s MediaSource and
  // tells us where those bytes came from.
  const blobAssets = new Map();

  // The asset id lives in the base64 `efg` param Meta puts on every rung URL.
  function assetOfUrl(u) {
    try {
      const efg = new URL(u).searchParams.get('efg');
      if (!efg) return '';
      return String(JSON.parse(atob(efg)).xpv_asset_id || '');
    } catch (e) { return ''; }
  }

  function bindAsset(blob, url) {
    const id = assetOfUrl(url);
    if (!blob || !id) return false;
    let set = blobAssets.get(blob);
    if (!set) { set = new Set(); blobAssets.set(blob, set); }
    if (set.has(id)) return false;
    set.add(id);
    return true;
  }

  window.addEventListener('message', (e) => {
    if (e.source !== window) return;
    const d = e.data;
    if (!d) return;
    let changed = false;
    if (d.source === 'odm-mse') {
      changed = bindAsset(d.blob, d.url);
    } else if (d.source === 'odm-mse-snapshot' && Array.isArray(d.data)) {
      // Answer to askProbe(): bindings made before this script attached.
      for (const [blob, urls] of d.data)
        for (const u of urls) changed = bindAsset(blob, u) || changed;
    } else {
      return;
    }
    // The panel can now resolve the hovered video exactly — re-render it.
    if (changed && visible) queryMedia().then(items => { if (visible) render(items); });
  });

  // The probe starts at document_start, this script at document_idle, so the
  // clip playing on arrival was bound before anyone was listening. Ask.
  function askProbe() {
    try { window.postMessage({ source: 'odm-mse-req' }, location.origin); } catch (e) {}
  }
  askProbe();

  // Driven by the popup's "Capture videos" switch (videoPanelEnabled).
  let captureEnabled = true;
  let host = null, shadow = null;
  let btnEl = null, listEl = null, barEl = null, lblEl = null;
  let activeVideo = null;
  let itemsCache = [];
  let visible = false;
  let hideTimer = 0;
  let feedbackTimer = 0;
  let lastMoveCheck = 0;
  let repositionTimer = 0;

  // ---- settings (same toggle as the popup) --------------------------------
  try {
    chrome.storage.local.get({ videoPanelEnabled: true }, d => {
      captureEnabled = !!d.videoPanelEnabled;
    });
    chrome.storage.onChanged.addListener((changes, area) => {
      if (area === 'local' && changes.videoPanelEnabled) {
        captureEnabled = !!changes.videoPanelEnabled.newValue;
        if (!captureEnabled) hide();
      }
    });
  } catch (e) { /* extension context invalidated after reload */ }

  // ---- video discovery ------------------------------------------------------
  // querySelectorAll does NOT pierce shadow roots (Reddit's <shreddit-player>
  // nests its <video> two shadow trees deep). Walk open shadow roots too, but
  // cache the result — a full-tree walk on every 150ms mousemove would hurt.
  const SCAN_TTL = 1000;
  let scanCache = [];
  let scanAt = 0;

  function collectVideos(root, out) {
    root.querySelectorAll('video').forEach(v => out.push(v));
    root.querySelectorAll('*').forEach(el => {
      if (el.shadowRoot) collectVideos(el.shadowRoot, out);
    });
  }

  function allVideos() {
    const now = Date.now();
    if (now - scanAt < SCAN_TTL) return scanCache;
    scanAt = now;
    const out = [];
    try { collectVideos(document, out); } catch (e) { /* detached roots */ }
    scanCache = out;
    hookPlay(out);
    return out;
  }

  function videos() {
    const out = [];
    for (const v of allVideos()) {
      const r = v.getBoundingClientRect();
      if (r.width >= MIN_W && r.height >= MIN_H) out.push({ v, r });
    }
    return out;
  }

  // Media events are composed:false — a shadow video's 'play' never reaches
  // the document listener below. Hook each discovered video directly.
  const hooked = new WeakSet();
  function hookPlay(vids) {
    for (const v of vids) {
      if (hooked.has(v)) continue;
      hooked.add(v);
      v.addEventListener('play', () => {
        if (!captureEnabled) return;
        const r = v.getBoundingClientRect();
        if (r.width >= MIN_W && r.height >= MIN_H) showFor(v);
      });
    }
  }

  function videoAtPoint(x, y) {
    for (const { v, r } of videos()) {
      if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return v;
    }
    return null;
  }

  // ---- panel construction (Shadow DOM => site CSS can't touch it) ----------
  function ensurePanel() {
    if (host && host.isConnected) return;
    host = document.createElement('div');
    host.setAttribute('data-odm-panel', '');
    // NOTE: no `all: initial` here — see the header comment.
    host.style.cssText = 'position:fixed !important; z-index:2147483647 !important; display:none;';
    shadow = host.attachShadow({ mode: 'open' });
    shadow.innerHTML =
      '<style>' +
      '*{box-sizing:border-box;margin:0;padding:0;' +
      'font-family:"Segoe UI",Inter,Arial,sans-serif;}' +
      '.wrap{position:relative;font-size:12px;}' +
      '.bar{display:flex;border-radius:8px;overflow:hidden;' +
      'box-shadow:0 4px 18px rgba(0,0,0,.45);}' +
      'button{border:0;cursor:pointer;font-size:12px;font-weight:600;' +
      'color:#fff;background:#1c1c26;padding:7px 11px;display:flex;' +
      'align-items:center;gap:6px;transition:background .15s;white-space:nowrap;}' +
      'button:hover:not(:disabled){background:#6a5cff;}' +
      'button:disabled{opacity:.55;cursor:default;}' +
      '.main{border-radius:8px 0 0 8px;}' +
      '.caret{border-radius:0 8px 8px 0;border-left:1px solid #33333f;' +
      'padding:7px 8px;}' +
      '.caret:hover{background:#5a4ce0;}' +
      '.single .main{border-radius:8px;}' +
      '.single .caret{display:none;}' +
      '.ico{font-size:13px;line-height:1;}' +
      '.list{position:absolute;right:0;top:calc(100% + 6px);min-width:230px;' +
      'max-width:340px;max-height:300px;overflow-y:auto;background:#18181f;' +
      'border:1px solid #2c2c38;border-radius:10px;' +
      'box-shadow:0 8px 28px rgba(0,0,0,.55);}' +
      '.list.up{top:auto;bottom:calc(100% + 6px);}' +
      '.list.lft{right:auto;left:0;}' +
      '.row{display:flex;align-items:center;gap:8px;padding:8px 11px;' +
      'cursor:pointer;}' +
      '.row:hover:not(.off){background:#262633;}' +
      '.row .ext{font-weight:700;color:#8f83ff;min-width:36px;' +
      'text-transform:uppercase;}' +
      '.row .meta{flex:1;color:#c9c9d6;overflow:hidden;' +
      'text-overflow:ellipsis;white-space:nowrap;}' +
      '.row .tag{font-size:10px;color:#9a9aa8;background:#262633;' +
      'border-radius:4px;padding:2px 6px;}' +
      '.row.off{cursor:default;opacity:.5;}' +
      '.ok{background:#1f6f43 !important;}' +
      '.err{background:#8c2f39 !important;}' +
      '</style>' +
      '<div class="wrap">' +
      '<div class="bar" id="bar">' +
      '<button class="main" id="main" type="button">' +
      '<span class="ico">⬇</span><span id="lbl">Download</span>' +
      '</button>' +
      '<button class="caret" id="caret" type="button" title="Other streams">▾</button>' +
      '</div>' +
      '<div class="list" id="list" hidden></div>' +
      '</div>';
    (document.documentElement || document.body).appendChild(host);

    barEl = shadow.getElementById('bar');
    btnEl = shadow.getElementById('main');
    lblEl = shadow.getElementById('lbl');
    listEl = shadow.getElementById('list');
    const caret = shadow.getElementById('caret');

    btnEl.addEventListener('click', () => {
      const p = itemsCache[0];
      if (canDownload(p)) download(p);
    });
    caret.addEventListener('click', () => {
      listEl.hidden = !listEl.hidden;
      if (!listEl.hidden) positionList();
    });
    barEl.addEventListener('mouseenter', () => clearTimeout(hideTimer));
    listEl.addEventListener('mouseenter', () => clearTimeout(hideTimer));
    barEl.addEventListener('mouseleave', scheduleHide);
    listEl.addEventListener('mouseleave', scheduleHide);
  }

  // ---- show / hide / position ----------------------------------------------
  function showFor(video) {
    if (!captureEnabled || isFullscreenLike()) return;
    activeVideo = video;
    askProbe();                 // pick up bindings made since the last panel
    ensurePanel();
    visible = true;
    host.style.display = 'block';
    reposition();
    queryMedia().then(items => { if (visible) render(items); });
  }

  function hide() {
    visible = false;
    activeVideo = null;
    itemsCache = [];
    if (host) host.style.display = 'none';
  }

  function scheduleHide() {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => {
      // ODM keeps the panel while the video is playing, even if the mouse
      // has wandered off; a paused video loses it after the grace period.
      if (activeVideo && !activeVideo.paused && !activeVideo.ended) return;
      hide();
    }, 1100);
  }


  // True fullscreen (Fullscreen API) or a player's CSS fake-fullscreen (a
  // fixed-position ancestor of the video covering the viewport, e.g. JW
  // Player's jw-flag-fullscreen) must hide the panel — the user is watching.
  function isFullscreenLike() {
    if (document.fullscreenElement) return true;
    // CSS-fake check runs at TOP LEVEL only: inside an iframe a "fake
    // fullscreen" can never exceed the frame (embeds legitimately fill 100%
    // of their frame by design) — there the Fullscreen API is the only signal.
    if (window !== window.top) return false;
    let el = activeVideo;
    while (el && el !== document.body && el !== document.documentElement) {
      if (getComputedStyle(el).position === 'fixed') {
        const r = el.getBoundingClientRect();
        if (r.width >= innerWidth * 0.95 && r.height >= innerHeight * 0.95)
          return true;
      }
      el = el.parentElement;
    }
    return false;
  }

  // IMPORTANT: left/top need !important — player stylesheets (JW & co.) ship
  // !important rules, and plain inline assignments lose against them.
  function reposition() {
    if (!visible || !activeVideo || !host) return;
    const r = activeVideo.getBoundingClientRect();
    if (isFullscreenLike()) { host.style.display = 'none'; return; }
    if (r.width < MIN_W || r.height < MIN_H ||
        r.bottom < 0 || r.right < 0 ||
        r.top > innerHeight || r.left > innerWidth) {
      host.style.display = 'none';
      return;
    }
    host.style.display = 'block';
    const pw = host.offsetWidth || 130;
    const ph = host.offsetHeight || 34;
    // Clamp into the viewport so the pill never slides off-screen.
    const left = Math.max(4, Math.min(r.right - pw - 8, innerWidth - pw - 4));
    const top = Math.max(4, Math.min(r.top + 8, innerHeight - ph - 4));
    host.style.setProperty('left', left + 'px', 'important');
    host.style.setProperty('top', top + 'px', 'important');
    if (listEl && !listEl.hidden) positionList();
  }

  // Keep the dropdown fully inside the viewport: open upwards when the panel
  // hugs the bottom edge, flip to left alignment only when the default
  // right-aligned list would spill off the LEFT edge, and cap its height
  // with an inner scrollbar.
  function positionList() {
    if (!host || !listEl) return;
    const hr = host.getBoundingClientRect();
    const lw = listEl.offsetWidth || 260;
    listEl.classList.toggle('up', hr.bottom + 6 + 300 > innerHeight && hr.top > 310);
    const overflowLeft = hr.right - lw < 4;
    const canFlipRight = hr.left + lw <= innerWidth - 4;
    listEl.classList.toggle('lft', overflowLeft && canFlipRight);
    listEl.style.maxHeight = Math.min(300, innerHeight - 16) + 'px';
  }

  // ---- media list (from the background sniffer) -----------------------------
  // Feed pages (Reddit home & co.) autoplay MANY videos into one tab list.
  // To pin the panel to the HOVERED video, harvest media-URL hints from the
  // element and its (shadow-host) ancestors: players carry the stream URL in
  // attributes (<shreddit-player src="…m3u8" preview="…mp4">). Streams from
  // the same CDN directory belong to the same video.
  function mediaDir(u) {
    try {
      const x = new URL(u);
      return x.origin + x.pathname.slice(0, x.pathname.lastIndexOf('/') + 1);
    } catch (e) { return ''; }
  }

  function mediaDirHints(video) {
    const dirs = new Set();
    const consider = u => {
      if (!/^https?:\/\//i.test(u)) return;
      const path = u.split('?')[0].split('#')[0];
      if (/\.(m3u8|mpd|mp4|webm|mov|m4v|mp3|m4a)$/i.test(path))
        dirs.add(mediaDir(u));
    };
    if (video && /^https?:/i.test(video.currentSrc || ''))
      consider(video.currentSrc);
    let el = video, depth = 0;
    while (el && depth++ < 12) {
      if (el.attributes)
        for (const a of el.attributes) consider(a.value || '');
      el = el.parentElement ||
           (el.getRootNode() instanceof ShadowRoot ? el.getRootNode().host : null);
    }
    return dirs;
  }

  async function queryMedia() {
    let items = [];
    try {
      const resp = await chrome.runtime.sendMessage({ t: 'ODM_MEDIA_QUERY' });
      if (resp && Array.isArray(resp.items)) items = resp.items;
    } catch (e) { /* worker waking up or extension reloaded */ }

    if (!items.length && activeVideo) {
      // Fallback: plain <video src="https://..."> pages the sniffer missed.
      const src = activeVideo.currentSrc || activeVideo.src || '';
      if (/^https?:\/\//i.test(src)) {
        items = [{ url: src, kind: 'video', ext: extOf(src) || 'mp4',
                   size: 0, timeStamp: Date.now() }];
      }
    }
    // Pin the list to the hovered video when the page told us which stream
    // is HIS: keep only items from the same CDN directory. No match = that
    // video simply hasn't streamed yet — better an honest "Searching for
    // stream…" than offering a DIFFERENT feed video's file.
    const dirs = activeVideo ? mediaDirHints(activeVideo) : new Set();
    if (dirs.size && items.length) {
      const own = items.filter(i => dirs.has(mediaDir(i.url)));
      items = own;   // possibly empty -> loading state + refresh on play
    }

    // Pin to the video on screen. mediaDirHints() above cannot do it here:
    // Instagram plays through MSE, so the element's src is a blob: url. The
    // MSE probe binds that blob to the clips whose bytes were fed into it, so
    // this is an identity match, not a heuristic.
    const blob = activeVideo ? (activeVideo.currentSrc || activeVideo.src || '') : '';
    const bound = blob ? blobAssets.get(blob) : null;
    if (bound && bound.size) {
      const dashItems = items.filter(i => i.kind === 'dash' && i.asset);
      if (dashItems.length) {
        const own = dashItems.filter(i => bound.has(i.asset));
        // Nothing of this element's is downloadable yet? Offer nothing rather
        // than a neighbouring clip — the panel stays in its loading state.
        items = items.filter(i => !(i.kind === 'dash' && i.asset) || own.includes(i));
      }
    } else if (activeVideo && isFinite(activeVideo.duration) && activeVideo.duration > 0) {
      // No binding (clip never played, or the probe missed it): fall back to
      // clip length, which is a guess and is treated as one.
      const vdur = activeVideo.duration;
      const near = i => i.dur && Math.abs(i.dur - vdur) <= 1.5;
      if (items.some(near))
        items = items.filter(i => !(i.kind === 'dash' && i.dur) || near(i));
    }

    // One HLS master = the whole video (all qualities + audio). Its variant
    // and audio-rendition playlists are subsets — pure noise, drop them.
    if (items.some(i => i.kind === 'hls' && i.master))
      items = items.filter(i => i.kind !== 'hls' || i.master);

    // HLS master beats progressive files: CMAF sites (Reddit & co.) expose
    // the video and audio tracks as SEPARATE .mp4 range-files — the biggest
    // "video" there is silent. The manifest is the only complete choice.
    // Known track files (trackOf) sink below untagged progressive media.
    const PRIO = { hls: 3, video: 2, audio: 1 };
    const prio = it => (PRIO[it.kind] || 0) - (it.trackOf ? 1.5 : 0);
    itemsCache = items.slice().sort((a, b) =>
      (canDownload(b) - canDownload(a)) ||
      (prio(b) - prio(a)) ||
      ((b.size || 0) - (a.size || 0)) ||
      ((b.timeStamp || 0) - (a.timeStamp || 0)));
    return itemsCache;
  }

  // An empty list is NOT a reason to vanish: on HLS sites the manifest only
  // reaches the sniffer once playback starts. Stay put in a loading state —
  // ODM_MEDIA_REFRESH upgrades us as soon as a stream arrives.
  function render(items) {
    if (!visible || !host) return;
    const p = items[0];
    if (!p) {
      lblEl.textContent = 'Searching for stream…';
      btnEl.disabled = true;
      barEl.classList.add('single');
      listEl.hidden = true;
      reposition();
      return;
    }
    lblEl.textContent = 'Download ' + shortLabel(p);
    btnEl.disabled = !canDownload(p);
    barEl.classList.toggle('single', items.length < 2);
    if (items.length > 1) renderList(items); else listEl.hidden = true;
    reposition();
  }

  function renderList(items) {
    listEl.innerHTML = '';
    for (const it of items) {
      const row = document.createElement('div');
      const dl = canDownload(it);
      row.className = 'row' + (dl ? '' : ' off');
      const ext = document.createElement('span');
      ext.className = 'ext';
      ext.textContent = KIND_LABEL[it.kind] || (it.ext || '?').slice(0, 5);
      const meta = document.createElement('span');
      meta.className = 'meta';
      if (it.kind === 'hls') {
        // Playlist byte size is meaningless; say what the user really gets.
        // "video + audio" only when the manifest was VERIFIED as a master.
        meta.textContent = it.master
          ? (it.res ? it.res + ' · ' : '') + 'video + audio'
          : 'stream';
      } else if (it.kind === 'dash' && it.paired) {
        // Size is the sum of both rungs, measured by HEAD in the worker.
        meta.textContent = (it.size ? fmtSize(it.size) + ' · ' : '') + 'video + audio';
      } else {
        meta.textContent = it.size ? fmtSize(it.size) : (dl ? 'size unknown' : '');
      }
      row.append(ext, meta);
      // CMAF track files download fine but are incomplete — say so.
      let tag = (it.kind === 'dash' && it.paired) ? null : KIND_TAGS[it.kind];
      if (!tag && it.trackOf)
        tag = it.trackOf === 'audio' ? 'audio only' : 'video only (silent)';
      if (tag) {
        const t = document.createElement('span');
        t.className = 'tag';
        t.textContent = tag;
        row.append(t);
      }
      if (dl) row.addEventListener('click', () => download(it));
      listEl.append(row);
    }
  }

  // ---- download hand-off ----------------------------------------------------
  async function download(it) {
    setFeedback(null);
    let ok = false;
    try {
      const resp = await chrome.runtime.sendMessage({
        t: 'ODM_MEDIA_DOWNLOAD',
        url: it.url,
        audioUrl: it.audioUrl || '',   // paired DASH: muxed in by the app
        ext: it.ext,
        kind: it.kind,
        title: document.title || ''
      });
      ok = !!(resp && resp.ok);
    } catch (e) { ok = false; }
    setFeedback(ok ? 'ok' : 'err');
    if (ok) listEl.hidden = true;
  }

  function setFeedback(kind) {
    clearTimeout(feedbackTimer);
    barEl.classList.remove('ok', 'err');
    if (!kind) { lblEl.textContent = 'Download…'; return; }
    if (kind === 'ok') {
      barEl.classList.add('ok');
      lblEl.textContent = '✓ Sent to ODM';
    } else {
      barEl.classList.add('err');
      lblEl.textContent = 'ODM is not running!';
    }
    feedbackTimer = setTimeout(() => { if (visible) render(itemsCache); }, 2200);
  }

  // ---- small helpers --------------------------------------------------------
  function fmtSize(b) {
    if (!b) return '';
    const u = ['B', 'KB', 'MB', 'GB'];
    let i = 0, n = b;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (n >= 100 ? Math.round(n) : n.toFixed(1)) + ' ' + u[i];
  }

  function shortLabel(it) {
    // HLS: never show a byte count — it would be the PLAYLIST's size (447 B),
    // not the video's. Resolution is the honest, useful bit of info.
    if (it.kind === 'hls')
      return '(HLS' + (it.res ? ' · ' + it.res : '') + ')';
    let s = '(' + (KIND_LABEL[it.kind] || (it.ext || 'video').toUpperCase());
    if (it.size) s += ' · ' + fmtSize(it.size);
    return s + ')';
  }

  function extOf(u) {
    try {
      const p = new URL(u).pathname;
      const i = p.lastIndexOf('.');
      return i >= 0 ? p.slice(i + 1).toLowerCase().replace(/[^a-z0-9].*$/, '') : '';
    } catch (e) { return ''; }
  }

  // ---- event wiring -----------------------------------------------------------
  // Pointer tracking by rect (not mouseover on <video>) because most players
  // cover the element with a controls overlay — the video never gets events.
  document.addEventListener('mousemove', e => {
    const now = Date.now();
    if (now - lastMoveCheck < 150) return;
    lastMoveCheck = now;
    if (!captureEnabled) return;
    if (host && e.target === host) return;              // pointer over our panel
    const v = videoAtPoint(e.clientX, e.clientY);
    if (v) {
      if (v !== activeVideo) showFor(v);
      clearTimeout(hideTimer);
    } else if (visible) {
      scheduleHide();
    }
  }, true);

  document.addEventListener('play', e => {
    // Light-DOM videos only — shadow videos are covered by hookPlay().
    if (!captureEnabled) return;
    if (e.target && e.target.tagName === 'VIDEO') {
      const r = e.target.getBoundingClientRect();
      if (r.width >= MIN_W && r.height >= MIN_H) showFor(e.target);
    }
  }, true);

  // scroll doesn't bubble, but capture-phase on document still sees it.
  document.addEventListener('scroll', reposition, { passive: true, capture: true });
  window.addEventListener('resize', reposition, { passive: true });
  document.addEventListener('fullscreenchange', hide);

  // Panel must die with its video (SPA players swap elements constantly).
  new MutationObserver(() => {
    if (activeVideo && !activeVideo.isConnected) hide();
  }).observe(document.documentElement, { childList: true, subtree: true });

  // Players move/resize without firing scroll or resize (theater mode,
  // sticky players...). Poll cheaply while the panel is visible. When it is
  // hidden, keep the shadow scan warm instead so play-hooks land on shadow
  // players even if the mouse never moves (autoplay).
  // The same tick watches for SPA navigations (Reddit & co. never issue a
  // real main_frame request): URL changed -> old streams are stale, tell
  // the sniffer to flush the tab list. Top frame only.
  let lastHref = location.href;
  repositionTimer = setInterval(() => {
    if (visible) reposition(); else allVideos();
    if (window === window.top && location.href !== lastHref) {
      lastHref = location.href;
      hide();
      // The old page's blob: urls are dead; keeping them only grows the map
      // for the life of the tab. The probe still holds its own bindings, so
      // askProbe() below repopulates whatever is still on screen.
      blobAssets.clear();
      askProbe();
      try {
        chrome.runtime.sendMessage({ t: 'ODM_MEDIA_SPA_NAV' },
          () => chrome.runtime.lastError);
      } catch (e) { /* extension context invalidated */ }
    }
  }, 500);

  // Background pushes "new stream found" -> refresh an open panel.
  try {
    chrome.runtime.onMessage.addListener(msg => {
      if (msg && msg.t === 'ODM_MEDIA_REFRESH' && visible) {
        queryMedia().then(items => { if (visible) render(items); });
      }
    });
  } catch (e) { /* extension context invalidated */ }
})();
