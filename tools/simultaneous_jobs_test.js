// Simultaneous jobs and per-job pause/stop.
//
// Asserts that:
//   1. Up to `optionsCache.maxConcurrent` downloads start simultaneously in
//      parallel without queueing behind the first job.
//   2. Any download started beyond `maxConcurrent` waits in `pendingStarts`
//      with `status === 'queued'`.
//   3. `pauseDownload(dl)` stops ONLY that job (`StopDownload(dl.id)`), leaves
//      other active downloads running, and immediately promotes the next
//      queued download into the freed slot.
//   4. `stopAllDownloads()` stops all running jobs (`StopDownload()`) and
//      clears the queue.
//
// Usage: node tools/simultaneous_jobs_test.js (wired into ctest as `simultaneous_jobs`)
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

const APP_JS = path.join(__dirname, '..', 'assets', 'app.js');
const source = fs.readFileSync(APP_JS, 'utf8');

let failures = 0;
function check(what, ok, detail) {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${what}${detail ? '  (' + detail + ')' : ''}`);
  if (!ok) failures++;
}

function extractBetween(beginMarker, endMarker) {
  const from = source.indexOf(beginMarker);
  const to = source.indexOf(endMarker, from >= 0 ? from : 0);
  if (from < 0 || to < 0 || to <= from) {
    console.error('Could not find block in assets/app.js:', beginMarker, '..', endMarker);
    process.exit(1);
  }
  return source.slice(from, to);
}

const queueBlock = extractBetween(
  '// Kick off (or queue) a download for the given row.',
  '// Patch only an active row\'s progress bar/labels'
);
const pauseBlock = extractBetween(
  '// Pause/stop a single download by id without disturbing other active jobs.',
  '// Delete targets: checked rows take precedence'
);

function makeHarness(maxConcurrent = 3) {
  const started = [];
  const stopped = [];
  const downloads = [];
  const pendingStarts = [];
  const ctx = vm.createContext({
    Math, Number, JSON,
    parseInt,
    clearInterval: () => {},
    downloads,
    pendingStarts,
    optionsCache: { maxConcurrent },
    StartDownload: (url, savePath, id) => { started.push(id); },
    StopDownload: (id) => { stopped.push(id === undefined ? '<ALL>' : id); },
    cancelRetry: () => {},
    updateTotalSpeed: () => {},
    getDeleteTargets: () => [],
    saveData: () => {},
    renderTable: () => {},
    updateToolbarState: () => {},
    simulateMockDownload: (id) => { started.push('mock:' + id); }
  });
  vm.runInContext(queueBlock + '\n' + pauseBlock, ctx);
  return { ctx, downloads, pendingStarts, started, stopped };
}

console.log('simultaneous downloads start in parallel up to maxConcurrent');
{
  const h = makeHarness(3);
  for (let i = 1; i <= 5; i++) {
    const dl = { id: 'dl_' + i, url: 'https://example.com/' + i, path: 'C:\\' + i };
    h.downloads.push(dl);
    h.ctx.requestStart(dl);
  }

  check('first 3 jobs start immediately',
        h.started.length === 3 &&
        h.started[0] === 'dl_1' && h.started[1] === 'dl_2' && h.started[2] === 'dl_3',
        JSON.stringify(h.started));
  check('first 3 rows have status "downloading"',
        h.downloads[0].status === 'downloading' &&
        h.downloads[1].status === 'downloading' &&
        h.downloads[2].status === 'downloading');
  check('jobs 4 and 5 are queued in FIFO order',
        h.downloads[3].status === 'queued' &&
        h.downloads[4].status === 'queued' &&
        h.pendingStarts.length === 2,
        'queue length=' + h.pendingStarts.length);
}

console.log('pausing one active job only stops that job and promotes the next queued job');
{
  const h = makeHarness(3);
  for (let i = 1; i <= 5; i++) {
    const dl = { id: 'dl_' + i, url: 'https://example.com/' + i, path: 'C:\\' + i };
    h.downloads.push(dl);
    h.ctx.requestStart(dl);
  }

  // Pause job 2 while 1 and 3 are running and 4 and 5 are queued.
  const ok = h.ctx.pauseDownload(h.downloads[1]);
  check('pauseDownload returns true for an active job', ok === true);
  check('StopDownload is called with ONLY that job\'s id ("dl_2")',
        h.stopped.length === 1 && h.stopped[0] === 'dl_2',
        JSON.stringify(h.stopped));
  check('paused job is now "stopped"', h.downloads[1].status === 'stopped');
  check('other active jobs ("dl_1", "dl_3") keep running undisturbed',
        h.downloads[0].status === 'downloading' &&
        h.downloads[2].status === 'downloading');
  check('queued job "dl_4" immediately starts in the freed slot',
        h.downloads[3].status === 'downloading' &&
        h.started.includes('dl_4'),
        JSON.stringify(h.started));
  check('job "dl_5" remains queued waiting for the next slot',
        h.downloads[4].status === 'queued' && h.pendingStarts.length === 1);
}

console.log('pausing a queued job drops it from the queue without calling native StopDownload');
{
  const h = makeHarness(2);
  for (let i = 1; i <= 3; i++) {
    const dl = { id: 'dl_' + i, url: 'https://example.com/' + i, path: 'C:\\' + i };
    h.downloads.push(dl);
    h.ctx.requestStart(dl);
  }
  // dl_3 is queued
  h.ctx.pauseDownload(h.downloads[2]);
  check('queued job becomes stopped', h.downloads[2].status === 'stopped');
  check('native StopDownload is NOT called for a queued job', h.stopped.length === 0);
  check('queue is now empty', h.pendingStarts.length === 0);
}

console.log('stopAllDownloads stops every active job and clears the queue');
{
  const h = makeHarness(2);
  for (let i = 1; i <= 4; i++) {
    const dl = { id: 'dl_' + i, url: 'https://example.com/' + i, path: 'C:\\' + i };
    h.downloads.push(dl);
    h.ctx.requestStart(dl);
  }
  h.ctx.stopAllDownloads();
  check('all rows are now stopped',
        h.downloads.every(d => d.status === 'stopped'));
  check('StopDownload() is called with no id to stop all native jobs',
        h.stopped.length === 1 && h.stopped[0] === '<ALL>',
        JSON.stringify(h.stopped));
}

console.log(`\n${failures ? 'FAILED' : 'ALL CHECKS PASSED'} (${failures} failing checks)`);
process.exit(failures ? 1 : 0);
