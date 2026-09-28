// cctext-render: the few web-platform globals QuickJS lacks (no DOM here;
// mm_dom.js adds that). Handwritten; loaded first in every JS bundle.
// docs/images.md, "Mermaid".
//
// The helper has no event loop, no network, no files and no clock beyond
// the monotonic one: timers queue and the host drains them (__drainTimers)
// while it waits on a promise; fetch / XMLHttpRequest do not exist, so a
// library that tries to load a font, an icon or an image fails soft.
{ // block scope: keep helper names out of the global lexical scope
  const g = globalThis;
  if (typeof g.global === 'undefined') g.global = g; // bundles probe `global`

  // base64 (DOMPurify, KaTeX and roughjs reach for these)
  if (typeof g.atob !== 'function') {
    const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    g.atob = (s) => {
      s = String(s).replace(/[^A-Za-z0-9+/]/g, '');
      let o = '', b = 0, n = 0;
      for (const c of s) {
        b = (b << 6) | A.indexOf(c);
        n += 6;
        if (n >= 8) { n -= 8; o += String.fromCharCode((b >> n) & 255); }
      }
      return o;
    };
    g.btoa = (s) => {
      let o = '';
      for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        const t = (a << 16) | ((b || 0) << 8) | (c || 0);
        o += A[(t >> 18) & 63] + A[(t >> 12) & 63] +
          (i + 1 < s.length ? A[(t >> 6) & 63] : '=') + (i + 2 < s.length ? A[t & 63] : '=');
      }
      return o;
    };
  }

  // console: warnings and errors go to the helper's stderr (the editor
  // sends that to /dev/null unless RTX_RENDER_DEBUG is set).
  {
    const p = typeof print === 'function' ? print : () => {};
    const quiet = () => {};
    g.console = Object.assign({ log: quiet, warn: p, error: p, info: quiet, debug: quiet, trace: quiet },
      g.console || {});
  }

  // Timers: no event loop. A callback queues; the host drains the queue
  // after each microtask turn until the job's promise settles. Delays are
  // ignored (nothing in a render waits on wall time).
  if (typeof g.setTimeout !== 'function') {
    const q = [];
    let next = 1;
    const cancelled = new Set();
    g.setTimeout = (fn, _ms, ...a) => { const id = next++; q.push([id, () => fn(...a)]); return id; };
    g.clearTimeout = (id) => { cancelled.add(id); };
    g.setInterval = g.setTimeout; // run once: nothing in a render repeats
    g.clearInterval = g.clearTimeout;
    g.queueMicrotask = g.queueMicrotask || ((fn) => Promise.resolve().then(fn));
    // Returns how many callbacks ran (0: nothing was queued, so a promise
    // still pending will never settle; the host gives up on it).
    g.__drainTimers = () => {
      let ran = 0;
      while (q.length) {
        const [id, f] = q.shift();
        if (cancelled.delete(id)) continue;
        ran++;
        f();
      }
      cancelled.clear();
      return ran;
    };
  }

  // UTF-8 TextEncoder / TextDecoder (enough for the parsers that use them).
  if (typeof g.TextEncoder === 'undefined') {
    g.TextEncoder = class {
      get encoding() { return 'utf-8'; }
      encode(s = '') {
        const o = [];
        for (const ch of String(s)) {
          const c = ch.codePointAt(0);
          if (c < 0x80) o.push(c);
          else if (c < 0x800) o.push(0xc0 | (c >> 6), 0x80 | (c & 63));
          else if (c < 0x10000) o.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
          else o.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
        return new Uint8Array(o);
      }
    };
    g.TextDecoder = class {
      constructor(l = 'utf-8') { this.encoding = l; }
      decode(b) {
        if (!b) return '';
        b = b instanceof Uint8Array ? b : new Uint8Array(b.buffer || b);
        let s = '';
        for (let i = 0; i < b.length;) {
          let c = b[i++];
          if (c >= 0xf0) c = ((c & 7) << 18) | ((b[i++] & 63) << 12) | ((b[i++] & 63) << 6) | (b[i++] & 63);
          else if (c >= 0xe0) c = ((c & 15) << 12) | ((b[i++] & 63) << 6) | (b[i++] & 63);
          else if (c >= 0xc0) c = ((c & 31) << 6) | (b[i++] & 63);
          s += String.fromCodePoint(c);
        }
        return s;
      }
    };
  }

  // Legacy RegExp.$1..$9: roughjs's SVG path tokenizer reads them and
  // QuickJS does not implement them.
  if (!('$1' in RegExp)) {
    const exec = RegExp.prototype.exec;
    RegExp.prototype.exec = function (s) {
      const m = exec.call(this, s);
      if (m) for (let i = 1; i <= 9; i++) RegExp['$' + i] = m[i] === undefined ? '' : m[i];
      return m;
    };
    for (let i = 1; i <= 9; i++) RegExp['$' + i] = '';
  }

  // No Intl in this QuickJS build. mermaid probes `Intl.Segmenter` (text
  // wrapping) and falls back to code points when it is missing; an empty
  // Intl makes the probe a miss instead of a ReferenceError.
  if (typeof g.Intl === 'undefined') g.Intl = {};

  // Deliberately absent: fetch, XMLHttpRequest, WebSocket, Worker,
  // importScripts, eval-from-network. Nothing in a render may reach out.
}
