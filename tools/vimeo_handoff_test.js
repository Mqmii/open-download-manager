// A Vimeo stream must reach the app as a yt-dlp page hand-off.
//
// On vimeo.com the panel used to sit on "Searching for stream..." forever.
// The sniffer knew three stream shapes — m3u8, mpd and plain files — and
// Vimeo's player speaks none of them: its manifest is a proprietary
// playlist.json on vod-adaptive-*.vimeocdn.com and its segments are bare
// .m4s, which the segment filter discards. So nothing was ever captured,
// and with nothing captured the panel has nothing to offer.
//
// The fix recognizes the vimeocdn adaptive pipeline as a "vimeo" entry and,
// like YouTube, hands the PAGE to the app, which resolves it with yt-dlp.
// The functions under test are lifted verbatim out of extension/background.js,
// so what is asserted here is what the extension actually ships.
//
// Usage: node tools/vimeo_handoff_test.js   (ctest: `vimeo_handoff`)
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

const BG = path.join(__dirname, '..', 'extension', 'background.js');

let failures = 0;
function check(what, ok, detail) {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${what}${detail ? '  (' + detail + ')' : ''}`);
  if (!ok) failures++;
}

// --- lift the real declarations --------------------------------------------
// The functions under test sit between chrome.* listeners the test cannot
// run, so they are cut out of the source one declaration at a time (balanced
// delimiters) and assembled into a sandbox. A missing declaration is a FAIL,
// not a crash — the point is to notice when one of these stops existing.
function liftDecl(source, marker) {
  const start = source.indexOf(marker);
  if (start < 0) return null;
  // A function declaration's span is its BODY — the parameter list is a
  // balanced span of its own and would end the scan early.
  const from = marker.includes('function ')
    ? source.indexOf('{', start + marker.length)
    : start;
  if (from < 0) return null;
  const open = { '{': '}', '(': ')', '[': ']' };
  const shut = { '}': '{', ')': '(', ']': '[' };
  const stack = [];
  for (let i = from; i < source.length; i++) {
    const c = source[i];
    if (open[c]) stack.push(c);
    else if (shut[c]) {
      if (stack.pop() !== shut[c]) return null;   // unbalanced source
      if (stack.length === 0) return source.slice(start, i + 1);
    }
  }
  return null;
}

const source = fs.readFileSync(BG, 'utf8');
const DECLS = [
  'const VIDEO_EXT = new Set(',
  'const AUDIO_EXT = new Set(',
  'const SKIP_EXT = new Set(',
  'function extOfUrl(',
  'function filenameFromUrl(',
  'function fbDashInfo(',
  'function classifyMedia(',
  'function mediaKey(',
  'function suggestMediaFilename(',
  'function isYouTubeVideoUrl(',
  'function isVimeoVideoUrl(',
  'function isVimeoStreamUrl(',
  'async function handleMediaDownload(',
];
const pieces = [];
const missing = [];
for (const marker of DECLS) {
  const decl = liftDecl(source, marker);
  if (decl) pieces.push(decl); else missing.push(marker);
}
check('every declaration this fix relies on exists in background.js',
      missing.length === 0,
      missing.length ? 'missing: ' + missing.join(', ') : '');

// The real captured shapes (vimeo.com/33698814). The clip GUID
// 71c35e5e-… folds every request of one video into one entry.
const GUID = '71c35e5e-d537-4e38-b6f6-5f0723f97877';
const PLAYLIST =
  'https://vod-adaptive-ak.vimeocdn.com/exp=1785~acl=%2F' + GUID + '%2F*~hmac=4646512361' +
  '/' + GUID + '/psid=9525ba/v2/playlist/av/primary/playlist.json?omit=av1-hevc&pathsig=8c953e4f';
const SEGMENT =
  'https://vod-adaptive-ak.vimeocdn.com/exp=1785~acl=%2F' + GUID + '%2F*~hmac=4646512361' +
  '/' + GUID + '/psid=9525ba/v2/remux/avf/c5fa7514-0704-428d-8ee9-b6a30e6ec757/segment.m4s?st=video&sid=1';
const OTHER_CLIP_PLAYLIST = PLAYLIST.replace(new RegExp(GUID, 'g'),
                                             '99999999-1111-4222-8333-abcdefabcdef');

let ctx = null;
if (!missing.length) {
  const notifications = [];
  const handed = [];
  ctx = vm.createContext({
    URL, Set, Map, JSON, Math, RegExp, Date, Promise,
    Array, Object, String, Number, Boolean, parseInt, parseFloat, isFinite,
    atob: s => Buffer.from(s, 'base64').toString('binary'),
    // Collaborators the hand-off calls into; they record instead of acting.
    notify: (title, body) => notifications.push({ title, body }),
    sendToOdm: async payload => { handed.push(payload); return true; },
  });
  vm.runInContext(pieces.join('\n'), ctx);
  ctx.__notifications = notifications;
  ctx.__handed = handed;
}

if (!ctx) {
  check('the sandbox assembles — every check below needs it', false);
  console.log(`\nFAILED (${failures} failing checks)`);
  process.exit(1);
} else {
  const { classifyMedia, mediaKey, isVimeoVideoUrl, isVimeoStreamUrl,
          handleMediaDownload } = ctx;

  console.log('vimeocdn adaptive traffic is recognized');
  check('the playlist.json manifest classifies as vimeo',
        classifyMedia(PLAYLIST, 'application/json')?.kind === 'vimeo');
  check('a CMAF segment under /v2/remux/ classifies as vimeo',
        classifyMedia(SEGMENT, 'video/iso.segment')?.kind === 'vimeo');
  check('  (the segment filter would otherwise eat it)',
        /\.m4s(\?|$)/.test(SEGMENT));
  check('an .mpd on a vimeocdn host classifies as vimeo, not undownloadable dash',
        classifyMedia('https://skyfire.vimeocdn.com/1690/0x123/33698814/master.mpd?base64_init=1',
                      'application/dash+xml')?.kind === 'vimeo');
  check('isVimeoStreamUrl rejects the same path on a foreign host',
        !isVimeoStreamUrl('https://cdn.example.com/v2/playlist/av/primary/playlist.json'));
  check('  and a foreign playlist.json is not media at all',
        classifyMedia('https://cdn.example.com/v2/playlist/av/primary/playlist.json',
                      'application/json') === null);

  console.log('the winnable shapes still win');
  check('a progressive .mp4 on vimeocdn stays plain video (no yt-dlp detour)',
        classifyMedia('https://vod-progressive-ak.vimeocdn.com/exp=1/' + GUID + '/vimeo-prod/33698814/76688959.mp4',
                      'video/mp4')?.kind === 'video');
  check('an .m3u8 on vimeocdn stays hls (the native engine handles it)',
        classifyMedia('https://vod-adaptive-ak.vimeocdn.com/exp=1/x/master.m3u8',
                      'application/vnd.apple.mpegurl')?.kind === 'hls');
  check('a bare .m4s off vimeocdn is still segment noise',
        classifyMedia('https://cdn.example.com/seg/segment-1.m4s',
                      'video/iso.segment') === null);
  check('a googlevideo rung still says youtube',
        classifyMedia('https://rr5---sn-abc.googlevideo.com/videoplayback?expire=1&id=abc123',
                      'video/mp4')?.kind === 'youtube');
  check('a generic .mpd elsewhere still says dash',
        classifyMedia('https://cdn.example.com/dash/master.mpd',
                      'application/dash+xml')?.kind === 'dash');

  console.log('one clip folds into one entry');
  {
    const k1 = mediaKey(PLAYLIST, 'vimeo');
    const k2 = mediaKey(SEGMENT, 'vimeo');
    check('manifest and segment of one clip share a key', k1 === k2,
          k1 === k2 ? k1 : `${k1} != ${k2}`);
    check('a different clip gets a different key',
          mediaKey(OTHER_CLIP_PLAYLIST, 'vimeo') !== k1);
  }
  console.log('the page gate knows what one Vimeo video looks like');
  check('plain watch page', isVimeoVideoUrl('https://vimeo.com/33698814'));
  check('  with an unlisted hash', isVimeoVideoUrl('https://vimeo.com/33698814/abcdef1234'));
  check('  under a channel', isVimeoVideoUrl('https://vimeo.com/channels/staffpicks/33698814'));
  check('  with a query', isVimeoVideoUrl('https://vimeo.com/33698814?fl=pl&fe=vl'));
  check('the embed player', isVimeoVideoUrl('https://player.vimeo.com/video/33698814'));
  check('the home feed is not a video', !isVimeoVideoUrl('https://vimeo.com/'));
  check('  and neither is /watch', !isVimeoVideoUrl('https://vimeo.com/watch'));
  check('  and neither is search', !isVimeoVideoUrl('https://vimeo.com/search?q=test'));
  check('a YouTube watch page is not Vimeo', !isVimeoVideoUrl('https://www.youtube.com/watch?v=abc123'));
  check('a lookalike host is not Vimeo', !isVimeoVideoUrl('https://notvimeo.com/33698814'));

  (async () => {
    console.log('the click hands the PAGE to yt-dlp');
    const sender = { url: 'https://vimeo.com/33698814',
                     tab: { url: 'https://vimeo.com/33698814' } };
    {
      ctx.__handed.length = 0; ctx.__notifications.length = 0;
      const ok = await handleMediaDownload(
        { kind: 'vimeo', url: 'vimeo:' + GUID,
          pageUrl: 'https://vimeo.com/33698814', height: 0 }, sender);
      const p = ctx.__handed[0];
      check('the hand-off succeeds', ok === true);
      check('  and hands over the watch page',
            p && p.url === 'https://vimeo.com/33698814', p && p.url);
      check('  routed to the yt-dlp resolve', p && p.type === 'ytdlp');
      check('  with the page as referrer',
            p && p.referrer === 'https://vimeo.com/33698814');
      check('  and no filename (the app names the file after the video title)',
            p && !('filename' in p));
    }
    {
      ctx.__handed.length = 0; ctx.__notifications.length = 0;
      const ok = await handleMediaDownload(
        { kind: 'vimeo', url: 'vimeo:x', pageUrl: 'https://vimeo.com/33698814',
          height: 720 }, sender);
      check('a picked quality rides along as a height',
            ok && ctx.__handed[0] && ctx.__handed[0].height === '720',
            ctx.__handed[0] && ctx.__handed[0].height);
    }
    {
      ctx.__handed.length = 0; ctx.__notifications.length = 0;
      // Nothing vimeo-shaped anywhere: better a notification than a garbage
      // hand-off.
      const ok = await handleMediaDownload(
        { kind: 'vimeo', url: 'vimeo:x', pageUrl: 'https://news.example/article' },
        { url: 'https://news.example/article',
          tab: { url: 'https://news.example/article' } });
      check('a page that names no video is refused', ok === false);
      check('  nothing is handed over', ctx.__handed.length === 0);
      check('  and the user is told', ctx.__notifications.length === 1);
    }
    {
      ctx.__handed.length = 0; ctx.__notifications.length = 0;
      // The embed case: the panel lives in the player iframe, so the frame's
      // own URL (sender.url) is the one that names the video, even when the
      // top page has nothing to do with Vimeo.
      const ok = await handleMediaDownload(
        { kind: 'vimeo', url: 'vimeo:x',
          pageUrl: 'https://player.vimeo.com/video/33698814' },
        { url: 'https://player.vimeo.com/video/33698814',
          tab: { url: 'https://news.example/' } });
      const p = ctx.__handed[0];
      check('an embed hands over its player URL',
            ok === true && p && p.url === 'https://player.vimeo.com/video/33698814',
            p && p.url);
    }
    {
      ctx.__handed.length = 0; ctx.__notifications.length = 0;
      const ok = await handleMediaDownload(
        { kind: 'youtube', url: 'yt:abc123',
          pageUrl: 'https://www.youtube.com/watch?v=abc123', height: 0 },
        { url: 'https://www.youtube.com/watch?v=abc123',
          tab: { url: 'https://www.youtube.com/watch?v=abc123' } });
      const p = ctx.__handed[0];
      check('YouTube still routes the same way',
            ok === true && p && p.type === 'ytdlp' &&
            p.url === 'https://www.youtube.com/watch?v=abc123', p && p.url);
    }

    console.log(`\n${failures ? 'FAILED' : 'ALL CHECKS PASSED'} (${failures} failing checks)`);
    process.exit(failures ? 1 : 0);
  })().catch(e => {
    console.error('the hand-off checks threw:', e);
    process.exit(1);
  });
}

