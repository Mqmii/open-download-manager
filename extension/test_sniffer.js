// Temporary verification: runs the background.js sniffer logic in Node with
// the chrome API mocked. Usage: node test_sniffer.js  (can be deleted later)
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

const code = fs.readFileSync(path.join(__dirname, 'background.js'), 'utf8');
const stub = new Proxy(function () {}, {
  get: () => stub,
  apply: () => stub,
  set: () => true
});
const ctx = vm.createContext({
  chrome: stub, console, URL, setTimeout, clearTimeout, atob,
  Date, Math, JSON, Number, String, parseInt, RegExp, Set, Map, Array, Object, Promise
});
vm.runInContext(code, ctx);

// Meta DASH rung URL builder: `efg` is base64 JSON, exactly as fbcdn sends it.
const efg = (tag, assetId) => Buffer.from(JSON.stringify({
  vencode_tag: 'ig-xpvds.clips.igwww-C3.' + tag, xpv_asset_id: assetId
})).toString('base64');
const IG_VIDEO_Q90 = 'https://instagram.fayt2-3.fna.fbcdn.net/o1/v/t2/f2/m367/AQabc.mp4' +
  '?efg=' + encodeURIComponent(efg('dash_r2evevp9-r1gen2vp9_q90', 1023140973606299)) +
  '&bytestart=0&byteend=817';
const IG_VIDEO_Q20 = 'https://instagram.fayt2-1.fna.fbcdn.net/o1/v/t2/f2/m367/AQxyz.mp4' +
  '?efg=' + encodeURIComponent(efg('dash_r2evevp9-r1gen2vp9_q20', 1023140973606299)) +
  '&bytestart=818&byteend=897';
const IG_AUDIO = 'https://instagram.fayt2-1.fna.fbcdn.net/o1/v/t2/f2/m78/AQsnd.mp4' +
  '?efg=' + encodeURIComponent(efg('dash_ln_heaac_vbr3_audio', 1023140973606299));
const IG_AUDIO_OTHER = 'https://instagram.fayt2-1.fna.fbcdn.net/o1/v/t2/f2/m78/AQoth.mp4' +
  '?efg=' + encodeURIComponent(efg('dash_ln_heaac_vbr3_audio', 2795788387460786));

const cases = [
  // [url, mime, expected kind]
  ['https://cdn.site.com/videos/movie.mp4?token=abc', 'video/mp4', 'video'],
  ['https://st.okcdn.ru/video/12345', 'video/mp4', 'video'],              // ok.ru-style extensionless, MIME video
  ['https://site.com/hls/master.m3u8', 'application/vnd.apple.mpegurl', 'hls'],
  ['https://site.com/get/playlist', 'application/x-mpegURL', 'hls'],       // extensionless manifest, from MIME
  ['https://site.com/dash/manifest.mpd', 'application/dash+xml', 'dash'],
  ['https://rr5---sn-4g5ednsz.googlevideo.com/videoplayback?id=x&itag=137&range=0-100', 'video/mp4', 'youtube'],
  ['https://cdn.site.com/seg-00001.ts', 'video/mp2t', null],               // HLS segment: noise
  ['https://cdn.site.com/seg-1.m4s', 'video/iso.segment', null],           // DASH segment: noise
  ['https://site.com/song.mp3', 'audio/mpeg', 'audio'],
  ['https://site.com/track', 'audio/webm', 'audio'],
  ['https://site.com/page.html', 'text/html', null],
  ['https://site.com/video', 'application/octet-stream', null],            // extensionless + generic MIME: skip
  ['https://site.com/movie.mkv', 'application/octet-stream', 'video'],     // the extension saves it

  // --- Meta (Instagram/Facebook) DASH: .mp4 extension, video/mp4 MIME but
  //     NOT a complete file. Must classify as dash so it isn't downloaded
  //     silent/fragmented.
  [IG_VIDEO_Q90, 'video/mp4', 'dash'],
  [IG_AUDIO, 'video/mp4', 'dash'],
  // Real progressive file on the same CDN: must still be downloadable video.
  ['https://scontent.cdninstagram.com/v/t50.2886-16/12345_n.mp4?_nc_cat=1', 'video/mp4', 'video'],
  // Host lock: even if bytestart appears on another CDN it must be untouched.
  ['https://cdn.site.com/movie.mp4?bytestart=0&byteend=99', 'video/mp4', 'video'],
  // Reddit CMAF traces (regression: this path must never change).
  ['https://v.redd.it/abc123/DASH_720.mp4', 'video/mp4', 'video'],
  ['https://v.redd.it/abc123/DASH_AUDIO_128.mp4', 'video/mp4', 'video'],
  ['https://v.redd.it/abc123/HLSPlaylist.m3u8', 'application/vnd.apple.mpegurl', 'hls']
];

let fail = 0;
for (const [u, mime, want] of cases) {
  const got = ctx.classifyMedia(u, mime);
  const g = got ? got.kind : null;
  const ok = g === want;
  if (!ok) fail++;
  console.log(ok ? 'PASS' : 'FAIL', JSON.stringify(u), '->', g, '(want ' + want + ')');
}

// Meta DASH dedupe: all quality rungs + audio of one video = ONE entry;
// another video's audio must be a separate entry.
const d1 = ctx.mediaKey(IG_VIDEO_Q90, 'dash');
const d2 = ctx.mediaKey(IG_VIDEO_Q20, 'dash');
const d3 = ctx.mediaKey(IG_AUDIO, 'dash');
const d4 = ctx.mediaKey(IG_AUDIO_OTHER, 'dash');
const dashOk = d1 === d2 && d2 === d3 && d3 !== d4;
console.log(dashOk ? 'PASS meta-dash dedupe' : 'FAIL meta-dash dedupe', d1, '|', d4);
if (!dashOk) fail++;

// YouTube range dedupe: different ranges must collapse into one entry
const k1 = ctx.mediaKey('https://rr5.googlevideo.com/videoplayback?id=a&itag=140&range=0-99', 'youtube');
const k2 = ctx.mediaKey('https://rr5.googlevideo.com/videoplayback?id=a&itag=140&range=100-199&rn=3', 'youtube');
console.log(k1 === k2 ? 'PASS dedupe' : 'FAIL dedupe', k1);
if (k1 !== k2) fail++;

// File name: Windows-forbidden characters must be stripped
const f = ctx.suggestMediaFilename('Big Buck Bunny: 4K "test" <hd>', 'https://x/v.mp4', 'mp4');
const bad = /[<>:"\\|?*]/.test(f);
console.log('filename:', JSON.stringify(f), bad ? 'FAIL filename' : 'PASS filename');
if (bad) fail++;

// No title: derive the name from the URL
const f2 = ctx.suggestMediaFilename('', 'https://x.com/dir/video%20final.mp4?x=1', 'mp4');
console.log('filename2:', JSON.stringify(f2));
if (f2 !== 'video final.mp4') { console.log('FAIL filename2'); fail++; }

process.exit(fail ? 1 : 0);
