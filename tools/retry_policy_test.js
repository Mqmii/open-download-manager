// Auto-retry must give up eventually.
//
// A failed row re-launches itself on a backoff. The backoff DELAY was capped
// at 60s but the attempt COUNT was not, so a row whose failure is permanent
// (404, expired signed URL, dead host) re-requested the same URL once a minute
// for as long as the app stayed open. Together with the DASH/yt-dlp engines
// reporting a cancel as a failure, pressing Stop started an unkillable loop.
//
// The auto-retry block is lifted straight out of assets/app.js and run against
// stubbed collaborators and a fake clock, so the numbers asserted here are the
// ones the app actually ships.
//
// Usage: node tools/retry_policy_test.js   (also wired into ctest as `retry_policy`)
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

const APP_JS = path.join(__dirname, '..', 'assets', 'app.js');
const BEGIN = '// ---- Auto-Retry Helpers ----';
const END = '// Kick off (or queue) a download for the given row.';

let failures = 0;
function check(what, ok, detail) {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${what}${detail ? '  (' + detail + ')' : ''}`);
  if (!ok) failures++;
}

// --- lift the block ---------------------------------------------------------
const source = fs.readFileSync(APP_JS, 'utf8');
const from = source.indexOf(BEGIN);
const to = source.indexOf(END);
if (from < 0 || to < 0 || to <= from) {
  console.error('Could not find the auto-retry block in assets/app.js.');
  console.error('Markers expected: ' + JSON.stringify(BEGIN) + ' .. ' + JSON.stringify(END));
  process.exit(1);
}
const block = source.slice(from, to);

// --- a clock we drive by hand ----------------------------------------------
// Real timers would make this test slow and flaky; the point is the retry
// COUNT, so time is simulated and advanced explicitly.
function makeClock() {
  let now = 0;
  let seq = 0;
  const timers = new Map();   // id -> {at, every, fn}
  return {
    now: () => now,
    setTimeout(fn, ms) { timers.set(++seq, { at: now + ms, every: 0, fn }); return seq; },
    setInterval(fn, ms) { timers.set(++seq, { at: now + ms, every: ms, fn }); return seq; },
    clearTimeout(id) { timers.delete(id); },
    clearInterval(id) { timers.delete(id); },
    pending: () => timers.size,
    // Advance to `now + ms`, firing everything due along the way.
    advance(ms) {
      const target = now + ms;
      for (let guard = 0; guard < 10000; guard++) {
        let nextId = null, nextAt = Infinity;
        for (const [id, t] of timers) {
          if (t.at <= target && t.at < nextAt) { nextAt = t.at; nextId = id; }
        }
        if (nextId === null) break;
        const t = timers.get(nextId);
        now = t.at;
        if (t.every > 0) t.at = now + t.every; else timers.delete(nextId);
        t.fn();
      }
      now = target;
    }
  };
}

// --- harness ----------------------------------------------------------------
// Builds a fresh sandbox holding the real block plus stubbed collaborators,
// and records every re-launch the block asks for.
function harness() {
  const clock = makeClock();
  const starts = [];
  const ctx = vm.createContext({
    Math, Date: { now: () => clock.now() },
    setTimeout: clock.setTimeout.bind(clock),
    setInterval: clock.setInterval.bind(clock),
    clearTimeout: clock.clearTimeout.bind(clock),
    clearInterval: clock.clearInterval.bind(clock),
    // Collaborators the block calls into; none of them matter to the policy.
    patchActiveRow: () => true,
    renderTable: () => {},
    saveData: () => {},
    updateToolbarState: () => {},
    requestStart: (dl) => { starts.push(dl.id); }
  });
  // Function declarations land on the sandbox global by themselves; a
  // top-level `const` does not, so the constant is re-exported explicitly.
  vm.runInContext(block + '\nglobalThis.MAX_AUTO_RETRIES = MAX_AUTO_RETRIES;', ctx);
  return { ctx, clock, starts };
}

// Let the row fail again as soon as it is re-launched, exactly as
// UI.onComplete does for a permanently broken URL.
function failForever(h, dl, totalMs) {
  h.ctx.requestStart = (row) => {
    h.starts.push(row.id);
    row.status = 'failed';
    h.ctx.scheduleRetry(row);
  };
  // Rebuild the context binding: requestStart is resolved at call time from
  // the sandbox global, so reassigning it above is enough.
  vm.runInContext('', h.ctx);
  h.clock.advance(totalMs);
}

console.log('the block exports what the app relies on');
{
  const h = harness();
  check('MAX_AUTO_RETRIES is defined', typeof h.ctx.MAX_AUTO_RETRIES === 'number',
        'got ' + h.ctx.MAX_AUTO_RETRIES);
  check('  and is a sane ceiling',
        h.ctx.MAX_AUTO_RETRIES > 0 && h.ctx.MAX_AUTO_RETRIES <= 10);
  check('scheduleRetry is a function', typeof h.ctx.scheduleRetry === 'function');
  check('cancelRetry is a function', typeof h.ctx.cancelRetry === 'function');
}

console.log('the backoff itself is unchanged');
{
  const h = harness();
  const d = h.ctx.getRetryDelay;
  check('first retry waits 2s', d(0) === 2000, d(0) + 'ms');
  check('it doubles', d(1) === 4000 && d(2) === 8000 && d(3) === 16000);
  check('and is capped at 60s', d(10) === 60000 && d(100) === 60000);
}

console.log('a permanently failing row eventually gives up');
{
  const h = harness();
  const dl = { id: 'dl_dead', status: 'failed' };
  const scheduled = h.ctx.scheduleRetry(dl);
  check('the first failure schedules a retry', scheduled === true);
  // An hour is far more than MAX_AUTO_RETRIES * 60s of backoff.
  failForever(h, dl, 60 * 60 * 1000);
  check('re-launch count stops at MAX_AUTO_RETRIES',
        h.starts.length === h.ctx.MAX_AUTO_RETRIES,
        h.starts.length + ' launches, cap is ' + h.ctx.MAX_AUTO_RETRIES);
  check('no timer is left running', h.clock.pending() === 0,
        h.clock.pending() + ' pending');
  check('scheduleRetry refuses once the attempts are spent',
        h.ctx.scheduleRetry(dl) === false);
  check('  and leaves no countdown behind in the row',
        dl.timeLeft === '--', JSON.stringify(dl.timeLeft));
}

console.log('a row that recovers is not punished for its earlier failures');
{
  const h = harness();
  const dl = { id: 'dl_flaky', status: 'failed' };
  // Two failures, then the download succeeds: UI.onComplete resets the count.
  h.ctx.scheduleRetry(dl);
  h.clock.advance(2000);
  dl.status = 'failed';
  h.ctx.scheduleRetry(dl);
  h.clock.advance(4000);
  check('two failures cost two re-launches', h.starts.length === 2,
        h.starts.length + '');
  dl._retryCount = 0;                       // what onComplete does on success
  check('after a success the full budget is available again',
        h.ctx.scheduleRetry(dl) === true);
  h.ctx.cancelRetry(dl);
}

console.log('cancelRetry really stops a pending retry');
{
  const h = harness();
  const dl = { id: 'dl_cancel', status: 'failed' };
  h.ctx.scheduleRetry(dl);
  check('a retry is pending', h.clock.pending() > 0);
  h.ctx.cancelRetry(dl);
  check('cancelRetry clears every timer', h.clock.pending() === 0);
  h.clock.advance(10 * 60 * 1000);
  check('and nothing is re-launched afterwards', h.starts.length === 0,
        h.starts.length + '');
}

console.log(`\n${failures ? 'FAILED' : 'ALL CHECKS PASSED'} (${failures} failing checks)`);
process.exit(failures ? 1 : 0);
