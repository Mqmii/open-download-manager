// ODM Clone Frontend Script
// Wires the Fluent UI layout to native C++ functions and implements local storage for download history.

// State variables
let downloads = [];
let selectedId = null;
let activeCategory = 'all';
// Starts requested while another download is running are queued here and
// kicked off one by one from UI.onComplete as the engine frees up. This avoids
// the Stop->Start race where the new job silently never starts.
// A FIFO, not a single slot: it used to be one variable, so queueing a third
// download overwrote the second and that row sat on "Waiting..." forever.
let pendingStarts = [];
// Request context (cookies/referrer/UA/filename) captured by the browser
// extension via UI.onExternalDownload. Consumed by the next startNewDownload()
// and attached to the new row as .ctx. NEVER persisted to localStorage
// (saveData strips it) — it contains session cookies.
let pendingContext = null;
// Throttle localStorage writes during high-frequency progress ticks.
let lastSaveTs = 0;

// DOM Elements
const el = {
  // Toolbar Buttons
  tbAddUrl:   document.getElementById('tb-add-url'),
  tbResume:   document.getElementById('tb-resume'),
  tbStop:     document.getElementById('tb-stop'),
  tbStopAll:  document.getElementById('tb-stop-all'),
  tbDelete:   document.getElementById('tb-delete'),
  tbOptions:  document.getElementById('tb-options'),
  
  // Search and Table
  searchInput:  document.getElementById('search-input'),
  tableBody:    document.getElementById('table-body'),
  selectAll:    document.getElementById('th-select-all'),
  
  // Sidebar
  categoryItems: document.querySelectorAll('.category-item'),
  
  // Status Bar
  statusMsg:    document.getElementById('status-message'),
  dlSpeed:      document.getElementById('stat-dl-speed'),
  ulSpeed:      document.getElementById('stat-ul-speed'),
  
  // Modal Dialog
  modal:        document.getElementById('add-url-modal'),
  modalClose:   document.getElementById('modal-close'),
  modalCancel:  document.getElementById('modal-cancel-btn'),
  urlInput:     document.getElementById('url'),
  pathInput:    document.getElementById('path'),
  pasteBtn:     document.getElementById('paste-btn'),
  browseBtn:    document.getElementById('browse-btn'),
  startBtn:     document.getElementById('start-btn'),
  probeBox:     document.getElementById('url-probe'),
  probeBadge:   document.getElementById('url-probe-badge'),
  probeText:    document.getElementById('url-probe-text'),
  
  // Context Menu
  contextMenu:  document.getElementById('context-menu'),
  
  // Options Modal
  optModal:       document.getElementById('options-modal'),
  optModalClose:  document.getElementById('options-modal-close'),
  optCancelBtn:   document.getElementById('options-cancel-btn'),
  optSaveBtn:     document.getElementById('options-save-btn'),
  optPathInput:   document.getElementById('opt-download-path'),
  optBrowseBtn:   document.getElementById('opt-browse-btn'),
  optConnections: document.getElementById('opt-connections'),
  optConnVal:     document.getElementById('opt-connections-val'),
  optThreshold:   document.getElementById('opt-threshold-select'),
  optLimitToggle: document.getElementById('opt-limit-toggle'),
  optLimitSpeed:  document.getElementById('opt-limit-speed'),

  // Properties Modal
  propModal:      document.getElementById('properties-modal'),
  propModalClose: document.getElementById('properties-modal-close'),
  propCloseBtn:   document.getElementById('prop-close-btn'),
  propCopyBtn:    document.getElementById('prop-copy-btn'),
  propCard:       document.getElementById('prop-download-card'),
  propMediaSects: document.getElementById('media-info-sections'),

  // View menu (top nav)
  navView:        document.getElementById('nav-view'),
  viewMenu:       document.getElementById('view-menu'),

  // Download Detail Modal
  ddModal:      document.getElementById('dl-detail-modal'),
  ddTitle:      document.getElementById('dd-title'),
  ddMinimize:   document.getElementById('dd-minimize'),
  ddCloseX:     document.getElementById('dd-close-x'),
  ddInfoCard:   document.getElementById('dd-info-card'),
  ddSettCard:   document.getElementById('dd-settings-card'),
  ddPaneInfo:   document.getElementById('dd-pane-info'),
  ddPaneSett:   document.getElementById('dd-pane-settings'),
  ddProgress:   document.getElementById('dd-progress'),
  ddPartToggle: document.getElementById('dd-part-toggle'),
  ddPartInfo:   document.getElementById('dd-part-info'),
  ddSegments:   document.getElementById('dd-segments'),
  ddPartTbody:  document.getElementById('dd-part-tbody'),
  ddPause:      document.getElementById('dd-pause'),
  ddClose:      document.getElementById('dd-close'),
};

// SVG File Icons based on category
const ICONS = {
  music: `<svg viewBox="0 0 24 24"><path d="M9 18V5l12-2v13"></path><circle cx="6" cy="18" r="3"></circle><circle cx="18" cy="16" r="3"></circle></svg>`,
  compressed: `<svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"></rect><line x1="12" y1="3" x2="12" y2="21"></line><path d="M9 8h6M9 12h6M9 16h6"></path></svg>`,
  videos: `<svg viewBox="0 0 24 24"><rect x="2" y="2" width="20" height="20" rx="2.18" ry="2.18"></rect><line x1="7" y1="2" x2="7" y2="22"></line><line x1="17" y1="2" x2="17" y2="22"></line><line x1="2" y1="12" x2="22" y2="12"></line></svg>`,
  programs: `<svg viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"></rect><rect x="9" y="9" width="6" height="6"></rect></svg>`,
  documents: `<svg viewBox="0 0 24 24"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path><polyline points="14 2 14 8 20 8"></polyline></svg>`,
  apks: `<svg viewBox="0 0 24 24"><line x1="6" y1="12" x2="18" y2="12"></line><line x1="12" y1="6" x2="12" y2="18"></line><rect x="3" y="3" width="18" height="18" rx="2"></rect></svg>`,
  images: `<svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"></rect><circle cx="8.5" cy="8.5" r="1.5"></circle><polyline points="21 15 16 10 5 21"></polyline></svg>`,
  generic: `<svg viewBox="0 0 24 24"><path d="M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"></path><polyline points="13 2 13 9 20 9"></polyline></svg>`
};

// Initial Load
function init() {
  loadData();
  loadOptions();
  setupEventListeners();
  renderTable();
  updateToolbarState();
  initDiskWidget();
  // Push the saved options into the native engine. Without this the engine
  // only learned the settings when the user re-saved them in the Options
  // dialog, so a restart silently reverted to the built-in defaults.
  applyOptionsToNativeWhenReady();
}

// ---- Generic modal dialog (alert/confirm/prompt replacement) --------------
// The embedded webview (Ultralight) implements no JS dialog callbacks, so
// window.alert/confirm/prompt silently no-op (confirm() returns undefined —
// which once made "Delete from Disk" unreachable from the toolbar). These
// Promise-based helpers provide the same interactions with our own markup.
const dlg = {};

function showDialog({ title, message = '', input = null, buttons }) {
  dlg.overlay = dlg.overlay || document.getElementById('app-dialog');
  dlg.title   = dlg.title   || document.getElementById('dialog-title');
  dlg.message = dlg.message || document.getElementById('dialog-message');
  dlg.input   = dlg.input   || document.getElementById('dialog-input');
  dlg.buttons = dlg.buttons || document.getElementById('dialog-buttons');

  return new Promise((resolve) => {
    dlg.title.textContent = title;
    dlg.message.textContent = message;
    dlg.message.style.display = message ? '' : 'none';
    if (input) {
      dlg.input.style.display = '';
      dlg.input.value = input.value || '';
      dlg.input.placeholder = input.placeholder || '';
    } else {
      dlg.input.style.display = 'none';
    }

    dlg.buttons.innerHTML = '';
    const close = (value) => {
      dlg.overlay.style.display = 'none';
      document.removeEventListener('keydown', onKey, true);
      resolve(value);
    };
    buttons.forEach((b) => {
      const btn = document.createElement('button');
      btn.className = 'modal-btn ' + (b.kind || 'modal-btn-secondary');
      btn.textContent = b.label;
      btn.addEventListener('click', () => close(b.value));
      dlg.buttons.appendChild(btn);
    });

    const onKey = (e) => {
      if (dlg.overlay.style.display === 'none') return;
      if (e.key === 'Escape') {
        e.stopPropagation();
        close(buttons[buttons.length - 1].value);   // last button = cancel
      } else if (e.key === 'Enter' && input) {
        e.stopPropagation();
        close(buttons[0].value);                    // first button = confirm
      }
    };
    document.addEventListener('keydown', onKey, true);

    dlg.overlay.style.display = 'flex';
    if (input) setTimeout(() => { dlg.input.focus(); dlg.input.select(); }, 0);
  });
}

function uiAlert(title, message) {
  return showDialog({
    title, message,
    buttons: [{ label: 'OK', kind: 'modal-btn-primary', value: true }]
  });
}

// Three-way delete chooser: 'disk' | 'list' | 'cancel'.
function uiDeleteChoice(name) {
  return showDialog({
    title: 'Delete Download',
    message: name,
    buttons: [
      { label: 'Delete from Disk', kind: 'modal-btn-danger', value: 'disk' },
      { label: 'Remove from List', kind: 'modal-btn-primary', value: 'list' },
      { label: 'Cancel', kind: 'modal-btn-secondary', value: 'cancel' }
    ]
  });
}

function uiConfirmDanger(title, message, okLabel) {
  return showDialog({
    title, message,
    buttons: [
      { label: okLabel, kind: 'modal-btn-danger', value: true },
      { label: 'Cancel', kind: 'modal-btn-secondary', value: false }
    ]
  });
}

// Resolves the entered string, or null when cancelled.
function uiPrompt(title, message, value = '') {
  const PROMPT_OK = '__prompt_ok__';
  return showDialog({
    title, message,
    input: { value },
    buttons: [
      { label: 'OK', kind: 'modal-btn-primary', value: PROMPT_OK },
      { label: 'Cancel', kind: 'modal-btn-secondary', value: null }
    ]
  }).then(v => (v === PROMPT_OK ? dlg.input.value : null));
}

// Disk Space Widget
function updateDiskWidget() {
  const pctEl = document.querySelector('.disk-pct-text');
  const arcEl = document.querySelector('.disk-active-circle');
  const centerEl = document.querySelector('.disk-center-text');
  const pathEl = document.querySelector('.disk-path');

  if (typeof GetDiskSpace !== 'function') return;

  let info;
  try {
    const raw = GetDiskSpace();
    if (!raw || raw === '{}') return;
    info = JSON.parse(raw);
  } catch (e) {
    return;
  }

  const pct = parseFloat(info.pct);
  if (Number.isNaN(pct)) return;

  const clampedPct = Math.max(0, Math.min(100, pct));

  if (pctEl) pctEl.textContent = Math.round(clampedPct) + '%';
  if (centerEl) centerEl.textContent = info.drive || 'C:';
  if (pathEl) pathEl.textContent = info.path || '';

  // Arc: circumference = 2 * PI * 40 (r=40 from SVG)
  if (arcEl) {
    const circumference = 251.2;
    const offset = circumference * (1 - clampedPct / 100);
    arcEl.style.strokeDasharray = circumference;
    arcEl.style.strokeDashoffset = offset;
  }
}

// Wait for the native disk-space bridge before the first update, then refresh periodically.
function initDiskWidget() {
  updateDiskWidget();

  const bridgeCheck = setInterval(() => {
    if (typeof GetDiskSpace === 'function') {
      clearInterval(bridgeCheck);
      updateDiskWidget();
      // Refresh the disk widget every 5 seconds so it tracks real usage.
      setInterval(updateDiskWidget, 5000);
    }
  }, 100);

  // Stop polling after 10 seconds so we don't spin forever in a preview browser.
  setTimeout(() => clearInterval(bridgeCheck), 10000);
}

// Load data from localStorage
function loadData() {
  // Fall back to the pre-rename 'idm_downloads' key so existing history
  // survives the ODM rebrand (migrated forward on the next saveData()).
  const data = localStorage.getItem('odm_downloads') ||
               localStorage.getItem('idm_downloads');
  if (data) {
    try {
      downloads = JSON.parse(data).filter(dl => dl && dl.id && !dl.id.startsWith('dl_demo'));
      // Reset any active downloading state to stopped on load, since app restarted
      downloads.forEach(dl => {
        if (dl.status === 'downloading') {
          dl.status = 'stopped';
          dl.timeLeft = '--';
          dl.speed = '';
        }
        dl.checked = false; // default unselected
      });
      saveData();
    } catch (e) {
      downloads = [];
    }
  } else {
    downloads = [];
    saveData();
  }
}

// Save data to localStorage. Row .ctx (browser request context) holds session
// cookies and must never hit disk, so it is stripped before serialization.
// Transient retry state (keys starting with '_') is also stripped.
function saveData() {
  localStorage.setItem('odm_downloads', JSON.stringify(downloads.map(dl => {
    if (!dl) return dl;
    const clean = {};
    for (const key of Object.keys(dl)) {
      if (key === 'ctx' || key.startsWith('_')) continue;
      clean[key] = dl[key];
    }
    return clean;
  })));
  lastSaveTs = Date.now();
}

// Save at most ~once per second during progress ticks to avoid stringifying
// the whole list 10x/sec (GC pressure + jank in the embedded webview).
function throttledSave() {
  const now = Date.now();
  if (now - lastSaveTs >= 1000) saveData();
}

// ---- Auto-Retry Helpers ----
// Compute the retry delay for a given attempt count (exponential backoff,
// starting at 2s, doubling each time, capped at 60s).
function getRetryDelay(count) {
  return Math.min(2000 * Math.pow(2, count), 60000);
}

// Schedule an auto-retry for a failed download. Shows a countdown in the
// timeLeft column while waiting, then re-launches the download.
function scheduleRetry(dl) {
  const delay = getRetryDelay(dl._retryCount || 0);
  const retryAt = Date.now() + delay;

  // Countdown interval updates timeLeft every second
  dl._retryCountdown = setInterval(() => {
    const remaining = Math.max(0, Math.ceil((retryAt - Date.now()) / 1000));
    dl.timeLeft = 'Retry in ' + remaining + 's...';
    if (!patchActiveRow(dl)) renderTable();
    if (remaining <= 0) clearInterval(dl._retryCountdown);
  }, 1000);

  // Actual retry fires after the delay
  dl._retryTimer = setTimeout(() => {
    clearInterval(dl._retryCountdown);
    dl._retryCountdown = null;
    dl._retryCount = (dl._retryCount || 0) + 1;
    dl.status = 'downloading';
    dl.timeLeft = 'Retrying...';
    saveData();
    renderTable();
    updateToolbarState();
    requestStart(dl);
  }, delay);
}

// Cancel any pending auto-retry for a download.
function cancelRetry(dl) {
  if (dl._retryTimer) { clearTimeout(dl._retryTimer); dl._retryTimer = null; }
  if (dl._retryCountdown) { clearInterval(dl._retryCountdown); dl._retryCountdown = null; }
}

// Kick off (or queue) a download for the given row. The engine runs one job
// at a time, so if another is active we stop it and remember this request;
// UI.onComplete starts the queued one once the engine is free.
function requestStart(dl) {
  const nativeAvailable = typeof StartDownload === 'function';
  const active = downloads.find(d => d.status === 'downloading' && d.id !== dl.id);

  if (nativeAvailable && active) {
    // Re-queueing the same row must not enqueue it twice (clicking Resume
    // repeatedly would otherwise start it once per click later on).
    const queued = pendingStarts.find(q => q.id === dl.id);
    const entry = { id: dl.id, url: dl.url, path: dl.path, ctx: dl.ctx || null };
    if (queued) Object.assign(queued, entry);
    else pendingStarts.push(entry);
    dl.timeLeft = 'Waiting...';
    if (typeof StopDownload === 'function') StopDownload();
    return;
  }

  if (nativeAvailable) {
    StartDownload(dl.url, dl.path, dl.id, dl.ctx ? JSON.stringify(dl.ctx) : '');
  } else {
    simulateMockDownload(dl.id);
  }
}

// Patch only the active row's progress bar/labels instead of rebuilding the
// entire table on every progress tick.
function patchActiveRow(dl) {
  const tr = el.tableBody.querySelector(`tr[data-id="${dl.id}"]`);
  if (!tr) return false;
  const fill = tr.querySelector('.progress-fill');
  const stats = tr.querySelectorAll('.progress-stats-row span');
  if (!fill || stats.length < 2) return false;   // row isn't in "downloading" layout
  fill.style.width = dl.pct + '%';
  stats[0].textContent = `${dl.downloaded || '0 B'} of ${dl.total || '0 B'} (${dl.pct.toFixed(0)}%)`;
  stats[1].textContent = dl.speed || '';
  const timeCell = tr.children[4];
  if (timeCell) timeCell.textContent = dl.timeLeft || '--';
  return true;
}

// Categorize file extensions
function getFileCategory(filename) {
  const ext = filename.split('.').pop().toLowerCase();
  
  if (['mp3', 'wav', 'flac', 'ogg', 'm4a', 'aac', 'wma'].includes(ext)) return 'music';
  if (['zip', 'rar', '7z', 'tar', 'gz', 'bz2', 'zipx'].includes(ext)) return 'compressed';
  if (['mp4', 'mkv', 'avi', 'mov', 'flv', 'wmv', 'webm', 'm4v'].includes(ext)) return 'videos';
  if (['exe', 'msi', 'bat', 'sh', 'app', 'dmg', 'bin', 'com'].includes(ext)) return 'programs';
  if (['pdf', 'doc', 'docx', 'xls', 'xlsx', 'ppt', 'pptx', 'txt', 'rtf', 'odt', 'csv'].includes(ext)) return 'documents';
  if (['apk'].includes(ext)) return 'apks';
  if (['png', 'jpg', 'jpeg', 'gif', 'svg', 'webp', 'bmp', 'ico', 'tiff'].includes(ext)) return 'images';
  
  return 'generic';
}

function getFileIcon(filename) {
  const category = getFileCategory(filename);
  return ICONS[category] || ICONS.generic;
}

function getFileExtension(filename) {
  const dot = filename.lastIndexOf('.');
  return dot !== -1 ? filename.substring(dot).toUpperCase() : 'FILE';
}

// Render Table Rows
function renderTable() {
  const filterText = el.searchInput.value.trim().toLowerCase();
  
  // Filter based on search input and active category sidebar selection
  const filtered = downloads.filter(dl => {
    // Search match
    const matchSearch = dl.name.toLowerCase().includes(filterText) || dl.url.toLowerCase().includes(filterText);
    
    if (!matchSearch) return false;
    
    // Category match
    if (activeCategory === 'all') return true;
    if (activeCategory === 'unfinished') return dl.status !== 'finished';
    if (activeCategory === 'finished') return dl.status === 'finished';
    
    return getFileCategory(dl.name) === activeCategory;
  });
  
  // Empty state handling
  if (filtered.length === 0) {
    el.tableBody.innerHTML = `
      <tr class="empty-row">
        <td colspan="6">
          <div class="empty-state">
            <svg viewBox="0 0 24 24" class="empty-icon"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path><polyline points="7 10 12 15 17 10"></polyline><line x1="12" y1="15" x2="12" y2="3"></line></svg>
            <p class="empty-title">No Downloads Found</p>
            <p class="empty-desc">${filterText ? 'No search results match your query.' : 'Click "Add URL" to start a new download.'}</p>
          </div>
        </td>
      </tr>
    `;
    syncSelectAllCheckbox();
    return;
  }
  
  el.tableBody.innerHTML = filtered.map(dl => {
    const isSelected = selectedId === dl.id;
    const isDownloading = dl.status === 'downloading';
    
    // Status text format
    let statusHTML = `<span class="status-badge ${dl.status}">${capitalize(dl.status)}</span>`;
    
    return `
      <tr data-id="${dl.id}" class="${isSelected ? 'selected' : ''}">
        <td class="cb-cell"><input type="checkbox" class="row-checkbox" data-id="${dl.id}" ${dl.checked ? 'checked' : ''} /></td>
        <td>
          <div class="file-name-cell">
            <div class="file-icon">${getFileIcon(dl.name)}</div>
            <div class="file-info">
              <span class="file-title" title="${dl.name}">${dl.name}</span>
              <span class="file-subtitle">${getFileExtension(dl.name)}</span>
              ${isDownloading ? `
                <div class="progress-wrapper">
                  <div class="progress-track">
                    <div class="progress-fill" style="width: ${dl.pct}%"></div>
                  </div>
                  <div class="progress-stats-row">
                    <span>${dl.downloaded || '0 B'} of ${dl.total || '0 B'} (${dl.pct.toFixed(0)}%)</span>
                    <span>${dl.speed || ''}</span>
                  </div>
                </div>
              ` : ''}
            </div>
          </div>
        </td>
        <td>${dl.size}</td>
        <td>${statusHTML}</td>
        <td>${dl.timeLeft || '--'}</td>
        <td>${dl.lastModified}</td>
      </tr>
    `;
  }).join('');

  syncSelectAllCheckbox();
}

// Tool button state manager
function updateToolbarState() {
  const selected = downloads.find(dl => dl.id === selectedId);
  const activeDl = downloads.find(dl => dl.status === 'downloading');
  const anyChecked = downloads.some(dl => dl.checked);

  // Trash is active when a row is highlighted OR any rows are ticked via
  // their checkboxes (checked rows take precedence for the delete action).
  el.tbDelete.disabled = !(selected || anyChecked);

  if (selected) {
    if (selected.status === 'downloading') {
      el.tbStop.disabled = false;
      el.tbResume.disabled = true;
    } else if (selected.status === 'stopped' || selected.status === 'failed') {
      el.tbResume.disabled = false;
      // Allow Stop when a failed download has a pending auto-retry
      el.tbStop.disabled = !selected._retryTimer;
    } else { // finished
      el.tbResume.disabled = true;
      el.tbStop.disabled = true;
    }
  } else {
    el.tbResume.disabled = true;
    el.tbStop.disabled = true;
  }

  el.tbStopAll.disabled = !activeDl;
}

// Keep the header "select all" checkbox in sync with the row checkboxes:
// checked when every visible row is ticked, indeterminate when only some
// are, and unchecked when none are (or the list is empty).
function syncSelectAllCheckbox() {
  const boxes = el.tableBody.querySelectorAll('.row-checkbox');
  let ticked = 0;
  boxes.forEach(cb => { if (cb.checked) ticked++; });
  el.selectAll.checked = boxes.length > 0 && ticked === boxes.length;
  el.selectAll.indeterminate = ticked > 0 && ticked < boxes.length;
}

// Capitalize helper
function capitalize(s) {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// Setup events
function setupEventListeners() {
  // Search Input filter
  el.searchInput.addEventListener('input', () => {
    renderTable();
  });
  
  // Select All Header Checkbox — use click on the parent <th> because
  // Ultralight's WebKit does not fire click/change events on <input> elements
  // with -webkit-appearance:none. pointer-events:none on the checkbox makes
  // the click land on the <th> instead; we toggle .checked manually.
  el.selectAll.parentElement.addEventListener('click', () => {
    const newState = !el.selectAll.checked;
    el.selectAll.checked = newState;
    el.selectAll.indeterminate = false;
    downloads.forEach(dl => {
      const tr = el.tableBody.querySelector(`tr[data-id="${dl.id}"]`);
      if (tr) {
        dl.checked = newState;
        const cb = tr.querySelector('.row-checkbox');
        if (cb) cb.checked = newState;
      }
    });
    syncSelectAllCheckbox();
    updateToolbarState();
  });
  
  // Table Body Row Actions (click to select, right click for context menu)
  // Checkbox inputs have pointer-events:none (Ultralight workaround), so
  // clicking the checkbox area actually targets the parent <td class="cb-cell">.
  // We detect that and manually toggle the checkbox.
  el.tableBody.addEventListener('click', (e) => {
    const tr = e.target.closest('tr');
    if (!tr || tr.classList.contains('empty-row')) return;
    
    const id = tr.getAttribute('data-id');
    
    // Checkbox cell click — pointer-events:none on the <input> means e.target
    // is the <td class="cb-cell">, not the checkbox itself.
    const cbCell = e.target.closest('.cb-cell');
    if (cbCell) {
      const cb = cbCell.querySelector('.row-checkbox');
      if (cb) {
        cb.checked = !cb.checked;           // manually toggle
        const dl = downloads.find(x => x.id === id);
        if (dl) dl.checked = cb.checked;
      }

      syncSelectAllCheckbox();
      updateToolbarState();
      return;
    }
    
    selectedId = id;
    
    // Highlight visually
    const rows = el.tableBody.querySelectorAll('tr');
    rows.forEach(r => r.classList.remove('selected'));
    tr.classList.add('selected');
    
    updateToolbarState();
  });
  
  // Double-click a finished row to open the file with the default app.
  el.tableBody.addEventListener('dblclick', (e) => {
    const tr = e.target.closest('tr');
    if (!tr || tr.classList.contains('empty-row')) return;
    if (e.target.closest('.cb-cell')) return;
    
    const id = tr.getAttribute('data-id');
    const dl = downloads.find(x => x.id === id);
    if (!dl) return;
    
    selectedId = id;
    
    const rows = el.tableBody.querySelectorAll('tr');
    rows.forEach(r => r.classList.remove('selected'));
    tr.classList.add('selected');
    updateToolbarState();
    
    // Downloading rows open the live detail modal; everything else keeps the
    // original open-the-file behavior.
    if (dl.status === 'downloading') {
      openDownloadDetail(dl);
      return;
    }
    openDownloadedFile(dl);
  });

  
  // Custom Context Menu on Right Click
  // Some webviews (e.g. Ultralight) do not fire the 'contextmenu' event, so we
  // also show the menu on the right-button mouseup. When BOTH fire (standard
  // browsers dispatch mouseup -> contextmenu for the same gesture), the second
  // call is suppressed via the timestamp below to avoid a double trigger.
  let lastCtxMenuShow = 0;
  function showContextMenu(e, tr) {
    if (!tr || tr.classList.contains('empty-row')) return;

    const id = tr.getAttribute('data-id');
    selectedId = id;

    // Refresh selections
    const rows = el.tableBody.querySelectorAll('tr');
    rows.forEach(r => r.classList.remove('selected'));
    tr.classList.add('selected');
    updateToolbarState();

    // Position and show menu
    el.contextMenu.style.display = 'flex';

    const rect = el.contextMenu.getBoundingClientRect();
    const x = Math.min(e.clientX, window.innerWidth - rect.width - 8);
    const y = Math.min(e.clientY, window.innerHeight - rect.height - 8);
    el.contextMenu.style.left = `${Math.max(8, x)}px`;
    el.contextMenu.style.top = `${Math.max(8, y)}px`;

    lastCtxMenuShow = Date.now();
  }

  // True when the other handler already showed the menu for this gesture.
  function ctxMenuJustShown() {
    return Date.now() - lastCtxMenuShow < 350;
  }

  el.tableBody.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    if (ctxMenuJustShown()) return;   // mouseup path already handled it
    showContextMenu(e, e.target.closest('tr'));
  });

  let rightMouseDown = false;
  el.tableBody.addEventListener('mousedown', (e) => {
    if (e.button !== 2) return;
    rightMouseDown = true;
    e.preventDefault();
  });

  el.tableBody.addEventListener('mouseup', (e) => {
    if (e.button !== 2 || !rightMouseDown) return;
    rightMouseDown = false;
    e.preventDefault();
    if (ctxMenuJustShown()) return;   // contextmenu path already handled it
    showContextMenu(e, e.target.closest('tr'));
  });
  
  // Close context menu on click elsewhere
  document.addEventListener('click', () => {
    el.contextMenu.style.display = 'none';
    rightMouseDown = false;
  });
  
  // Handle Context Menu Actions
  el.contextMenu.addEventListener('click', (e) => {
    const item = e.target.closest('.menu-item');
    if (!item) return;
    
    const action = item.getAttribute('data-action');
    handleContextMenuAction(action);
  });
  
  // Sidebar Category Filter selection
  el.categoryItems.forEach(item => {
    item.addEventListener('click', () => {
      el.categoryItems.forEach(x => x.classList.remove('active'));
      item.classList.add('active');
      activeCategory = item.getAttribute('data-category');
      selectedId = null;
      renderTable();
      updateToolbarState();
    });
  });
  
  // Toolbar Buttons Actions
  el.tbAddUrl.addEventListener('click', () => {
    openModal();
  });
  
  el.tbResume.addEventListener('click', () => {
    resumeSelected();
  });
  
  el.tbStop.addEventListener('click', () => {
    stopSelected();
  });
  
  el.tbStopAll.addEventListener('click', () => {
    stopAllDownloads();
  });
  
  el.tbDelete.addEventListener('click', async () => {
    // Checked rows take precedence (batch delete); otherwise the highlighted
    // row. Mirrors updateToolbarState(), which enables this button for both.
    const targets = getDeleteTargets();
    if (targets.length === 0) return;
    // Only ask about disk deletion when there is a real file path and the
    // download actually finished (deleting an in-progress temp file is
    // pointless — StopDownload handles that).
    const canDeleteFromDisk = targets.some(dl => dl.path &&
                                                 dl.status === 'finished');
    if (canDeleteFromDisk) {
      const label = targets.length === 1 ? targets[0].name
                                         : `${targets.length} downloads`;
      const choice = await uiDeleteChoice(label);
      if (choice === 'cancel') return;
      deleteSelected(choice === 'disk');
    } else {
      deleteSelected(false);
    }
  });
  
  el.tbOptions.addEventListener('click', openOptionsModal);
  
  // Modal Buttons
  el.modalClose.addEventListener('click', closeModal);
  el.modalCancel.addEventListener('click', closeModal);
  
  el.pasteBtn.addEventListener('click', async () => {
    try {
      const text = await navigator.clipboard.readText();
      if (text) {
        el.urlInput.value = text.trim();
        requestUrlProbe();          // pasted a full URL — check it right away
      }
    } catch (e) {
      showStatusToast('Clipboard access denied. Please paste manually (Ctrl+V).');
    }
  });
  
  el.browseBtn.addEventListener('click', () => {
    const urlVal = el.urlInput.value.trim();
    const suggested = (urlVal.split('/').pop() || 'download.bin').split('?')[0];
    
    // Call Native pick path injected in C++
    if (typeof PickSavePath === 'function') {
      const chosen = PickSavePath(suggested);
      if (chosen) el.pathInput.value = chosen;
    } else {
      // Mock for standard browsers
      el.pathInput.value = 'C:\\Downloads\\DownloadManager\\' + suggested;
    }
  });
  
  el.startBtn.addEventListener('click', () => {
    startNewDownload();
  });
  
  el.urlInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') startNewDownload();
  });

  // Live link probe while the user types/pastes into the URL field.
  el.urlInput.addEventListener('input', scheduleUrlProbe);

  // Top nav menu: data-action driven handlers (Downloads opens the folder).
  document.querySelectorAll('.nav-item[data-action]').forEach(item => {
    item.addEventListener('click', (e) => {
      e.preventDefault();
      const action = item.getAttribute('data-action');
      if (action === 'open-downloads') openDownloadsFolder();
    });
  });

  setupViewMenu();
  setupOptionsEventListeners();
  setupPropertiesEventListeners();
  setupDownloadDetailListeners();
}

// ---- View menu (top nav): persisted layout toggles ----
const DEFAULT_VIEW_PREFS = { compactRows: false, showDisk: true };
let viewPrefs = { ...DEFAULT_VIEW_PREFS };

function loadViewPrefs() {
  try {
    const raw = localStorage.getItem('odm_view');
    if (raw) viewPrefs = { ...DEFAULT_VIEW_PREFS, ...JSON.parse(raw) };
  } catch (e) {
    viewPrefs = { ...DEFAULT_VIEW_PREFS };
  }
}

function applyViewPrefs() {
  document.body.classList.toggle('compact-rows', viewPrefs.compactRows);
  document.body.classList.toggle('hide-disk', !viewPrefs.showDisk);
  el.viewMenu.querySelectorAll('.view-toggle').forEach(item => {
    const key = item.getAttribute('data-toggle');
    item.classList.toggle('on', !!viewPrefs[key]);
  });
}

function setupViewMenu() {
  loadViewPrefs();
  applyViewPrefs();

  el.navView.addEventListener('click', (e) => {
    e.preventDefault();
    e.stopPropagation();
    el.viewMenu.classList.toggle('open');
  });

  // Toggle an option; the menu stays open so several can be flipped in a row.
  el.viewMenu.addEventListener('click', (e) => {
    e.stopPropagation();
    const item = e.target.closest('.view-toggle');
    if (!item) return;
    const key = item.getAttribute('data-toggle');
    viewPrefs[key] = !viewPrefs[key];
    localStorage.setItem('odm_view', JSON.stringify(viewPrefs));
    applyViewPrefs();
  });

  // Any click outside closes the dropdown.
  document.addEventListener('click', (e) => {
    if (!e.target.closest('.nav-dropdown-wrap')) el.viewMenu.classList.remove('open');
  });
}

// Open the user's downloads folder via the native bridge. Honors the configured
// Options folder; falls back to the OS Downloads folder; surfaces a toast on
// any failure (folder missing, bridge unavailable, etc.).
function openDownloadsFolder() {
  if (typeof OpenDownloadsFolder !== 'function') {
    showStatusToast('Open folder bridge is not available.');
    return;
  }
  let result;
  try {
    result = OpenDownloadsFolder(optionsCache.downloadPath || '');
  } catch (e) {
    showStatusToast('Could not open downloads folder.');
    return;
  }
  // The bridge returns a JSON string: {"ok":bool,"reason":str,"path":str}
  let parsed = null;
  try { parsed = JSON.parse(result); } catch (e) { /* legacy bool */ }
  if (parsed) {
    if (parsed.ok) {
      showStatusToast('Opened downloads folder.');
    } else if (parsed.reason === 'no_downloads_folder') {
      showStatusToast('Could not resolve the OS Downloads folder.');
    } else if (parsed.reason === 'not_found') {
      showStatusToast('Downloads folder could not be created.');
    } else {
      showStatusToast('Could not open downloads folder.');
    }
  } else if (result === true || result === 'true') {
    showStatusToast('Opened downloads folder.');
  } else {
    showStatusToast('Could not open downloads folder.');
  }
}

// Modal open/close controls
function openModal() {
  el.urlInput.value = '';
  el.pathInput.value = '';
  resetUrlProbe();
  el.modal.style.display = 'flex';
  el.urlInput.focus();
}

function closeModal() {
  el.modal.style.display = 'none';
  resetUrlProbe();
}

// ---- URL probe (what IS this link?) ---------------------------------------
// The Add-Download modal asks the native side to HEAD/range-GET the URL so
// the user sees "MP4 Video · 5.2 MB" (or "EXE"!) BEFORE committing. Results
// return async via UI.onUrlProbe; a sequence id drops stale replies.

let probeSeq = 0;
let currentProbeId = null;
let lastProbe = null;         // { forUrl, filename } — consumed on start
let probeTimer = null;

const MIME_LABELS = {
  'video/mp4': 'MP4 Video',           'video/webm': 'WebM Video',
  'video/x-matroska': 'MKV Video',    'video/quicktime': 'MOV Video',
  'video/x-msvideo': 'AVI Video',     'video/mp2t': 'TS Video',
  'audio/mpeg': 'MP3 Audio',          'audio/mp4': 'M4A Audio',
  'audio/aac': 'AAC Audio',           'audio/ogg': 'OGG Audio',
  'audio/wav': 'WAV Audio',           'audio/flac': 'FLAC Audio',
  'image/jpeg': 'JPEG Image',         'image/png': 'PNG Image',
  'image/gif': 'GIF Image',           'image/webp': 'WebP Image',
  'image/svg+xml': 'SVG Image',       'application/pdf': 'PDF Document',
  'application/zip': 'ZIP Archive',   'application/x-rar-compressed': 'RAR Archive',
  'application/x-7z-compressed': '7Z Archive',
  'application/gzip': 'GZIP Archive', 'application/x-tar': 'TAR Archive',
  'application/x-msdownload': 'Windows Program (EXE)',
  'application/x-msdos-program': 'Windows Program (EXE)',
  'application/vnd.microsoft.portable-executable': 'Windows Program (EXE)',
  'application/x-msi': 'Windows Installer (MSI)',
  'application/x-iso9660-image': 'Disc Image (ISO)',
  'application/json': 'JSON File',
  'application/vnd.android.package-archive': 'Android App (APK)',
  'application/x-mpegurl': 'HLS Stream', 'application/vnd.apple.mpegurl': 'HLS Stream',
  'text/html': 'Web Page (not a file!)', 'text/plain': 'Text File'
};

// application/octet-stream says nothing — fall back to the file extension.
const EXT_LABELS = {
  mp4: 'MP4 Video', mkv: 'MKV Video', webm: 'WebM Video', avi: 'AVI Video',
  mov: 'MOV Video', mp3: 'MP3 Audio', m4a: 'M4A Audio', flac: 'FLAC Audio',
  jpg: 'JPEG Image', jpeg: 'JPEG Image', png: 'PNG Image', gif: 'GIF Image',
  pdf: 'PDF Document', zip: 'ZIP Archive', rar: 'RAR Archive', '7z': '7Z Archive',
  exe: 'Windows Program (EXE)', msi: 'Windows Installer (MSI)',
  iso: 'Disc Image (ISO)', apk: 'Android App (APK)', torrent: 'Torrent File'
};

function extOfName(name) {
  const m = /\.([a-z0-9]{1,5})(?:$|[?#])/i.exec(name || '');
  return m ? m[1].toLowerCase() : '';
}

function describeFileType(mime, filename, url) {
  mime = String(mime || '').split(';')[0].trim().toLowerCase();
  if (MIME_LABELS[mime]) return MIME_LABELS[mime];
  const ext = extOfName(filename) ||
              extOfName((url || '').split('?')[0].split('/').pop());
  if (EXT_LABELS[ext]) return EXT_LABELS[ext];
  if (mime.startsWith('video/')) return mime.slice(6).toUpperCase() + ' Video';
  if (mime.startsWith('audio/')) return mime.slice(6).toUpperCase() + ' Audio';
  if (mime.startsWith('image/')) return mime.slice(6).toUpperCase() + ' Image';
  if (ext) return '.' + ext.toUpperCase() + ' File';
  return mime || 'Unknown';
}

// Short badge for the strip ("MP4", "PDF", "EXE"...).
function fileTypeBadge(label) {
  const m = /\(([A-Z0-9]+)\)/.exec(label);           // "(EXE)" style labels
  if (m) return m[1];
  return (label.split(' ')[0] || '?').slice(0, 6);
}

function resetUrlProbe() {
  clearTimeout(probeTimer);
  currentProbeId = null;
  lastProbe = null;
  if (el.probeBox) {
    el.probeBox.style.display = 'none';
    el.probeBox.classList.remove('err');
  }
}

// Host match, not a substring test: "example.com/?r=youtube.com" is not a
// YouTube link and must not be routed to the extractor.
function isYouTubeUrl(url) {
  try {
    let h = new URL(url).hostname.toLowerCase().replace(/^(www|m)\./, '');
    return h === 'youtube.com' || h === 'youtu.be' ||
           h === 'music.youtube.com' || h === 'youtube-nocookie.com';
  } catch (e) { return false; }
}

function requestUrlProbe() {
  if (!el.probeBox) return;
  const url = el.urlInput.value.trim();
  lastProbe = null;
  if (!/^https?:\/\//i.test(url) || typeof ProbeUrl !== 'function') {
    el.probeBox.style.display = 'none';
    return;
  }
  // A YouTube watch page is HTML, not a file: probing it would report
  // "Web page · 200 KB", which is a lie about what the user is going to get.
  // The real name and size only exist after yt-dlp has resolved the page.
  if ((pendingContext && pendingContext.type === 'ytdlp') || isYouTubeUrl(url)) {
    el.probeBox.style.display = 'flex';
    el.probeBox.classList.remove('err');
    el.probeBadge.textContent = 'YT';
    const h = pendingContext && pendingContext.height;
    el.probeText.textContent = 'YouTube video  ·  ' +
      (h ? h + 'p with audio' : 'best quality with audio') +
      '  ·  resolved on start';
    return;
  }
  const id = 'probe_' + (++probeSeq);
  currentProbeId = id;
  el.probeBox.style.display = 'flex';
  el.probeBox.classList.remove('err');
  el.probeBadge.textContent = '';
  el.probeText.textContent = 'Checking link…';
  // Reuse the captured browser context (cookies/UA/referrer): CDN links like
  // Instagram's answer 403 without it.
  const ctx = pendingContext || {};
  ProbeUrl(url, id, ctx.cookies || '', ctx.referrer || '', ctx.userAgent || '');
}

function scheduleUrlProbe() {
  clearTimeout(probeTimer);
  probeTimer = setTimeout(requestUrlProbe, 500);   // debounce typing
}

function renderUrlProbe(info) {
  if (!el.probeBox) return;
  el.probeBox.style.display = 'flex';
  if (!info || !info.ok) {
    el.probeBox.classList.add('err');
    el.probeBadge.textContent = '';
    el.probeText.textContent = 'Could not check this link' +
      (info && info.http ? ' (HTTP ' + info.http + ')' : '') +
      ' — you can still try downloading.';
    return;
  }
  el.probeBox.classList.remove('err');
  const label = describeFileType(info.mime, info.filename,
                                 info.finalUrl || el.urlInput.value.trim());
  el.probeBadge.textContent = fileTypeBadge(label);
  const parts = [label];
  if (info.size > 0) parts.push(formatBytes(info.size));
  if (info.filename) parts.push(info.filename);
  parts.push(info.resumable ? 'Resume supported' : 'No resume');
  el.probeText.textContent = parts.join('  ·  ');
  // Better default name than a hashed CDN basename, consumed on start.
  if (info.filename)
    lastProbe = { forUrl: el.urlInput.value.trim(), filename: info.filename };
}

function showStatusToast(msg) {
  el.statusMsg.textContent = msg;
}

// ---- Options Modal --------------------------------------------------------

const DEFAULT_OPTIONS = {
  downloadPath: '',
  connections: 8,
  threshold: 10485760,
  speedLimitEnabled: false,
  speedLimit: 0
};

let optionsCache = { ...DEFAULT_OPTIONS };

function loadOptions() {
  try {
    const raw = localStorage.getItem('odm_options') ||
            localStorage.getItem('idm_options'); // pre-rename key fallback
    if (raw) {
      const parsed = JSON.parse(raw);
      optionsCache = { ...DEFAULT_OPTIONS, ...parsed };
    }
  } catch (e) {
    optionsCache = { ...DEFAULT_OPTIONS };
  }
}

function applyOptionsToUI() {
  el.optPathInput.value = optionsCache.downloadPath || '';
  el.optConnections.value = optionsCache.connections;
  el.optConnVal.textContent = optionsCache.connections;
  setCustomSelectValue(el.optThreshold, String(optionsCache.threshold));
  el.optLimitToggle.classList.toggle('on', optionsCache.speedLimitEnabled);
  el.optLimitSpeed.value = optionsCache.speedLimit || '';
  el.optLimitSpeed.disabled = !optionsCache.speedLimitEnabled;
}

function saveOptions() {
  optionsCache.downloadPath = el.optPathInput.value.trim();
  optionsCache.connections = parseInt(el.optConnections.value, 10) || 8;
  optionsCache.threshold = parseInt(getCustomSelectValue(el.optThreshold), 10) || 10485760;
  optionsCache.speedLimitEnabled = el.optLimitToggle.classList.contains('on');
  optionsCache.speedLimit = optionsCache.speedLimitEnabled
    ? (parseInt(el.optLimitSpeed.value, 10) || 0)
    : 0;

  localStorage.setItem('odm_options', JSON.stringify(optionsCache));

  // Push settings to native engine if available
  applyOptionsToNative();

  showStatusToast('Settings saved successfully.');
}

function applyOptionsToNative() {
  if (typeof ApplySettings !== 'function') return;
  ApplySettings(
    optionsCache.connections,
    optionsCache.threshold,
    optionsCache.speedLimitEnabled ? optionsCache.speedLimit * 1024 : 0,
    optionsCache.downloadPath
  );
}

// The native bridge functions are injected on DOM-ready, which can land AFTER
// this script's first run — poll briefly so startup settings still reach the
// engine (and give up quietly in a plain browser preview).
function applyOptionsToNativeWhenReady() {
  if (typeof ApplySettings === 'function') {
    applyOptionsToNative();
    return;
  }
  let tries = 0;
  const timer = setInterval(() => {
    if (typeof ApplySettings === 'function') {
      clearInterval(timer);
      applyOptionsToNative();
    } else if (++tries >= 100) {   // ~10s
      clearInterval(timer);
    }
  }, 100);
}

// Make a modal draggable by its header. Returns a controller with reset()
// so the modal can be re-centered every time it opens. Position is applied
// as an inline transform on top of the flexbox centering.
function makeModalDraggable(container, handle) {
  let dragging = false;
  let moved = false;            // true if the pointer actually moved mid-drag
  let startX = 0, startY = 0;   // pointer position minus current offset
  let offX = 0, offY = 0;       // current translate offset
  let baseLeft = 0, baseTop = 0; // untranslated container position
  let boundW = 0;               // container width, cached at mousedown —
                                // reading offsetWidth per mousemove forces a
                                // layout on every event and makes drags jerky
  const overlay = container.parentElement; // .modal-overlay (backdrop)

  handle.addEventListener('mousedown', (e) => {
    if (e.target.closest('button')) return; // close button must stay clickable
    dragging = true;
    moved = false;
    startX = e.clientX - offX;
    startY = e.clientY - offY;
    const rect = container.getBoundingClientRect();
    baseLeft = rect.left - offX;
    baseTop  = rect.top  - offY;
    boundW = container.offsetWidth;
    // .dragging drops the shadow and hit-testing while the modal moves (see
    // the CSS). .dragged PERMANENTLY suppresses the entry animation: it
    // animates transform, and letting it restart on mouseup made the modal
    // flash back to its default position before snapping to the drop spot.
    container.classList.add('dragging', 'dragged');
    // The backdrop also gets .dragging so it can own the move cursor while the
    // modal itself is pointer-events:none (see the CSS note).
    if (overlay) overlay.classList.add('dragging');
    e.preventDefault();
  });

  document.addEventListener('mousemove', (e) => {
    if (!dragging) return;
    moved = true;
    let nx = e.clientX - startX;
    let ny = e.clientY - startY;
    // Keep the header reachable: clamp so the modal never leaves the viewport
    // entirely (80px of it always stays visible horizontally, and the header
    // can never go above the top edge or below the bottom).
    nx = Math.max(-(baseLeft + boundW - 80), Math.min(nx, window.innerWidth - baseLeft - 80));
    ny = Math.max(-baseTop, Math.min(ny, window.innerHeight - baseTop - 40));
    offX = nx;
    offY = ny;
    // Written straight from the event instead of batching into rAF: Ultralight
    // paints the View once per run-loop tick no matter how many style writes
    // happened in between, so batching bought nothing and cost a frame of
    // latency whenever the rAF tick landed just after the engine's paint.
    // translate3d (not translate) so engines that do promote 3D transforms
    // (browsers, not Ultralight) give the modal its own layer.
    container.style.transform = `translate3d(${nx}px, ${ny}px, 0)`;
  });

  document.addEventListener('mouseup', () => {
    if (!dragging) return;
    dragging = false;
    container.classList.remove('dragging');
    if (overlay) overlay.classList.remove('dragging');
  });

  return {
    reset() {
      dragging = false;
      offX = 0;
      offY = 0;
      // Fresh open: shadow back AND the entry animation may play again.
      container.classList.remove('dragging', 'dragged');
      if (overlay) overlay.classList.remove('dragging');
      container.style.transform = '';
    },
    // Returns true (and clears the flag) when a drag just finished. Used to
    // suppress the synthetic click the browser fires on the overlay when the
    // mouse is released outside the modal after a drag.
    consumeDrag() {
      const was = moved;
      moved = false;
      return was;
    }
  };
}

let optModalDrag = null;

function openOptionsModal() {
  loadOptions();
  applyOptionsToUI();

  if (optModalDrag) optModalDrag.reset(); // always open centered
  el.optModal.style.display = 'flex';
}

function closeOptionsModal() {
  el.optModal.style.display = 'none';
}

// ---- Properties Modal (download details + MediaInfo media analysis) ----

// Minimal HTML escaping for values injected into the properties tables.
function escapeHtml(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

let propModalDrag = null;
let propMediaInfoRaw = '';       // raw Inform() text for "Copy Details"
let propCurrentDl = null;        // download shown in the open modal

function propKvRowsHtml(pairs) {
  return pairs.map(([k, v]) =>
    `<div class="prop-kv-row"><span class="prop-key">${escapeHtml(k)}</span>` +
    `<span class="prop-val">${escapeHtml(v)}</span></div>`).join('');
}

// Parse MediaInfo's sectioned text output (blocks separated by blank lines;
// first line of each block is the section name, then "Key : Value" lines)
// into [{name, pairs: [[k, v], ...]}, ...].
function parseMediaInfoText(text) {
  const sections = [];
  for (const block of text.replace(/\r/g, '').split(/\n{2,}/)) {
    const lines = block.split('\n').filter(l => l.trim() !== '');
    if (!lines.length) continue;
    const name = lines[0].trim();
    const pairs = [];
    for (let i = 1; i < lines.length; i++) {
      const sep = lines[i].indexOf(' : ');
      if (sep === -1) continue;
      pairs.push([lines[i].slice(0, sep).trim(), lines[i].slice(sep + 3).trim()]);
    }
    if (pairs.length) sections.push({ name, pairs });
  }
  return sections;
}

function openPropertiesModal(dl) {
  propCurrentDl = dl;
  propMediaInfoRaw = '';

  // Download details card, filled from the row's record.
  el.propCard.innerHTML = propKvRowsHtml([
    ['Name',          dl.name],
    ['URL',           dl.url],
    ['Save Path',     dl.path || '(not set)'],
    ['Size',          dl.size],
    ['Status',        capitalize(dl.status)],
    ['Last Modified', dl.lastModified || '--'],
  ]);

  // Media analysis: ask the native side, show a loading note meanwhile.
  const canAnalyze = typeof RequestMediaInfo === 'function' && dl.path;
  el.propMediaSects.innerHTML =
    `<div class="prop-note">${canAnalyze
      ? 'Analyzing media...'
      : 'Media analysis is available once the file exists on disk.'}</div>`;
  if (canAnalyze) RequestMediaInfo(dl.path, dl.id);

  if (propModalDrag) propModalDrag.reset(); // always open centered
  el.propModal.style.display = 'flex';
}

function closePropertiesModal() {
  el.propModal.style.display = 'none';
  propCurrentDl = null;
}

function renderMediaInfoSections(text) {
  const sections = parseMediaInfoText(text);
  if (!sections.length) {
    el.propMediaSects.innerHTML =
      '<div class="prop-note">No media information available for this file.</div>';
    return;
  }
  el.propMediaSects.innerHTML = sections.map(sec =>
    `<div class="opt-section">` +
    `<div class="opt-section-header"><span>${escapeHtml(sec.name)}</span></div>` +
    `<div class="opt-card">${propKvRowsHtml(sec.pairs)}</div></div>`).join('');
}

function setupPropertiesEventListeners() {
  el.propModalClose.addEventListener('click', closePropertiesModal);
  el.propCloseBtn.addEventListener('click', closePropertiesModal);

  propModalDrag = makeModalDraggable(
    el.propModal.querySelector('.modal-container'),
    el.propModal.querySelector('.modal-header'));

  el.propModal.addEventListener('click', (e) => {
    if (propModalDrag && propModalDrag.consumeDrag()) return; // drag, not a click
    if (e.target === el.propModal) closePropertiesModal();
  });

  el.propCopyBtn.addEventListener('click', async () => {
    const dl = propCurrentDl;
    let text = dl
      ? `Name: ${dl.name}\nURL: ${dl.url}\nSave Path: ${dl.path || '(not set)'}\n` +
        `Size: ${dl.size}\nStatus: ${capitalize(dl.status)}\nLast Modified: ${dl.lastModified || '--'}\n`
      : '';
    if (propMediaInfoRaw) text += '\n' + propMediaInfoRaw;
    try {
      await navigator.clipboard.writeText(text);
      showStatusToast('Details copied to clipboard.');
    } catch (e) {
      showStatusToast('Could not access the clipboard.');
    }
  });
}

// ---- Download Detail Modal (double-click a downloading row) ----
let ddCurrent = null;        // download shown in the modal
let ddTimer = null;          // 500ms refresh interval
let ddModalDrag = null;
let ddLastSegHtml = '';      // avoid re-rendering identical DOM every tick
let ddLastTblHtml = '';
let ddResume = null;         // last known resume-support flag

function ddKv(key, val, cls) {
  return `<div class="dd-kv"><span class="dd-kv-key">${escapeHtml(key)}</span>` +
         `<span class="dd-kv-val${cls ? ' ' + cls : ''}">${escapeHtml(val)}</span></div>`;
}

function renderDdInfo(dl) {
  const resumeTxt = ddResume === null ? '--' : (ddResume ? 'Yes' : 'No');
  el.ddInfoCard.innerHTML =
    ddKv('Name:', dl.name) +
    ddKv('Status:', capitalize(dl.status)) +
    ddKv('Size:', dl.total || dl.size || '--') +
    ddKv('Downloaded:', dl.downloaded || '--') +
    ddKv('Speed:', dl.speed || '--') +
    ddKv('Remaining Time:', dl.timeLeft || '--') +
    ddKv('Resume Support:', resumeTxt, ddResume ? 'yes' : '');
  el.ddProgress.style.width = (dl.pct || 0) + '%';
}

function renderDdSettings(dl) {
  el.ddSettCard.innerHTML =
    ddKv('URL:', dl.url) +
    ddKv('Save Path:', dl.path || '(default folder)') +
    ddKv('Connections:', String(optionsCache.connections)) +
    ddKv('Speed Limit:', optionsCache.speedLimitEnabled
      ? optionsCache.speedLimit + ' KB/s' : 'Off');
}

// Render the chunk/part segment strip. Large chunk maps are downsampled to
// at most 96 cells to keep the DOM light in the embedded webview.
function renderDdSegments(segments) {
  const MAX_CELLS = 96;
  let cells = segments;
  if (segments.length > MAX_CELLS) {
    cells = [];
    const per = segments.length / MAX_CELLS;
    for (let c = 0; c < MAX_CELLS; c++) {
      const from = Math.floor(c * per);
      const to = Math.min(segments.length, Math.floor((c + 1) * per) || from + 1);
      let allDone = true, anyActive = false, anyDone = false;
      for (let i = from; i < to; i++) {
        if (segments[i] === 2) anyDone = true; else allDone = false;
        if (segments[i] === 1) anyActive = true;
      }
      cells.push(allDone ? 2 : (anyActive || anyDone ? 1 : 0));
    }
  }
  const html = cells.map(s =>
    `<div class="dd-seg ${s === 2 ? 'done' : s === 1 ? 'active' : 'pending'}"></div>`
  ).join('');
  if (html !== ddLastSegHtml) {
    ddLastSegHtml = html;
    el.ddSegments.innerHTML = html;
  }
}

function renderDdParts(parts) {
  const html = parts.map(p =>
    `<tr><td>${p.i}</td><td>${escapeHtml(p.status)}</td>` +
    `<td>${formatBytes(p.done || 0)}</td><td>${formatBytes(p.total || 0)}</td></tr>`
  ).join('');
  if (html !== ddLastTblHtml) {
    ddLastTblHtml = html;
    el.ddPartTbody.innerHTML = html;
  }
}

// Pause button states for the download-detail modal: two bars while the
// download runs, a circled-pause "Paused" badge once it is stopped.
const DD_ICON_PAUSE =
  '<svg viewBox="0 0 24 24" width="13" height="13" fill="currentColor" stroke="none">' +
  '<rect x="6" y="4" width="4" height="16" rx="1"></rect>' +
  '<rect x="14" y="4" width="4" height="16" rx="1"></rect></svg>';
const DD_ICON_PAUSED =
  '<svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" ' +
  'stroke-width="2" stroke-linecap="round">' +
  '<circle cx="12" cy="12" r="9"></circle>' +
  '<line x1="10" y1="9" x2="10" y2="15"></line>' +
  '<line x1="14" y1="9" x2="14" y2="15"></line></svg>';
let ddPauseState = '';

// Keep the button in sync with the shown download: Pause (active) while
// downloading, Paused (click to RESUME) after a stop, disabled otherwise
// (finished/failed rows keep the modal open but there is nothing to pause).
function syncDdPauseBtn(dl) {
  const st = dl ? dl.status : '';
  const mode = st === 'downloading' ? 'pause'
             : st === 'stopped'     ? 'paused' : 'idle';
  if (mode === ddPauseState) return;
  ddPauseState = mode;
  el.ddPause.innerHTML = (mode === 'paused' ? DD_ICON_PAUSED : DD_ICON_PAUSE) +
    '<span>' + (mode === 'paused' ? 'Paused' : 'Pause') + '</span>';
  el.ddPause.disabled = mode === 'idle';
  el.ddPause.title = mode === 'paused' ? 'Click to resume' : '';
  el.ddPause.classList.toggle('paused', mode === 'paused');
}

function ddTick() {
  const dl = ddCurrent;
  if (!dl) return;

  renderDdInfo(dl);
  syncDdPauseBtn(dl);   // status can change externally (resume/finish/retry)

  // Per-part snapshot from the engine (only meaningful while downloading).
  if (dl.status === 'downloading' && typeof GetPartInfo === 'function') {
    let info = null;
    try { info = JSON.parse(GetPartInfo(dl.id) || '{}'); } catch (e) { info = null; }
    if (info && info.parts) {
      ddResume = !!info.resume;
      if (!el.ddPartInfo.classList.contains('collapsed')) {
        renderDdSegments(info.segments || []);
        renderDdParts(info.parts);
      }
    }
  }
}

function openDownloadDetail(dl) {
  ddCurrent = dl;
  ddResume = null;
  ddLastSegHtml = '';
  ddLastTblHtml = '';
  ddPauseState = '';                 // force a fresh Pause/Paused render
  el.ddTitle.textContent = Math.round(dl.pct || 0) + '%-' + dl.name;

  renderDdInfo(dl);
  renderDdSettings(dl);
  el.ddSegments.innerHTML = '';
  el.ddPartTbody.innerHTML = '';

  if (ddModalDrag) ddModalDrag.reset();
  el.ddModal.style.display = 'flex';

  ddTick();
  if (ddTimer) clearInterval(ddTimer);
  ddTimer = setInterval(() => {
    if (ddCurrent) {
      el.ddTitle.textContent = Math.round(ddCurrent.pct || 0) + '%-' + ddCurrent.name;
      ddTick();
    }
  }, 500);
}

function closeDownloadDetail() {
  if (ddTimer) { clearInterval(ddTimer); ddTimer = null; }
  ddCurrent = null;
  el.ddModal.style.display = 'none';
}

function setupDownloadDetailListeners() {
  el.ddCloseX.addEventListener('click', closeDownloadDetail);
  el.ddClose.addEventListener('click', closeDownloadDetail);
  // Minimize behaves like hide: the download keeps running in the list.
  el.ddMinimize.addEventListener('click', closeDownloadDetail);

  ddModalDrag = makeModalDraggable(
    el.ddModal.querySelector('.modal-container'),
    el.ddModal.querySelector('.dd-header'));

  el.ddModal.addEventListener('click', (e) => {
    if (ddModalDrag && ddModalDrag.consumeDrag()) return;
    if (e.target === el.ddModal) closeDownloadDetail();
  });

  // Tabs
  el.ddModal.querySelectorAll('.dd-tab').forEach(tab => {
    tab.addEventListener('click', () => {
      el.ddModal.querySelectorAll('.dd-tab').forEach(t => t.classList.remove('active'));
      tab.classList.add('active');
      const which = tab.getAttribute('data-tab');
      el.ddPaneInfo.style.display = which === 'info' ? '' : 'none';
      el.ddPaneSett.style.display = which === 'settings' ? '' : 'none';
    });
  });

  // Collapsible Part Info
  el.ddPartToggle.addEventListener('click', () => {
    el.ddPartInfo.classList.toggle('collapsed');
    el.ddPartToggle.classList.toggle('collapsed');
    ddLastSegHtml = '';
    ddLastTblHtml = '';
    ddTick();
  });

  // Pause/Resume toggle for the shown download. Downloading -> stop (same
  // semantics as the toolbar Stop) and flip to "Paused"; clicking the
  // "Paused" state resumes from the kept partial file (chunk bitmap).
  el.ddPause.addEventListener('click', () => {
    const dl = ddCurrent;
    if (!dl) return;
    if (dl.status === 'downloading') {
      cancelRetry(dl);
      dl._retryCount = 0;
      dl.status = 'stopped';
      dl.timeLeft = '--';
      dl.speed = '';
      if (typeof StopDownload === 'function') StopDownload();
    } else if (dl.status === 'stopped') {
      cancelRetry(dl);
      dl._retryCount = 0;
      dl.status = 'downloading';
      dl.timeLeft = 'Resuming...';
      requestStart(dl);           // queues behind any active job
    } else {
      return;                     // finished/failed: nothing to toggle
    }
    saveData();
    renderTable();
    updateToolbarState();
    renderDdInfo(dl);
    syncDdPauseBtn(dl);
  });
}

function setupOptionsEventListeners() {
  el.optModalClose.addEventListener('click', closeOptionsModal);
  el.optCancelBtn.addEventListener('click', closeOptionsModal);

  // Drag the Options modal around by its header.
  optModalDrag = makeModalDraggable(
    el.optModal.querySelector('.modal-container'),
    el.optModal.querySelector('.modal-header'));

  el.optModal.addEventListener('click', (e) => {
    if (optModalDrag && optModalDrag.consumeDrag()) return; // drag, not a click
    if (e.target === el.optModal) closeOptionsModal();
  });

  el.optSaveBtn.addEventListener('click', () => {
    saveOptions();
    closeOptionsModal();
  });

  // Range slider live value
  el.optConnections.addEventListener('input', () => {
    el.optConnVal.textContent = el.optConnections.value;
  });

  // Browse button for download path
  el.optBrowseBtn.addEventListener('click', () => {
    if (typeof PickFolder === 'function') {
      const chosen = PickFolder();
      if (chosen) {
        el.optPathInput.value = chosen;
      }
    } else if (typeof PickSavePath === 'function') {
      const chosen = PickSavePath('folder');
      if (chosen) {
        const idx = Math.max(chosen.lastIndexOf('/'), chosen.lastIndexOf('\\'));
        el.optPathInput.value = idx !== -1 ? chosen.substring(0, idx) : chosen;
      }
    } else {
      el.optPathInput.value = 'C:\\Downloads\\DownloadManager';
    }
  });

  // Speed limit toggle switch. A plain div (clicks on it work reliably in
  // Ultralight, unlike styled checkbox inputs); its .on class is the single
  // source of truth for the enabled state.
  el.optLimitToggle.addEventListener('click', () => {
    const on = el.optLimitToggle.classList.toggle('on');
    el.optLimitSpeed.disabled = !on;
    if (!on) el.optLimitSpeed.value = '';
  });

  // Custom dropdown toggle
  const trigger = el.optThreshold.querySelector('.custom-select-trigger');
  const options = el.optThreshold.querySelector('.custom-select-options');

  trigger.addEventListener('click', (e) => {
    e.stopPropagation();
    const isOpen = el.optThreshold.classList.toggle('open');
    if (isOpen) {
      // Auto-detect if dropdown should open upward
      const opts = el.optThreshold.querySelector('.custom-select-options');
      const rect = opts.getBoundingClientRect();
      const spaceBelow = window.innerHeight - rect.top;
      if (spaceBelow < rect.height) {
        el.optThreshold.classList.add('dropup');
      } else {
        el.optThreshold.classList.remove('dropup');
      }
    }
  });

  // Close dropdown when clicking outside
  document.addEventListener('click', (e) => {
    if (!el.optThreshold.contains(e.target)) {
      el.optThreshold.classList.remove('open');
    }
  });

  // Option selection
  options.addEventListener('click', (e) => {
    const opt = e.target.closest('.custom-select-option');
    if (!opt) return;
    const value = opt.getAttribute('data-value');
    const label = opt.textContent;
    el.optThreshold.querySelector('.custom-select-value').textContent = label;
    options.querySelectorAll('.custom-select-option').forEach(o => o.classList.remove('selected'));
    opt.classList.add('selected');
    el.optThreshold.classList.remove('open');
  });
}

// Custom select helpers
function setCustomSelectValue(container, value) {
  const opt = container.querySelector(`.custom-select-option[data-value="${value}"]`);
  if (opt) {
    container.querySelector('.custom-select-value').textContent = opt.textContent;
    container.querySelectorAll('.custom-select-option').forEach(o => o.classList.remove('selected'));
    opt.classList.add('selected');
  }
}

function getCustomSelectValue(container) {
  const selected = container.querySelector('.custom-select-option.selected');
  return selected ? selected.getAttribute('data-value') : '10485760';
}

// Open the downloaded file with the OS default application.
// Hands the file to Windows' own "Open with" chooser. Same guards as
// openDownloadedFile: the file has to exist before anything can open it.
function openDownloadedFileWith(dl) {
  if (!dl) return;
  if (dl.status !== 'finished') {
    showStatusToast('Cannot open. Download is not completed.');
    return;
  }
  if (!dl.path) {
    showStatusToast('File path is not available.');
    return;
  }
  if (typeof OpenWith !== 'function') {
    showStatusToast('Open with bridge is not available.');
    return;
  }
  OpenWith(dl.path);
}

function openDownloadedFile(dl) {
  if (!dl) return;
  if (dl.status !== 'finished') {
    showStatusToast('Cannot open. Download is not completed.');
    return;
  }
  if (!dl.path) {
    showStatusToast('File path is not available.');
    return;
  }
  if (typeof OpenFile !== 'function') {
    showStatusToast('Open file bridge is not available.');
    return;
  }
  const ok = OpenFile(dl.path);
  if (!ok) {
    showStatusToast('Could not open file.');
  }
}

// Open the folder containing the downloaded file and highlight it.
function openContainingFolder(dl) {
  if (!dl || !dl.path) {
    showStatusToast('File path is not available.');
    return;
  }
  if (typeof OpenFolder !== 'function') {
    showStatusToast('Open folder bridge is not available.');
    return;
  }
  const ok = OpenFolder(dl.path);
  if (!ok) {
    showStatusToast('Could not open folder.');
  }
}


// Context Menu Action Logic
async function handleContextMenuAction(action) {
  const selected = downloads.find(dl => dl.id === selectedId);
  if (!selected) return;

  switch (action) {
    case 'open':
      openDownloadedFile(selected);
      break;
    case 'open-with':
      openDownloadedFileWith(selected);
      break;
    case 'open-folder':
      openContainingFolder(selected);
      break;
    case 'rename': {
      const newName = await uiPrompt('Move / Rename', 'Enter new filename:', selected.name);
      if (newName && newName.trim()) {
        selected.name = newName.trim();
        saveData();
        renderTable();
      }
      break;
    }
    case 'redownload':
      selected.pct = 0;
      selected.downloaded = '0 B';
      selected.status = 'downloading';
      selected.timeLeft = 'Connecting...';
      saveData();
      renderTable();
      requestStart(selected);
      break;
    case 'resume':
      resumeSelected();
      break;
    case 'stop':
      stopSelected();
      break;
    case 'refresh-address': {
      const newUrl = await uiPrompt('Refresh Download Address', 'Refresh download URL link:', selected.url);
      if (newUrl && newUrl.trim()) {
        selected.url = newUrl.trim();
        saveData();
        showStatusToast('Download address refreshed successfully.');
      }
      break;
    }
    case 'add-queue':
      showStatusToast(`Added ${selected.name} to Download Queue.`);
      break;
    case 'delete':
      // The context menu always acts on the right-clicked row only — never
      // the checkbox batch (deleteRows bypasses checked-rows precedence).
      deleteRows([selected], false);
      break;
    case 'delete-from-disk': {
      // Deleting a real file is destructive, so confirm — same protection the
      // toolbar delete path has (previously this menu item deleted instantly).
      const ok = await uiConfirmDanger(
        'Delete From Disk',
        `Permanently delete "${selected.name}"?\nThe file will be moved to the Recycle Bin.`,
        'Delete from Disk'
      );
      if (ok) deleteRows([selected], true);
      break;
    }
    case 'properties':
      openPropertiesModal(selected);
      break;
  }
}

// Trigger start download from modal input
function startNewDownload() {
  const url = el.urlInput.value.trim();
  if (!url) {
    uiAlert('Add New Download', 'Please enter a valid download link URL.');
    return;
  }

  // Derive name: the probe's Content-Disposition filename beats the URL
  // basename (CDN links are hashed noise); an extension-provided filename
  // still wins on the native side via the request context.
  let name = url.substring(url.lastIndexOf('/') + 1).split('?')[0] || 'download.bin';
  if (lastProbe && lastProbe.forUrl === url && lastProbe.filename &&
      !(pendingContext && pendingContext.filename)) {
    name = lastProbe.filename;
  }
  if (!name.includes('.')) name += '.bin';
  // A watch page has no file name at all ("watch" -> "watch.bin"). The native
  // side replaces this with the video title through UI.onPath as soon as
  // yt-dlp has resolved the page; until then, say what is happening.
  if (isYouTubeUrl(url)) name = 'YouTube video…';
  
  // The Options default folder is a DIRECTORY by definition — mark it with
  // a trailing separator so the native side still treats it as one (and
  // recreates it) even when the user deleted the folder from disk.
  let savePath = el.pathInput.value.trim();
  if (!savePath && optionsCache.downloadPath) {
    savePath = optionsCache.downloadPath.replace(/[\\/]+$/, '') + '\\';
  }
  
  const newDl = {
    id: 'dl_' + Date.now(),
    name: name,
    url: url,
    path: savePath,
    size: 'Calculating...',
    status: 'downloading',
    pct: 0,
    downloaded: '0 B',
    total: '0 B',
    speed: '',
    timeLeft: 'Connecting...',
    lastModified: new Date().toLocaleString()
  };

  // Attach a browser-captured request context (cookies/referrer/UA/filename),
  // if one was delivered by the extension. One-shot: consumed here.
  if (pendingContext) {
    newDl.ctx = pendingContext;
    pendingContext = null;
  }

  downloads.unshift(newDl);
  selectedId = newDl.id;
  
  saveData();
  closeModal();
  renderTable();
  updateToolbarState();
  
  // Start (or queue behind any active job). Events route back by id.
  requestStart(newDl);
}

// Action button: resume selected
function resumeSelected() {
  const selected = downloads.find(dl => dl.id === selectedId);
  if (!selected || selected.status === 'downloading' || selected.status === 'finished') return;

  // Cancel any pending auto-retry and reset backoff (manual resume = fresh start)
  cancelRetry(selected);
  selected._retryCount = 0;

  selected.status = 'downloading';
  selected.timeLeft = 'Resuming...';
  saveData();
  renderTable();
  updateToolbarState();

  // Start (or queue behind any active job). Events route back by id.
  requestStart(selected);
}

// Action button: stop selected
function stopSelected() {
  const selected = downloads.find(dl => dl.id === selectedId);
  if (!selected) return;

  // Cancel any pending auto-retry
  cancelRetry(selected);
  selected._retryCount = 0;

  // Allow stopping a failed download (cancels its auto-retry)
  if (selected.status !== 'downloading' && selected.status !== 'failed') return;

  selected.status = 'stopped';
  selected.timeLeft = '--';
  selected.speed = '';
  saveData();
  renderTable();
  updateToolbarState();
  
  if (typeof StopDownload === 'function') {
    StopDownload();
  }
}

// Action button: stop all downloads
function stopAllDownloads() {
  downloads.forEach(dl => {
    // Cancel any pending auto-retries
    cancelRetry(dl);
    dl._retryCount = 0;

    if (dl.status === 'downloading') {
      dl.status = 'stopped';
      dl.timeLeft = '--';
      dl.speed = '';
    }
  });
  saveData();
  renderTable();
  updateToolbarState();
  
  if (typeof StopDownload === 'function') {
    StopDownload();
  }
}

// Delete targets: checked rows take precedence (batch delete); otherwise the
// highlighted row. Shared by the toolbar Trash button and deleteSelected().
function getDeleteTargets() {
  const checked = downloads.filter(dl => dl.checked);
  if (checked.length > 0) return checked;
  const selected = downloads.find(dl => dl.id === selectedId);
  return selected ? [selected] : [];
}

// Core removal: deletes the given rows, optionally their files from disk.
// fromDisk=true also deletes the file(s) on disk via the native bridge.
function deleteRows(targets, fromDisk) {
  if (!targets || targets.length === 0) return;

  // Stop an in-progress download before removing its entry.
  if (targets.some(dl => dl.status === 'downloading')) {
    if (typeof StopDownload === 'function') StopDownload();
  }

  // Optionally delete the file(s) from disk. We only attempt this for
  // finished downloads with a known path; in-progress/errored entries have
  // no useful file to delete.
  const diskTargets = fromDisk
    ? targets.filter(dl => dl.path && dl.status === 'finished')
    : [];
  if (fromDisk && diskTargets.length > 0) {
    if (typeof DeleteFile !== 'function') {
      showStatusToast('Delete file bridge is not available.');
      // Still remove from list — the file(s) just stay on disk.
    } else {
      let failed = 0;
      diskTargets.forEach(dl => {
        let ok = false;
        try { ok = DeleteFile(dl.path); } catch (e) { ok = false; }
        if (!ok) failed++;
      });
      if (failed > 0) {
        showStatusToast('Could not delete file(s); removed from list only.');
      } else {
        showStatusToast(targets.length === 1
          ? 'File deleted and removed from list.'
          : `${targets.length} files deleted and removed from list.`);
      }
    }
  } else {
    showStatusToast(targets.length === 1
      ? 'Download removed from list.'
      : `${targets.length} downloads removed from list.`);
  }

  const ids = new Set(targets.map(dl => dl.id));
  downloads = downloads.filter(dl => !ids.has(dl.id));
  selectedId = null;

  saveData();
  renderTable();
  updateToolbarState();
}

// Action button: delete the checked download(s), or the highlighted row when
// nothing is checked. fromDisk=true also deletes file(s) via the native bridge.
function deleteSelected(fromDisk) {
  deleteRows(getDeleteTargets(), fromDisk);
}

// Mock simulation helper when previewing outside C++ application
let mockInterval = null;
function simulateMockDownload(id) {
  if (mockInterval) clearInterval(mockInterval);
  
  let currentPct = 0;
  mockInterval = setInterval(() => {
    const dl = downloads.find(x => x.id === id);
    if (!dl || dl.status !== 'downloading') {
      clearInterval(mockInterval);
      return;
    }
    
    currentPct += 5;
    if (currentPct >= 100) {
      currentPct = 100;
      clearInterval(mockInterval);
      
      // Complete
      UI.onComplete(id, 'ok', dl.path || 'C:\\Downloads\\DownloadManager\\' + dl.name, '');
    } else {
      // Progress
      const downloadedBytes = (currentPct / 100) * 1024 * 1024 * 150; // 150 MB total
      const speedBps = 1024 * 1024 * 3.5; // 3.5 MB/s

      UI.onProgress(
        id,
        currentPct,
        formatBytes(downloadedBytes),
        formatBytes(1024 * 1024 * 150),
        8,
        formatBytes(speedBps) + '/s',
        downloadedBytes,
        1024 * 1024 * 150,
        speedBps
      );
    }
  }, 500);
}

function formatBytes(bytes) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}


// ---- Native C++ Communication Bridges (Wire UI objects) ----
window.UI = {
  // 1. Progress Event — routed to the exact row by id (never by status).
  // downloadedBytes/totalBytes/speedBps are the RAW byte counts from the
  // engine; the ETA must be computed from these, never from the humanized
  // strings (whose units are chosen independently per value — mixing their
  // parseFloat() results produced a garbage ETA).
  onProgress(id, pct, downloaded, total, activeParts, speed,
             downloadedBytes, totalBytes, speedBps) {
    const dl = (id && downloads.find(d => d.id === id)) ||
               downloads.find(d => d.status === 'downloading');

    if (dl) {
      if (dl.status !== 'downloading') dl.status = 'downloading';
      dl.pct = pct;
      dl.downloaded = downloaded;
      dl.total = total;
      dl.size = total;
      dl.speed = speed;

      // Calculate simple time left from raw byte counts.
      let seconds = NaN;
      if (Number.isFinite(totalBytes) && Number.isFinite(downloadedBytes) &&
          Number.isFinite(speedBps) &&
          totalBytes > 0 && downloadedBytes < totalBytes && speedBps > 0) {
        seconds = (totalBytes - downloadedBytes) / speedBps;
      } else if (pct > 0 && pct < 100 && speed) {
        // Fallback for callers that don't pass raw bytes (mock preview):
        // only valid when all three humanized values share the same unit.
        const speedNum = parseFloat(speed);
        const downloadedNum = parseFloat(downloaded);
        const totalNum = parseFloat(total);
        const unitOf = s => (s.match(/[A-Za-z]+/) || [''])[0];
        if (!isNaN(speedNum) && speedNum > 0 && !isNaN(downloadedNum) &&
            !isNaN(totalNum) &&
            unitOf(speed) === unitOf(downloaded) && unitOf(downloaded) === unitOf(total)) {
          seconds = (totalNum - downloadedNum) / speedNum;
        }
      }
      if (Number.isFinite(seconds) && seconds > 0) {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        const s = Math.floor(seconds % 60);
        dl.timeLeft = `${h > 0 ? h + 'h ' : ''}${m > 0 ? m + 'm ' : ''}${s}s`;
      } else if (pct <= 0 || pct >= 100 || !speed) {
        dl.timeLeft = 'Calculating...';
      }

      // Patch just this row's progress bar; only rebuild the whole table if the
      // row is present but not yet showing the downloading layout.
      if (!patchActiveRow(dl)) {
        if (el.tableBody.querySelector(`tr[data-id="${dl.id}"]`)) renderTable();
      }
      throttledSave();
    }

    // Status Bar speed gauges
    el.dlSpeed.textContent = speed || '0 B/s';
    el.statusMsg.textContent = 'Downloading...';
  },

  // 2. Complete Event — routed to the exact row by id.
  onComplete(id, kind, filePath, errorMsg) {
    // Route by id ONLY. The old fallback ("first row that is downloading")
    // could mark an unrelated row finished and overwrite its path when an
    // event arrived without a usable id.
    const dl = id ? downloads.find(d => d.id === id) : null;

    if (dl) {
      if (kind === 'ok') {
        dl.status = 'finished';
        dl.pct = 100;
        dl.timeLeft = '0s';
        dl.speed = '';
        dl._retryCount = 0; // success resets retry backoff
        if (filePath) {
          dl.path = filePath;
          // Strip out folder path to keep filename
          const parts = filePath.split(/[/\\]/);
          dl.name = parts[parts.length - 1];
        }
        dl.lastModified = new Date().toLocaleString();

        showStatusToast('Download completed successfully.');
      } else if (kind === 'stopped') {
        dl.status = 'stopped';
        dl.timeLeft = '--';
        dl.speed = '';
        if (filePath) dl.path = filePath;

        showStatusToast('Download stopped.');
      } else {
        dl.status = 'failed';
        dl.timeLeft = '--';
        dl.speed = '';

        showStatusToast('Download failed: ' + (errorMsg || 'Unknown error'));

        // Auto-retry with exponential backoff
        scheduleRetry(dl);
      }

      saveData();
      renderTable();
      updateToolbarState();
    }

    el.dlSpeed.textContent = '0 B/s';
    updateDiskWidget();

    // The engine is now free: launch any download that was queued behind this
    // one (the Stop->Start race fix).
    // Take the first queued job whose row still wants to run: rows the user
    // removed or stopped meanwhile are dropped, and the queue keeps its order.
    while (pendingStarts.length) {
      const next = pendingStarts.shift();
      const ndl = downloads.find(d => d.id === next.id);
      if (!ndl || ndl.status !== 'downloading') continue;
      if (typeof StartDownload === 'function') {
        StartDownload(next.url, next.path, next.id,
                      next.ctx ? JSON.stringify(next.ctx) : '');
      } else {
        simulateMockDownload(next.id);
      }
      break;
    }
  },

  // 3. Path Resolved — the engine finalized the output path (e.g. added an
  // extension); persist it now so a later resume finds the partial file.
  onPath(id, path) {
    if (!id || !path) return;
    const dl = downloads.find(d => d.id === id);
    if (!dl) return;
    dl.path = path;
    const parts = path.split(/[/\\]/);
    dl.name = parts[parts.length - 1] || dl.name;
    saveData();
    renderTable();
  },

  // 4. Status Toast Notification
  onStatus(msg) {
    showStatusToast(msg);
  },

  // 4b. Media analysis result for the Properties modal. requestId is the
  // download id passed to RequestMediaInfo; ignore stale replies for rows
  // other than the one currently shown.
  onMediaInfo(requestId, text) {
    if (!propCurrentDl || propCurrentDl.id !== requestId) return;
    propMediaInfoRaw = text || '';
    if (!propMediaInfoRaw) {
      el.propMediaSects.innerHTML =
        '<div class="prop-note">Media analysis is available once the file exists on disk.</div>';
      return;
    }
    renderMediaInfoSections(propMediaInfoRaw);
  },

  // 4c. URL probe result for the Add-Download modal. Sequence id guards
  // against stale replies (user re-typed the URL mid-flight).
  onUrlProbe(requestId, json) {
    if (requestId !== currentProbeId) return;
    let info = null;
    try { info = JSON.parse(json); } catch (e) { info = null; }
    renderUrlProbe(info);
  },

  // 5. External Download — the browser extension captured a download and the
  // native bridge handed it over. Open the Add-URL modal prefilled; the user
  // just presses "Download Now". The request context (cookies/referrer/UA/
  // filename/extra headers) is kept in memory only and consumed by
  // startNewDownload(). `headersJson` is a JSON object string of extra
  // request headers captured by the extension (may be empty).
  onExternalDownload(url, filename, referrer, cookies, userAgent, headersJson,
                     type, audioUrl, height) {
    if (!url) return;
    let headers = {};
    if (headersJson) {
      try { headers = JSON.parse(headersJson) || {}; } catch (e) { headers = {}; }
    }
    pendingContext = {
      filename: filename || '',
      referrer: referrer || '',
      cookies: cookies || '',
      userAgent: userAgent || '',
      headers: headers,
      // "" (plain HTTP) | "hls" | "dash" | "ytdlp" — routes the engine
      type: type || '',
      // Paired-track DASH only: the audio rung that belongs to `url`. The
      // native side downloads both and muxes them into one file.
      audioUrl: audioUrl || '',
      // "ytdlp" only: the quality picked in the page panel, as a video height.
      // Empty means best available.
      height: height || ''
    };
    openModal();              // also clears url/path inputs
    el.urlInput.value = url;
    // pathInput stays empty on purpose: the native side saves to the default
    // folder and uses `filename` from the context for the final name.
    requestUrlProbe();        // show type/size before the user commits
    showStatusToast(type === 'ytdlp'
      ? 'YouTube video captured. Press "Download Now" to start.'
      : 'Download captured from browser. Press "Download Now" to start.');
  }
};

// Initialize App
init();
