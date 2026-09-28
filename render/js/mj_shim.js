// cctext-render: MathJax's configuration and module loader (handwritten).
// Runs after rt_shim.js and before the official MathJax 4.1.3 component
// files (third_party/mathjax, third_party/mathjax-newcm-font), which the
// build wraps unchanged: docs/images.md, "Math".
//
// MathJax loads its components through `loader.require` when it is not in
// a browser. Ours never touches a file or the network: a component is
//   - a module compiled into this bundle's bytecode (the manifest's `wrap`
//     lines: the core, the TeX and MathML inputs, the SVG output, its base
//     font, the liteDOM adaptor), run once from __cctextMods, or
//   - JavaScript source in the pack (the manifest's `src` lines: TeX
//     extensions and the font's dynamic files), which the host evaluates
//     on first use (__hostLoad; SHA-256 checked against this build).
// Anything else fails like a missing file (a TeX error for \require of an
// unknown extension).
{
  const g = globalThis;
  g.__cctextMods = g.__cctextMods || {};
  // Extensions that reach outside the formula: raw HTML in TeX (texhtml),
  // links / classes / styles / ids (html), and parser options changed
  // from inside a formula (setoptions: it could lift maxMacros). Refused.
  const REFUSED = new Set([
    'mathjax/input/tex/extensions/texhtml',
    'mathjax/input/tex/extensions/html',
    'mathjax/input/tex/extensions/setoptions',
  ]);
  // A resolved component path -> our key: "mathjax/input/tex",
  // "mathjax-newcm-font/svg/dynamic/latin", ...
  const modKey = (path) => {
    const p = String(path).replace(/\\/g, '/').replace(/\.js$/, '');
    let m = /(?:^|\/)(mathjax-newcm-font\/.*)$/.exec(p);
    if (m) return m[1];
    m = /(?:^|\/)mathjax\/(.*)$/.exec(p);
    if (m) return 'mathjax/' + m[1];
    return p;
  };
  const loaded = new Set();
  g.__mjLoaded = loaded; // tests: which components a render needed
  g.__mjRequire = (path) => {
    const key = modKey(path);
    if (loaded.has(key)) return {};
    if (REFUSED.has(key)) throw new Error('not available here: ' + key.replace(/^.*\//, ''));
    const f = g.__cctextMods[key];
    if (typeof f === 'function') {
      loaded.add(key);
      delete g.__cctextMods[key]; // run once; its closure is garbage after
      f();
      return {};
    }
    let ok = false;
    try {
      ok = typeof g.__hostLoad === 'function' && g.__hostLoad(key);
    } catch (e) {
      // stderr (the editor shows it only with RTX_RENDER_DEBUG)
      console.warn('cctext-render: ' + key + ': ' + ((e && e.message) || e));
      throw e;
    }
    if (ok) {
      loaded.add(key);
      return {};
    }
    console.warn('cctext-render: component not bundled: ' + key + ' (' + path + ')');
    throw new Error('component not bundled: ' + key);
  };

  g.MathJax = {
    loader: {
      paths: { mathjax: 'mathjax', fonts: 'fonts' },
      // The SVG output pulls in its font (mathjax-newcm-font/svg) itself.
      // begingroup: mj_glue.js wraps each formula in a group (below).
      load: ['core', 'input/tex', 'input/mml', 'output/svg', 'adaptors/liteDOM', '[tex]/begingroup'],
      require: g.__mjRequire,
      versionWarnings: false,
      failed: (e) => { g.__mjLoadError = String((e && e.message) || e); },
    },
    startup: {
      typeset: false,            // no page: formulas come one at a time (mj_glue.js)
      pageReady: () => Promise.resolve(),
    },
    tex: {
      // begingroup keeps each formula's \def / \newcommand to itself;
      // without noundefined an unknown macro is an error (the host shows
      // why) instead of red text in the picture.
      packages: { '[+]': ['begingroup'], '[-]': ['noundefined'] },
      maxMacros: 1000,           // macro substitutions per formula (\def recursion ends here)
      maxBuffer: 5 * 1024,       // bytes a formula may expand to
      // Errors reach mj_glue.js as exceptions, not as a red <merror> box:
      // the editor keeps the last good picture and says why.
      formatError: (_jax, err) => { throw err; },
    },
    mml: {},
    svg: {
      fontCache: 'local',        // glyph paths as <defs> + <use> inside each SVG
      // One SVG per formula: MathJax 4 would split long inline math into
      // several pieces for the page's line breaker; there is no page here.
      linebreaks: { inline: false },
      // A display formula wider than the request's max width breaks its
      // lines there (mjRender's containerWidth; none: never).
      displayOverflow: 'linebreak',
    },
  };
}
