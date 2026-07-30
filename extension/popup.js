'use strict';
/** ODM Bridge popup: connection status + capture toggle. */

const PING_URL = 'http://127.0.0.1:47923/ping';

const dot    = document.getElementById('status-dot');
const text   = document.getElementById('status-text');
const toggle = document.getElementById('capture-toggle');
const videoToggle = document.getElementById('video-toggle');

// The extension's own version, from the manifest. It used to be typed into the
// header by hand, which meant the popup showed a number that had been wrong for
// several releases. The app's version is a different number and is reported
// separately by refreshStatus().
document.getElementById('ext-version').textContent =
  'v' + chrome.runtime.getManifest().version;

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

async function refreshToggle() {
  const data = await chrome.storage.local.get({ captureEnabled: true, videoPanelEnabled: true });
  toggle.checked = !!data.captureEnabled;
  videoToggle.checked = !!data.videoPanelEnabled;
}

toggle.addEventListener('change', () => {
  chrome.storage.local.set({ captureEnabled: toggle.checked });
});

videoToggle.addEventListener('change', () => {
  chrome.storage.local.set({ videoPanelEnabled: videoToggle.checked });
});

// How many streams did the sniffer find on the active tab?
async function refreshMediaCount() {
  const el = document.getElementById('media-count');
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
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
refreshToggle();
refreshMediaCount();
