// Every curl handle ODM creates must carry the browser-grade TLS policy.
//
// The bug: a MediaFire download died with "Failed to probe the URL (check
// the link / network / HTTPS cert)" while Chrome downloaded the same file
// happily. Windows Schannel, at libcurl's defaults, checks certificate
// revocation and fails CLOSED when the CRL/OCSP endpoint cannot be reached —
// the user's middlebox answers 403 for crls.ssl.com, so every handshake to
// MediaFire's CDN died at InitializeSecurityContext (CRYPT_E_REVOKED) while
// the browser, which soft-fails revocation, got the file. The gentler
// REVOKE_BEST_EFFORT does not cure it (the chain comes back IS_REVOKED,
// which best-effort does not forgive), so the policy is NO_REVOKE — the same
// default libcurl has on every non-Schannel backend.
//
// The fix is ApplyTlsPolicy() on EVERY curl handle — a handle born without
// it is the same bug all over again, so this test scans the sources the way
// extension_identity scans the manifest: any curl_easy_init() not followed
// by the policy call fails.
//
// Usage: node tools/tls_policy_test.js   (ctest: `tls_policy`)
'use strict';
const fs = require('fs');
const path = require('path');

const SRC = path.join(__dirname, '..', 'src');

let failures = 0;
function check(what, ok, detail) {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${what}${detail ? '  (' + detail + ')' : ''}`);
  if (!ok) failures++;
}

console.log('the policy itself is the right one');
// Inline in the header: the test executables link single engine sources, and
// a .cpp definition would force them to drag in the whole engine for it.
const header = fs.readFileSync(path.join(SRC, 'Downloader.h'), 'utf8');
check('ApplyTlsPolicy is defined inline in Downloader.h',
      /inline void ApplyTlsPolicy\(CURL\* ?curl\)/.test(header));
// NO_REVOKE, not the gentler REVOKE_BEST_EFFORT: best-effort forgives only
// STATUS_UNKNOWN/OFFLINE, and the real failure (MediaFire CDN behind a
// middlebox that 403s crls.ssl.com) comes back IS_REVOKED — still fatal.
// Verified live on the affected machine. Every other libcurl TLS backend
// and the browsers check revocation not at all, so this is parity, not
// recklessness.
check('  and sets NO_REVOKE (the only mode that cures the blocked-CRL case)',
      /CURLSSLOPT_NO_REVOKE/.test(header));
check('  and not best-effort (proven insufficient against IS_REVOKED)',
      !/CURLSSLOPT_REVOKE_BEST_EFFORT/.test(header));

console.log('every curl handle gets the policy');
// A few lines of slack covers the mandatory `if (!curl)` guard (multi-line
// in the part worker); the call belongs immediately after init, before any
// option that could start the handshake.
const GRACE = 8;
let handles = 0;
for (const file of fs.readdirSync(SRC).filter(f => f.endsWith('.cpp'))) {
  const lines = fs.readFileSync(path.join(SRC, file), 'utf8').split(/\r?\n/);
  lines.forEach((line, i) => {
    if (!/curl_easy_init\s*\(/.test(line)) return;
    handles++;
    const near = lines.slice(i, i + 1 + GRACE).join('\n');
    check(`${file}:${i + 1} — curl_easy_init is followed by ApplyTlsPolicy`,
          /ApplyTlsPolicy\(/.test(near),
          near.includes('ApplyTlsPolicy') ? '' : 'nothing within ' + GRACE + ' lines');
  });
}
check('the scan actually found curl handles (a silent zero would pass vacuously)',
      handles >= 5, handles + ' init sites');

console.log(`\n${failures ? 'FAILED' : 'ALL CHECKS PASSED'} (${failures} failing checks)`);
process.exit(failures ? 1 : 0);
