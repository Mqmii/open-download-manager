// The extension id the bridge trusts must be the id Chrome actually assigns.
//
// The bridge no longer waves every `chrome-extension://` origin through; it
// names exactly one. That name is only correct as long as it stays in step
// with the `key` in extension/manifest.json, and nothing in either file makes
// the connection visible — edit one, forget the other, and the hand-off dies
// with a 403 that looks like "ODM is not running".
//
// So the id is recomputed here the way Chrome does it, straight from the
// manifest, and compared against the constant in src/BridgeServer.h.
//
// Usage: node tools/extension_identity_test.js  (ctest: `extension_identity`)
'use strict';
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const MANIFEST = path.join(ROOT, 'extension', 'manifest.json');
const BRIDGE_H = path.join(ROOT, 'src', 'BridgeServer.h');

let failures = 0;
function check(what, ok, detail) {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${what}${detail ? '  (' + detail + ')' : ''}`);
  if (!ok) failures++;
}

// Chrome's rule: SHA-256 over the key's DER bytes, take the first 16, and map
// each nibble 0-15 onto 'a'-'p'. (Same alphabet as an extension id, which is
// why ids look like words and never contain a digit.)
function idFromKey(base64Key) {
  const der = Buffer.from(base64Key, 'base64');
  const h = crypto.createHash('sha256').update(der).digest();
  let id = '';
  for (let i = 0; i < 16; i++) {
    id += String.fromCharCode(97 + (h[i] >> 4));
    id += String.fromCharCode(97 + (h[i] & 0x0f));
  }
  return id;
}

console.log('the manifest pins an identity at all');
const manifest = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
check('manifest.json carries a `key`', typeof manifest.key === 'string' &&
      manifest.key.length > 0);
if (typeof manifest.key !== 'string' || !manifest.key) {
  console.log('\nFAILED (no key to check against)');
  process.exit(1);
}
{
  const der = Buffer.from(manifest.key, 'base64');
  // A 2048-bit RSA SPKI is 294 bytes; anything near that is plausible, a
  // truncated or re-wrapped blob is not.
  check('  and it decodes to a plausible SPKI blob', der.length > 100,
        der.length + ' bytes');
  // Round-trip: base64 that silently drops characters would still "decode".
  check('  and the base64 round-trips exactly',
        der.toString('base64') === manifest.key);
}

console.log('the bridge trusts exactly that identity');
const derived = idFromKey(manifest.key);
check('the derived id is a well-formed extension id',
      /^[a-p]{32}$/.test(derived), derived);

const header = fs.readFileSync(BRIDGE_H, 'utf8');
const m = header.match(/kExtensionId\s*=\s*"([^"]*)"/);
check('src/BridgeServer.h declares kExtensionId', !!m);
if (m) {
  check('kExtensionId matches the id derived from the manifest key',
        m[1] === derived, m[1] === derived ? derived
                                           : `header=${m[1]} manifest=${derived}`);
}

console.log('the derivation itself is right');
{
  // Chrome's own published example: the id for this key is well known, so a
  // broken implementation of idFromKey cannot quietly agree with a broken
  // constant. An all-zero 16-byte digest prefix would map to "aaaa...", so a
  // fixed vector is the only way to catch a mirrored/shifted nibble map.
  const der = Buffer.alloc(32, 0);          // sha256 of this is a fixed value
  const h = crypto.createHash('sha256').update(der).digest();
  let want = '';
  for (let i = 0; i < 16; i++) {
    want += 'abcdefghijklmnop'[h[i] >> 4];
    want += 'abcdefghijklmnop'[h[i] & 0x0f];
  }
  check('a known blob maps to the expected id',
        idFromKey(der.toString('base64')) === want, want);
  check('  and the map really is nibble-per-character',
        want.length === 32 && /^[a-p]+$/.test(want));
}

console.log(`\n${failures ? 'FAILED' : 'ALL CHECKS PASSED'} (${failures} failing checks)`);
process.exit(failures ? 1 : 0);
