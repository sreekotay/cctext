// cctext-render: the math entry point the host calls (render/cr_js.c).
// Loaded after rt_shim.js, mj_shim.js, the wrapped MathJax components and
// the official startup.js. docs/images.md, "Math".
//
//   mjRender(kind, source, optsJSON) -> Promise<string>
//
// kind: 'tex' | 'mml'. opts: { display, em (px), fg ('#rrggbb'),
// maxWidth (px, 0: none), nodeMax (0: none) }.
// Resolves to a line of JSON {"w":..,"h":..,"b":..} (CSS px; b: the
// baseline's distance from the bottom), a newline, then the SVG text;
// or to "CCTEXT_ERR <message>" (the formula does not parse: TeX's or
// MathML's own message) or "CCTEXT_TOO_LARGE <count> <limit>" (more
// MathML nodes than nodeMax). Both keep the engine. Anything else rejects
// (the host starts a fresh engine for the next formula).
{
  const g = globalThis;
  let ready = null;
  let nodeMax = 0;
  class TooLarge extends Error {}

  // Once, after MathJax's startup: the node cap and MathML's error nodes,
  // checked on the internal MathML tree of every formula (both inputs)
  // before any output work.
  const setup = async () => {
    await MathJax.startup.promise;
    for (const jax of MathJax.startup.document.inputJax) {
      // The filter's data: TeX's parse options (the tree is data.root) or
      // MathML's compiled tree itself.
      jax.postFilters.add(({ data }) => {
        const root = data && data.root && typeof data.root.walkTree === 'function' ? data.root : data;
        let n = 0, err = null;
        if (!root || typeof root.walkTree !== 'function') return;
        root.walkTree((node) => {
          n++;
          // MathJax's own error nodes (a MathML compile error, a verify
          // error such as a wrong child count); an <merror> the source
          // wrote itself has neither attribute and is drawn.
          if (!err && node.isKind && node.isKind('merror') && node.attributes) {
            const m = node.attributes.get('data-mjx-error') || node.attributes.get('data-mjx-message');
            if (m) err = String(m);
          }
        });
        if (nodeMax > 0 && n > nodeMax) throw new TooLarge(n + ' ' + nodeMax);
        if (err) throw new Error(err);
      }, -100);
    }
  };

  // SVG width / height / vertical-align in ex -> CSS px.
  const exOf = (s, re) => {
    const m = re.exec(s || '');
    return m ? parseFloat(m[1]) : 0;
  };

  g.mjRender = async (kind, src, optsJSON) => {
    const o = JSON.parse(optsJSON || '{}');
    const em = o.em > 0 && o.em < 1000 ? o.em : 16;
    const ex = em / 2; // any ratio works: sizes come back in ex and go out times ex
    const fg = /^#[0-9a-fA-F]{6}$/.test(o.fg || '') ? o.fg : '#000000';
    if (!ready) ready = setup();
    await ready;
    nodeMax = o.nodeMax > 0 ? o.nodeMax : 0;
    const opts = {
      display: !!o.display, em, ex,
      containerWidth: o.maxWidth > 0 ? o.maxWidth : 100000,
    };
    let node;
    try {
      if (kind === 'mml') {
        node = await MathJax.mathml2svgPromise(src, opts);
      } else {
        // A group around the formula: \def and \newcommand stay inside it,
        // so no formula changes how a later one renders (a render depends
        // on its own bytes only: the cache key). The newline ends a
        // trailing % comment before \endgroup.
        node = await MathJax.tex2svgPromise('\\begingroup ' + src + '\n\\endgroup', opts);
      }
    } catch (e) {
      if (e instanceof TooLarge) return 'CCTEXT_TOO_LARGE ' + e.message;
      // A TeX error (MathJax's TexError, not an Error subclass), a MathML
      // error (a plain Error from the MathML input) or a limit (maxMacros,
      // maxBuffer): an answer, the engine is fine. A script failure
      // (TypeError, a stack overflow, out of memory) rejects.
      if (e instanceof TypeError || e instanceof ReferenceError || e instanceof SyntaxError ||
          e instanceof RangeError || (typeof InternalError === 'function' && e instanceof InternalError))
        throw e;
      const msg = String((e && e.message) || e).replace(/\s+/g, ' ').trim();
      return 'CCTEXT_ERR ' + (msg || 'formula does not parse');
    } finally {
      nodeMax = 0;
    }
    const A = MathJax.startup.adaptor;
    let svg = node;
    while (svg && A.kind(svg) !== 'svg') svg = A.firstChild(svg);
    if (!svg) throw new Error('no svg in the output');
    // An <merror> the source wrote itself (MathML) is drawn, in MathJax's
    // page-stylesheet colours: there is no page, so they go inline.
    (function fix(n) {
      if (A.kind(n) === 'g' && A.getAttribute(n, 'data-mml-node') === 'merror')
        for (const c of A.childNodes(n)) {
          if (A.kind(c) === 'rect') { A.setAttribute(c, 'fill', '#ffff88'); A.setAttribute(c, 'stroke', 'none'); }
          else if (A.kind(c) === 'g') { A.setAttribute(c, 'fill', '#cc0000'); A.setAttribute(c, 'stroke', '#cc0000'); }
        }
      for (const c of A.childNodes(n) || []) if (A.kind(c) !== '#text' && A.kind(c) !== '#comment') fix(c);
    })(svg);
    const wex = exOf(A.getAttribute(svg, 'width'), /^(-?[\d.]+)ex$/);
    const hex = exOf(A.getAttribute(svg, 'height'), /^(-?[\d.]+)ex$/);
    const vex = exOf(A.getAttribute(svg, 'style'), /vertical-align:\s*(-?[\d.]+)ex/);
    const w = wex * ex, h = hex * ex, b = vex < 0 ? -vex * ex : 0;
    A.setAttribute(svg, 'width', w.toFixed(3));
    A.setAttribute(svg, 'height', h.toFixed(3));
    A.removeAttribute(svg, 'style');
    let s = A.serializeXML ? A.serializeXML(svg) : A.outerHTML(svg);
    s = s.replace(/currentColor/g, fg);
    if (!/^<svg[^>]*xmlns=/.test(s)) s = s.replace('<svg', '<svg xmlns="http://www.w3.org/2000/svg"');
    return JSON.stringify({ w, h, b }) + '\n' + s;
  };

  // Development (cctext-render --math IN OUT.mml): TeX -> MathML text, for
  // the MathML side of testdata/math (the same formulas through the other
  // input must draw the same pictures).
  g.mjTex2mml = async (src, optsJSON) => {
    const o = JSON.parse(optsJSON || '{}');
    if (!ready) ready = setup();
    await ready;
    return MathJax.tex2mml(src, { display: !!o.display });
  };

  // Test hooks: only bin/cctext-render-selftest calls these (a source
  // "%cr-selftest:jsloop" / "%cr-selftest:jsheap N"); the release helper
  // never does.
  g.__cctextSelftestLoop = () => { for (;;) { /* until interrupted */ } };
  g.__cctextSelftestHeap = (mb) => {
    const keep = (g.__cctextKeep = g.__cctextKeep || []);
    for (let i = 0; i < Number(mb); i++) keep.push(new Uint8Array(1 << 20).fill(1));
    return '{"w":4,"h":4,"b":0}\n<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"/>';
  };
}
