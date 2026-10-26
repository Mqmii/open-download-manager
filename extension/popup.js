'use strict';
/** ODM Bridge popup: connection status + capture toggle. */

const PING_URL = 'http://127.0.0.1:47923/ping';

const dot            = document.getElementById('status-dot');
const text           = document.getElementById('status-text');
const toggle         = document.getElementById('capture-toggle');
const videoToggle    = document.getElementById('video-toggle');
const captureDesc    = document.getElementById('capture-desc');
const videoDesc      = document.getElementById('video-desc');
const scopeSiteBtn   = document.getElementById('scope-site');
const scopeSiteLabel = document.getElementById('scope-site-label');
const scopeGlobalBtn = document.getElementById('scope-global');

// The extension's own version, from the manifest. It used to be typed into the
// header by hand, which meant the popup showed a number that had been wrong for
// several releases. The app's version is a different number and is reported
// separately by refreshStatus().
document.getElementById('ext-version').textContent =
  'v' + chrome.runtime.getManifest().version;

let currentSite = '';
let scope = 'site'; // 'site' | 'global'
let state = {
  captureEnabled: true,
  videoPanelEnabled: true,
  disabledCaptureSites: [],
  disabledVideoSites: []
};

function siteHost(urlOrHost) {
  if (!urlOrHost) return '';
  const s = String(urlOrHost).trim();
  try {
    if (/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) {
      const u = new URL(s);
      if (u.protocol !== 'http:' && u.protocol !== 'https:') return '';
      return u.hostname.toLowerCase().replace(/^www\./, '').replace(/\.$/, '');
    }
  } catch (e) {
    return '';
  }
  return s.toLowerCase().replace(/^www\./, '').replace(/\.$/, '');
}

function isSiteDisabled(host, list) {
  if (!host || !Array.isArray(list)) return false;
  for (const raw of list) {
    const d = siteHost(raw);
    if (d && (host === d || host.endsWith('.' + d))) return true;
  }
  return false;
}

function setSiteDisabled(host, list, disabled) {
  const clean = (Array.isArray(list) ? list : [])
    .map(siteHost)
    .filter(d => d && !(host === d || host.endsWith('.' + d)));
  if (disabled && host) clean.push(host);
  return clean;
}

async function refreshStatus() {
  try {
    const r = await fetch(PING_URL, { cache: 'no-store' });
    if (!r.ok) throw new Error('http ' + r.status);
    const j = await r.json();
    dot.className = 'dot on';
    text.textContent = 'ODM connected (v' + (j.version || '?') + ')';
  } catch (e) {
    dot.className = 'dot off';
    text.textContent = 'ODM is not running';
  }
}

function renderToggles() {
  const hasSite = !!currentSite;
  if (!hasSite) scope = 'global';

  scopeSiteBtn.disabled = !hasSite;
  scopeSiteLabel.textContent = hasSite ? currentSite : 'No site';
  scopeSiteBtn.classList.toggle('active', scope === 'site');
  scopeGlobalBtn.classList.toggle('active', scope === 'global');

  if (scope === 'site' && hasSite) {
    const capSiteOff = isSiteDisabled(currentSite, state.disabledCaptureSites);
    const vidSiteOff = isSiteDisabled(currentSite, state.disabledVideoSites);
    const capOn = !!state.captureEnabled && !capSiteOff;
    const vidOn = !!state.videoPanelEnabled && !vidSiteOff;

    toggle.checked = capOn;
    videoToggle.checked = vidOn;

    if (!state.captureEnabled) {
      captureDesc.textContent = 'Disabled globally (All sites).';
    } else if (capSiteOff) {
      captureDesc.textContent = 'Off on ' + currentSite + ' · active on other sites.';
    } else {
      captureDesc.textContent = 'Browser downloads on ' + currentSite + ' go to ODM.';
    }

    if (!state.videoPanelEnabled) {
      videoDesc.textContent = 'Disabled globally (All sites).';
    } else if (vidSiteOff) {
      videoDesc.textContent = 'Off on ' + currentSite + ' · active on other sites.';
    } else {
      videoDesc.textContent = 'Shows a Download button over videos on ' + currentSite + '.';
    }
  } else {
    toggle.checked = !!state.captureEnabled;
    videoToggle.checked = !!state.videoPanelEnabled;
    const nCap = (state.disabledCaptureSites || []).length;
    const nVid = (state.disabledVideoSites || []).length;
    captureDesc.textContent = nCap
      ? 'Browser downloads go to ODM (' + nCap + ' site' + (nCap > 1 ? 's' : '') + ' excluded).'
      : 'Browser downloads are handed to ODM instead.';
    videoDesc.textContent = nVid
      ? 'Shows Download button on videos (' + nVid + ' site' + (nVid > 1 ? 's' : '') + ' excluded).'
      : 'Shows a Download button over videos you hover.';
  }
}

async function initPopup() {
  let tab = null;
  try {
    const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
    tab = tabs && tabs[0];
  } catch (e) {}

  currentSite = tab && tab.url ? siteHost(tab.url) : '';
  scope = currentSite ? 'site' : 'global';

  const data = await chrome.storage.local.get({
    captureEnabled: true,
    videoPanelEnabled: true,
    disabledCaptureSites: [],
    disabledVideoSites: []
  });
  state = {
    captureEnabled: !!data.captureEnabled,
    videoPanelEnabled: !!data.videoPanelEnabled,
    disabledCaptureSites: Array.isArray(data.disabledCaptureSites) ? data.disabledCaptureSites : [],
    disabledVideoSites: Array.isArray(data.disabledVideoSites) ? data.disabledVideoSites : []
  };
  renderToggles();
  refreshMediaCount(tab);
}

scopeSiteBtn.addEventListener('click', () => {
  if (!currentSite) return;
  scope = 'site';
  renderToggles();
});

scopeGlobalBtn.addEventListener('click', () => {
  scope = 'global';
  renderToggles();
});

toggle.addEventListener('change', () => {
  if (scope === 'site' && currentSite) {
    const wantOn = toggle.checked;
    state.disabledCaptureSites = setSiteDisabled(currentSite, state.disabledCaptureSites, !wantOn);
    const patch = { disabledCaptureSites: state.disabledCaptureSites };
    if (wantOn && !state.captureEnabled) {
      state.captureEnabled = true;
      patch.captureEnabled = true;
    }
    chrome.storage.local.set(patch);
  } else {
    state.captureEnabled = toggle.checked;
    chrome.storage.local.set({ captureEnabled: state.captureEnabled });
  }
  renderToggles();
});

videoToggle.addEventListener('change', () => {
  if (scope === 'site' && currentSite) {
    const wantOn = videoToggle.checked;
    state.disabledVideoSites = setSiteDisabled(currentSite, state.disabledVideoSites, !wantOn);
    const patch = { disabledVideoSites: state.disabledVideoSites };
    if (wantOn && !state.videoPanelEnabled) {
      state.videoPanelEnabled = true;
      patch.videoPanelEnabled = true;
    }
    chrome.storage.local.set(patch);
  } else {
    state.videoPanelEnabled = videoToggle.checked;
    chrome.storage.local.set({ videoPanelEnabled: state.videoPanelEnabled });
  }
  renderToggles();
});

// How many streams did the sniffer find on the active tab?
async function refreshMediaCount(tabArg) {
  const el = document.getElementById('media-count');
  try {
    let tab = tabArg;
    if (!tab) {
      const [t] = await chrome.tabs.query({ active: true, currentWindow: true });
      tab = t;
    }
    if (!tab || !/^https?:/.test(tab.url || '')) {
      el.textContent = 'No media on this page';
      return;
    }
    const resp = await chrome.runtime.sendMessage({
      t: 'ODM_MEDIA_QUERY_TAB', tabId: tab.id
    });
    const n = resp && resp.items ? resp.items.length : 0;
    el.textContent = n
      ? n + ' media found on this page'
      : 'No media found on this page';
  } catch (e) {
    el.textContent = 'Could not get media info';
  }
}

refreshStatus();
initPopup();
