// cctext-render: the Mermaid entry point the host calls (render/cr_js.c).
// Loaded after rt_shim.js, mm_dom.js and the official mermaid.min.js
// (which sets globalThis.mermaid). docs/images.md, "Mermaid".
//
//   mmRender(id, source, optsJSON) -> Promise<string>  the SVG text
//
// opts: { theme: 'default' | 'dark', themeVariables: {...}, fontFamily,
//         nodeMax, maxTextSize }
// A source that does not parse resolves to "CCTEXT_PARSE <message>"; a
// diagram over opts.nodeMax to "CCTEXT_TOO_LARGE <count> <limit>" (the
// host answers "diagram too large to render (N nodes, limit M)"). Both keep
// the engine: no layout ran. Errors past the parse reject with mermaid's
// Error (the host then starts a fresh engine for the next diagram).
{
  // Configuration no diagram may change: `secure` keys are dropped from
  // %%{init}%% directives and frontmatter config (mermaid's own rule).
  // Ours add the label mode (HTML labels would need a browser: lunasvg
  // skips <foreignObject>) and the text limit.
  const SECURE = ['secure', 'securityLevel', 'startOnLoad', 'maxTextSize', 'suppressErrorRendering',
    'maxEdges', 'htmlLabels', 'deterministicIds', 'deterministicIDSeed', 'fontFamily'];

  // Elements a diagram lays out, from mermaid's own parse (its db), per
  // type. Layout is what costs (about 25 ms a node), parsing is cheap.
  const size = (x) => (x == null ? 0 : typeof x.size === 'number' ? x.size
    : Array.isArray(x) ? x.length : typeof x === 'object' ? Object.keys(x).length : 0);
  const call = (db, name) => (db && typeof db[name] === 'function' ? db[name]() : null);
  // State diagrams: every state id in the (nested) statement tree.
  function stateIds(doc, ids, depth) {
    if (!Array.isArray(doc) || depth > 10000) return;
    for (const s of doc) {
      if (!s || typeof s !== 'object') continue;
      if (s.stmt === 'state' && s.id != null) ids.add(String(s.id));
      if (s.stmt === 'relation') {
        if (s.state1 && s.state1.id != null) ids.add(String(s.state1.id));
        if (s.state2 && s.state2.id != null) ids.add(String(s.state2.id));
      }
      if (s.doc) stateIds(s.doc, ids, depth + 1);
      if (s.state1 && s.state1.doc) stateIds(s.state1.doc, ids, depth + 1);
      if (s.state2 && s.state2.doc) stateIds(s.state2.doc, ids, depth + 1);
    }
  }
  function countNodes(diagram) {
    const db = diagram.db, type = String(diagram.type || '');
    // Subgraphs and composite states are laid out like nodes (clusters).
    if (/^flowchart/.test(type)) return size(call(db, 'getVertices')) + size(call(db, 'getSubGraphs'));
    if (/^state/.test(type)) {
      const ids = new Set();
      const root = call(db, 'getRootDocV2');
      if (root) stateIds(root.doc || root, ids, 0);
      return Math.max(ids.size, size(call(db, 'getStates')));
    }
    if (type === 'sequence') return size(call(db, 'getActors')) + size(call(db, 'getMessages'));
    if (/^class/.test(type)) return size(call(db, 'getClasses')) + size(call(db, 'getNamespaces'));
    if (type === 'er') return size(call(db, 'getEntities'));
    if (type === 'gantt') return size(call(db, 'getTasks'));
    if (type === 'pie') return size(call(db, 'getSections'));
    // Other types: the largest collection the db exposes.
    let n = 0;
    for (const k of ['getVertices', 'getNodes', 'getActors', 'getClasses', 'getStates', 'getEntities',
      'getTasks', 'getSections', 'getItems', 'getData']) n = Math.max(n, size(call(db, k)));
    return n;
  }

  let lastKey = '';
  globalThis.mmRender = async (id, src, optsJSON) => {
    const o = JSON.parse(optsJSON || '{}');
    const key = JSON.stringify([o.theme, o.themeVariables, o.fontFamily, o.maxTextSize]);
    if (key !== lastKey) {
      mermaid.initialize({
        startOnLoad: false,
        securityLevel: 'strict',     // no click handlers, no links out, labels sanitized
        htmlLabels: false,           // SVG <text> labels (no <foreignObject>)
        flowchart: { htmlLabels: false },
        class: { htmlLabels: false },
        deterministicIds: true,
        theme: o.theme === 'dark' ? 'dark' : 'default',
        themeVariables: o.themeVariables || {},
        fontFamily: o.fontFamily || '"trebuchet ms", verdana, arial, sans-serif',
        maxTextSize: o.maxTextSize || 50000,
        secure: SECURE,
        suppressErrorRendering: true,
        logLevel: 5,                 // fatal only
      });
      lastKey = key;
    }
    try {
      // Parse first. A source that does not parse (every other keystroke
      // while one is typed) is an answer, not a failure: resolve with the
      // message (the host keeps the engine; parsing ran only the diagram's
      // own grammar over a fresh db).
      let d;
      try {
        d = await mermaid.mermaidAPI.getDiagramFromText(src);
      } catch (e) {
        return 'CCTEXT_PARSE ' + String(e && e.message ? e.message : e);
      }
      if (o.nodeMax > 0) {
        const n = countNodes(d);
        // A refusal, not a failure: resolve (the host keeps the engine).
        if (n > o.nodeMax) return 'CCTEXT_TOO_LARGE ' + n + ' ' + o.nodeMax;
      }
      const r = await mermaid.render(id, src);
      return r.svg;
    } finally {
      // mermaid leaves its scratch container in <body>: drop it so the
      // document does not grow with every job; forget per-job caches.
      for (const c of document.body.childNodes.slice()) c.remove();
      __cctextDom.reset();
    }
  };

  // Test hooks: only bin/cctext-render-selftest calls these (a Mermaid
  // source "%%cr-selftest:jsloop" / "%%cr-selftest:jsheap N"); the release
  // helper never does. An endless script proves the interrupt handler ends
  // it; a large retained allocation proves the heap recycle.
  globalThis.__cctextSelftestLoop = () => { for (;;) { /* until interrupted */ } };
  globalThis.__cctextSelftestHeap = (mb) => {
    const keep = (globalThis.__cctextKeep = globalThis.__cctextKeep || []);
    for (let i = 0; i < Number(mb); i++) keep.push(new Uint8Array(1 << 20).fill(1));
    return '<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"/>';
  };
}
