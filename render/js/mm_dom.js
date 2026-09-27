// cctext-render: a small handwritten DOM for running the official
// mermaid.min.js in QuickJS (docs/images.md, "Mermaid").
//
// Scope: what mermaid 12 (d3-selection, the dagre / cose / elk layout
// glue, createText with htmlLabels:false) touches while it renders a
// diagram to an SVG string. It is a node tree with a selector engine, an
// XML serializer / parser, inline `style` access and a tiny CSS cascade
// for the inherited text properties that geometry needs. No layout
// engine, no events, no network, no timers (rt_shim.js).
//
// Geometry: getBBox / getComputedTextLength answer from attributes plus
// the host's text metrics (__hostMeasure / __hostFontMetrics: the same
// plutovg faces lunasvg rasterizes with, so boxes fit the drawn text).
//
// DOMPurify is deliberately inert: `document.implementation` has no
// createHTMLDocument, so DOMPurify reports itself unsupported and hands
// strings back unchanged. That is safe here: the SVG never reaches a
// browser; it is rasterized by lunasvg inside the same sandbox, which
// runs no script, follows no link and loads nothing external.
(function () {
  'use strict';
  const g = globalThis;
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const HTML_NS = 'http://www.w3.org/1999/xhtml';
  const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta',
    'source', 'track', 'wbr']);
  const mkText = (doc, n) => (typeof n === 'string' ? doc.createTextNode(n) : n);

  // ---------------------------------------------------------------- Node
  class Node {
    constructor(doc, type, name) {
      this.ownerDocument = doc;
      this.nodeType = type;
      this.nodeName = name;
      this.parentNode = null;
      this.childNodes = [];
    }
    get parentElement() { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }
    get firstChild() { return this.childNodes[0] || null; }
    get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
    get nextSibling() {
      const p = this.parentNode;
      if (!p) return null;
      const c = p.childNodes;
      return c[c.indexOf(this) + 1] || null;
    }
    get previousSibling() {
      const p = this.parentNode;
      if (!p) return null;
      const c = p.childNodes, i = c.indexOf(this);
      return i > 0 ? c[i - 1] : null;
    }
    get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
    get firstElementChild() { return this.children[0] || null; }
    get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
    get childElementCount() { return this.children.length; }
    get nextElementSibling() {
      let n = this.nextSibling;
      while (n && n.nodeType !== 1) n = n.nextSibling;
      return n;
    }
    get previousElementSibling() {
      let n = this.previousSibling;
      while (n && n.nodeType !== 1) n = n.previousSibling;
      return n;
    }
    get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n.nodeType === 9; }
    getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
    hasChildNodes() { return this.childNodes.length > 0; }
    contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }

    // A fragment gives up its children; any other node leaves its parent.
    _adopt(n) {
      if (n.nodeType === 11) {
        const kids = n.childNodes.slice();
        for (const k of kids) k.parentNode = null;
        n.childNodes.length = 0;
        return kids;
      }
      if (n.parentNode) n.parentNode.removeChild(n);
      return [n];
    }
    appendChild(n) {
      for (const k of this._adopt(n)) { k.parentNode = this; this.childNodes.push(k); }
      return n;
    }
    insertBefore(n, ref) {
      if (ref == null) return this.appendChild(n);
      const kids = this._adopt(n), i = this.childNodes.indexOf(ref);
      if (i < 0) throw new Error('insertBefore: reference is not a child');
      for (const k of kids) k.parentNode = this;
      this.childNodes.splice(i, 0, ...kids);
      return n;
    }
    removeChild(n) {
      const i = this.childNodes.indexOf(n);
      if (i < 0) throw new Error('removeChild: not a child');
      this.childNodes.splice(i, 1);
      n.parentNode = null;
      return n;
    }
    replaceChild(n, old) { this.insertBefore(n, old); return this.removeChild(old); }
    remove() { if (this.parentNode) this.parentNode.removeChild(this); }
    append(...ns) { for (const n of ns) this.appendChild(mkText(this.ownerDocument, n)); }
    prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(mkText(this.ownerDocument, n), f); }
    before(...ns) {
      const p = this.parentNode;
      if (p) for (const n of ns) p.insertBefore(mkText(this.ownerDocument, n), this);
    }
    after(...ns) {
      const p = this.parentNode;
      if (!p) return;
      const r = this.nextSibling;
      for (const n of ns) p.insertBefore(mkText(this.ownerDocument, n), r);
    }
    replaceWith(...ns) { this.before(...ns); this.remove(); }
    get textContent() {
      if (this.nodeType === 3 || this.nodeType === 8) return this.data;
      let s = '';
      for (const c of this.childNodes) if (c.nodeType !== 8) s += c.textContent;
      return s;
    }
    set textContent(v) {
      if (this.nodeType === 3 || this.nodeType === 8) { this.data = String(v); return; }
      for (const c of this.childNodes) c.parentNode = null;
      this.childNodes = [];
      if (v != null && v !== '') this.appendChild(this.ownerDocument.createTextNode(String(v)));
    }
    get nodeValue() { return this.nodeType === 3 || this.nodeType === 8 ? this.data : null; }
    set nodeValue(v) { if (this.nodeType === 3 || this.nodeType === 8) this.data = String(v); }
    addEventListener() {}
    removeEventListener() {}
    dispatchEvent() { return true; }
    // Enough for d3's selection.order(): 4 = `o` follows this node.
    compareDocumentPosition(o) {
      if (o === this) return 0;
      const path = (n) => { const a = []; for (; n; n = n.parentNode) a.unshift(n); return a; };
      const a = path(this), b = path(o);
      let i = 0;
      while (i < a.length && i < b.length && a[i] === b[i]) i++;
      if (i === a.length) return 20; // o is inside this (CONTAINED_BY | FOLLOWING)
      if (i === b.length) return 10; // this is inside o (CONTAINS | PRECEDING)
      const p = a[i - 1];
      if (!p) return 1;             // different trees
      return p.childNodes.indexOf(a[i]) < p.childNodes.indexOf(b[i]) ? 4 : 2;
    }
    cloneNode(deep) {
      let n;
      if (this.nodeType === 1) {
        n = new Element(this.ownerDocument, this.namespaceURI, this.tagName);
        for (const a of this._attrs) n._attrs.push({ name: a.name, value: a.value, namespaceURI: a.namespaceURI });
      } else if (this.nodeType === 3) n = new Text(this.ownerDocument, this.data);
      else if (this.nodeType === 8) n = new Comment(this.ownerDocument, this.data);
      else if (this.nodeType === 11) n = new DocumentFragment(this.ownerDocument);
      else n = new Node(this.ownerDocument, this.nodeType, this.nodeName);
      if (deep) for (const c of this.childNodes) n.appendChild(c.cloneNode(true));
      return n;
    }
  }
  Object.assign(Node, {
    ELEMENT_NODE: 1, TEXT_NODE: 3, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_FRAGMENT_NODE: 11,
    DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_PRECEDING: 2,
  });

  class Text extends Node {
    constructor(doc, s) { super(doc, 3, '#text'); this.data = String(s); }
    get wholeText() { return this.data; }
    get length() { return this.data.length; }
  }
  class Comment extends Node {
    constructor(doc, s) { super(doc, 8, '#comment'); this.data = String(s); }
  }
  class DocumentFragment extends Node {
    constructor(doc) { super(doc, 11, '#document-fragment'); }
    querySelector(s) { return qsa(this, s, true)[0] || null; }
    querySelectorAll(s) { return qsa(this, s, false); }
    getElementById(id) { return findFirst(this, (e) => e.getAttribute('id') === id); }
  }

  // ------------------------------------------------ inline style (el.style)
  const camelToKebab = (p) => p.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());
  // "a: b; c: d !important" -> Map(prop -> {v, prio})
  function parseStyle(s) {
    const m = new Map();
    if (!s) return m;
    for (const decl of String(s).split(';')) {
      const i = decl.indexOf(':');
      if (i < 0) continue;
      const k = decl.slice(0, i).trim().toLowerCase();
      let v = decl.slice(i + 1).trim(), prio = '';
      if (/!important$/i.test(v)) { prio = 'important'; v = v.replace(/\s*!important$/i, ''); }
      if (k) m.set(k, { v, prio });
    }
    return m;
  }
  // CSSStyleDeclaration over the element's `style` attribute (the
  // attribute stays the one truth: the serializer writes it).
  function styleProxy(el) {
    const read = () => parseStyle(el.getAttribute('style'));
    const write = (m) => {
      let s = '';
      for (const [k, { v, prio }] of m) s += `${k}:${v}${prio ? ' !important' : ''};`;
      if (s) el.setAttribute('style', s); else el.removeAttribute('style');
    };
    const api = {
      getPropertyValue: (k) => { const e = read().get(String(k).toLowerCase()); return e ? e.v : ''; },
      getPropertyPriority: (k) => { const e = read().get(String(k).toLowerCase()); return e ? e.prio : ''; },
      setProperty: (k, v, prio) => {
        const m = read();
        k = String(k).toLowerCase();
        if (v == null || v === '') m.delete(k); else m.set(k, { v: String(v), prio: prio || '' });
        write(m);
      },
      removeProperty: (k) => {
        const m = read(), key = String(k).toLowerCase(), e = m.get(key);
        m.delete(key);
        write(m);
        return e ? e.v : '';
      },
      item: (i) => [...read().keys()][i] || '',
    };
    return new Proxy(api, {
      get(t, p) {
        if (p in t) return t[p];
        if (p === 'cssText') return el.getAttribute('style') || '';
        if (p === 'length') return read().size;
        if (typeof p === 'string') return t.getPropertyValue(camelToKebab(p));
        return undefined;
      },
      set(t, p, v) {
        if (p === 'cssText') { el.setAttribute('style', String(v)); return true; }
        t.setProperty(camelToKebab(String(p)), v);
        return true;
      },
    });
  }

  // ------------------------------------------------------------- Element
  class Element extends Node {
    constructor(doc, ns, name) {
      super(doc, 1, ns === HTML_NS || ns == null ? String(name).toUpperCase() : String(name));
      this.namespaceURI = ns == null ? HTML_NS : ns;
      this.localName = String(name).replace(/^.*:/, '');
      if (this.namespaceURI === HTML_NS) this.localName = this.localName.toLowerCase();
      this._attrs = [];
      this._style = null;
    }
    get tagName() { return this.nodeName; }
    get attributes() {
      const a = this._attrs.map((x) => ({
        name: x.name, localName: x.name.replace(/^.*:/, ''), value: x.value, nodeName: x.name,
        namespaceURI: x.namespaceURI || null,
      }));
      a.item = (i) => a[i] || null;
      a.getNamedItem = (n) => a.find((x) => x.name === n) || null;
      return a;
    }
    _find(name) { for (const a of this._attrs) if (a.name === name) return a; return null; }
    getAttribute(n) { const a = this._find(String(n)); return a ? a.value : null; }
    getAttributeNS(ns, n) {
      const a = this._find(String(n)) ||
        this._attrs.find((x) => x.name.replace(/^.*:/, '') === n && x.namespaceURI === ns);
      return a ? a.value : null;
    }
    setAttribute(n, v) {
      n = String(n);
      const a = this._find(n);
      if (a) a.value = String(v); else this._attrs.push({ name: n, value: String(v), namespaceURI: null });
    }
    setAttributeNS(ns, n, v) {
      const a = this._find(String(n));
      if (a) a.value = String(v); else this._attrs.push({ name: String(n), value: String(v), namespaceURI: ns });
    }
    removeAttribute(n) {
      const i = this._attrs.findIndex((a) => a.name === n);
      if (i >= 0) this._attrs.splice(i, 1);
    }
    removeAttributeNS(ns, n) {
      const i = this._attrs.findIndex((a) => a.name === n || (a.name.replace(/^.*:/, '') === n && a.namespaceURI === ns));
      if (i >= 0) this._attrs.splice(i, 1);
    }
    hasAttribute(n) { return !!this._find(String(n)); }
    hasAttributeNS(ns, n) { return this.getAttributeNS(ns, n) != null; }
    hasAttributes() { return this._attrs.length > 0; }
    getAttributeNames() { return this._attrs.map((a) => a.name); }
    toggleAttribute(n, f) {
      const has = this.hasAttribute(n);
      if (f === undefined) f = !has;
      if (f && !has) this.setAttribute(n, '');
      if (!f && has) this.removeAttribute(n);
      return f;
    }
    get id() { return this.getAttribute('id') || ''; }
    set id(v) { this.setAttribute('id', v); }
    get className() { return this.getAttribute('class') || ''; }
    set className(v) { this.setAttribute('class', v); }
    get classList() {
      const el = this;
      const get = () => (el.getAttribute('class') || '').split(/\s+/).filter(Boolean);
      const set = (l) => el.setAttribute('class', l.join(' '));
      return {
        add: (...c) => { const l = get(); for (const x of c) if (!l.includes(x)) l.push(x); set(l); },
        remove: (...c) => set(get().filter((x) => !c.includes(x))),
        contains: (c) => get().includes(c),
        toggle: (c, f) => {
          const l = get(), has = l.includes(c);
          if (f === undefined) f = !has;
          if (f && !has) l.push(c);
          if (!f && has) l.splice(l.indexOf(c), 1);
          set(l);
          return f;
        },
        get length() { return get().length; },
        item: (i) => get()[i] || null,
        toString: () => el.getAttribute('class') || '',
        [Symbol.iterator]: () => get()[Symbol.iterator](),
      };
    }
    get style() { return this._style || (this._style = styleProxy(this)); }
    set style(v) { this.setAttribute('style', String(v)); }
    get dataset() {
      const el = this;
      return new Proxy({}, {
        get: (t, p) => el.getAttribute('data-' + camelToKebab(String(p))) ?? undefined,
        set: (t, p, v) => { el.setAttribute('data-' + camelToKebab(String(p)), v); return true; },
      });
    }
    get innerHTML() { let s = ''; for (const c of this.childNodes) s += serialize(c); return s; }
    set innerHTML(v) { this.textContent = ''; parseInto(this, String(v)); }
    get outerHTML() { return serialize(this); }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    insertAdjacentHTML(pos, html) {
      const tmp = new Element(this.ownerDocument, this.namespaceURI, 'div');
      parseInto(tmp, html);
      const f = new DocumentFragment(this.ownerDocument);
      for (const k of tmp.childNodes.slice()) f.appendChild(k);
      if (pos === 'beforeend') this.appendChild(f);
      else if (pos === 'afterbegin') this.insertBefore(f, this.firstChild);
      else if (pos === 'beforebegin') this.parentNode.insertBefore(f, this);
      else if (pos === 'afterend') this.parentNode.insertBefore(f, this.nextSibling);
    }
    querySelector(s) { return qsa(this, s, true)[0] || null; }
    querySelectorAll(s) { return qsa(this, s, false); }
    getElementsByTagName(t) {
      t = t.toLowerCase();
      return collect(this, (e) => t === '*' || e.localName.toLowerCase() === t);
    }
    getElementsByClassName(c) {
      return collect(this, (e) => (' ' + (e.getAttribute('class') || '') + ' ').replace(/\s+/g, ' ').includes(' ' + c + ' '));
    }
    matches(s) { return parseSelector(s).some((sel) => matchComplex(this, sel, null)); }
    closest(s) {
      const sels = parseSelector(s);
      for (let e = this; e && e.nodeType === 1; e = e.parentNode)
        if (sels.some((sel) => matchComplex(e, sel, null))) return e;
      return null;
    }
    focus() {}
    blur() {}
    click() {}

    // ---- geometry (SVG): see `geom` below
    getBBox() { return geom.bbox(this) || { x: 0, y: 0, width: 0, height: 0 }; }
    getComputedTextLength() { return geom.measure(this.textContent, geom.fontOf(this)); }
    getSubStringLength(i, n) { return geom.measure(this.textContent.substr(i, n), geom.fontOf(this)); }
    getBoundingClientRect() {
      const b = this.getBBox();
      return { x: b.x, y: b.y, width: b.width, height: b.height, left: b.x, top: b.y, right: b.x + b.width, bottom: b.y + b.height };
    }
    getClientRects() { return [this.getBoundingClientRect()]; }
    getTotalLength() { return geom.pathLength(this.getAttribute('d') || ''); }
    getPointAtLength(l) { return geom.pointAtLength(this.getAttribute('d') || '', l); }
    getCTM() { return { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 }; }
    getScreenCTM() { return this.getCTM(); }
    // HTML blocks: Chromium's default 1280 px viewport minus the body's
    // 8 px margins (what mermaid's reference renders measured against).
    get offsetWidth() { return this.namespaceURI === HTML_NS ? 1264 : undefined; }
    get offsetHeight() { return this.namespaceURI === HTML_NS ? 0 : undefined; }
    get clientWidth() { return this.namespaceURI === HTML_NS ? 1264 : 0; }
    get clientHeight() { return 0; }
    get sheet() { return this.localName === 'style' ? { cssRules: [], insertRule() { return 0; } } : null; }
  }

  class Document extends Node {
    constructor() {
      super(null, 9, '#document');
      this.ownerDocument = null;
      this.documentElement = this.createElement('html');
      this.appendChild(this.documentElement);
      this.head = this.createElement('head');
      this.body = this.createElement('body');
      this.documentElement.appendChild(this.head);
      this.documentElement.appendChild(this.body);
      this.implementation = { hasFeature: () => false }; // no createHTMLDocument: DOMPurify stays inert
      this.readyState = 'complete';
      this.contentType = 'text/html';
      this.defaultView = g;
    }
    createElement(t) { return new Element(this, HTML_NS, t); }
    createElementNS(ns, t) { return new Element(this, ns, t); }
    createTextNode(s) { return new Text(this, s); }
    createComment(s) { return new Comment(this, s); }
    createDocumentFragment() { return new DocumentFragment(this); }
    createRange() {
      const d = this;
      return {
        selectNodeContents() {},
        createContextualFragment(h) {
          const t = d.createElement('template');
          parseInto(t, h);
          const f = d.createDocumentFragment();
          for (const k of t.childNodes.slice()) f.appendChild(k);
          return f;
        },
      };
    }
    getElementById(id) { return findFirst(this, (e) => e.getAttribute('id') === id); }
    querySelector(s) { return qsa(this, s, true)[0] || null; }
    querySelectorAll(s) { return qsa(this, s, false); }
    getElementsByTagName(t) { return this.documentElement.getElementsByTagName(t); }
    getElementsByClassName(c) { return this.documentElement.getElementsByClassName(c); }
    get children() { return [this.documentElement]; }
  }

  function findFirst(root, pred) {
    for (const c of root.childNodes) {
      if (c.nodeType !== 1) continue;
      if (pred(c)) return c;
      const r = findFirst(c, pred);
      if (r) return r;
    }
    return null;
  }
  function collect(root, pred, out = []) {
    for (const c of root.childNodes) {
      if (c.nodeType !== 1) continue;
      if (pred(c)) out.push(c);
      collect(c, pred, out);
    }
    return out;
  }

  // ------------------------------------------------------------ selectors
  // Compounds (tag, #id, .class, [attr op value], :scope / :first-child /
  // :last-child / :not(...)), the descendant / > / + / ~ combinators and
  // comma lists. Parsed selectors are cached (mermaid repeats a few).
  const selCache = new Map();
  function parseSelector(src) {
    src = String(src).trim();
    if (selCache.has(src)) return selCache.get(src);
    const list = [];
    let i = 0, cur = [], comb = ' ';
    const ident = () => {
      const m = /^(?:\\.|[\w -￿-])+/.exec(src.slice(i));
      if (!m) throw new SyntaxError('bad selector: ' + src);
      i += m[0].length;
      return m[0].replace(/\\(.)/g, '$1');
    };
    while (i < src.length) {
      const comp = { tag: null, id: null, cls: [], attrs: [], pseudo: [] };
      let any = false;
      for (;;) {
        const c = src[i];
        if (c === '*') { i++; any = true; }
        else if (c === '#') { i++; comp.id = ident(); any = true; }
        else if (c === '.') { i++; comp.cls.push(ident()); any = true; }
        else if (c === '[') {
          const j = src.indexOf(']', i);
          if (j < 0) throw new SyntaxError('bad attribute selector: ' + src);
          const body = src.slice(i + 1, j);
          i = j + 1;
          any = true;
          const m = /^\s*([\w:-]+)\s*(?:([~^$*|]?=)\s*(?:"([^"]*)"|'([^']*)'|([^\s\]]*)))?\s*$/.exec(body);
          if (!m) throw new SyntaxError('bad attribute selector: ' + src);
          comp.attrs.push({ name: m[1], op: m[2] || null, val: m[3] ?? m[4] ?? m[5] ?? '' });
        } else if (c === ':') {
          i++;
          if (src[i] === ':') i++;
          const name = ident();
          let arg = null;
          if (src[i] === '(') {
            let d = 1, j = i + 1;
            for (; j < src.length && d; j++) { if (src[j] === '(') d++; else if (src[j] === ')') d--; }
            arg = src.slice(i + 1, j - 1);
            i = j;
          }
          comp.pseudo.push({ name, arg });
          any = true;
        } else if (c && /[\w-]/.test(c) && !any) { comp.tag = ident().toLowerCase(); any = true; }
        else break;
      }
      if (!any) throw new SyntaxError('bad selector: ' + src);
      cur.push({ comb, comp });
      let ws = false;
      while (src[i] === ' ' || src[i] === '\n' || src[i] === '\t') { i++; ws = true; }
      if (src[i] === '>' || src[i] === '+' || src[i] === '~') {
        comb = src[i++];
        while (src[i] === ' ') i++;
      } else if (src[i] === ',') {
        i++;
        while (src[i] === ' ') i++;
        list.push(cur);
        cur = [];
        comb = ' ';
      } else if (ws) comb = ' ';
    }
    if (cur.length) list.push(cur);
    selCache.set(src, list);
    return list;
  }
  function matchCompound(e, c, scope) {
    if (e.nodeType !== 1) return false;
    if (c.tag && c.tag !== e.localName.toLowerCase()) return false;
    if (c.id != null && e.getAttribute('id') !== c.id) return false;
    if (c.cls.length) {
      const l = (e.getAttribute('class') || '').split(/\s+/);
      for (const x of c.cls) if (!l.includes(x)) return false;
    }
    for (const a of c.attrs) {
      const v = e.getAttribute(a.name);
      if (v == null) return false;
      if (a.op === '=' && v !== a.val) return false;
      if (a.op === '^=' && !v.startsWith(a.val)) return false;
      if (a.op === '$=' && !v.endsWith(a.val)) return false;
      if (a.op === '*=' && !v.includes(a.val)) return false;
      if (a.op === '~=' && !v.split(/\s+/).includes(a.val)) return false;
      if (a.op === '|=' && !(v === a.val || v.startsWith(a.val + '-'))) return false;
    }
    for (const p of c.pseudo) {
      if (p.name === 'scope') { if (e !== scope) return false; }
      else if (p.name === 'first-child') { if (e.parentNode && e.parentNode.children[0] !== e) return false; }
      else if (p.name === 'last-child') {
        const k = e.parentNode ? e.parentNode.children : [e];
        if (k[k.length - 1] !== e) return false;
      } else if (p.name === 'not') { if (parseSelector(p.arg).some((s) => matchComplex(e, s, scope))) return false; }
      else return false; // unsupported pseudo-classes never match
    }
    return true;
  }
  // Right to left: the element matches the last compound, then each
  // combinator walks toward the root.
  function matchComplex(e, parts, scope) {
    const k = parts.length - 1;
    if (!matchCompound(e, parts[k].comp, scope)) return false;
    const rec = (el, k) => {
      if (k < 0) return true;
      const comb = parts[k + 1].comb, c = parts[k].comp;
      if (comb === '>') { const p = el.parentNode; return !!p && matchCompound(p, c, scope) && rec(p, k - 1); }
      if (comb === '+') { const p = el.previousElementSibling; return !!p && matchCompound(p, c, scope) && rec(p, k - 1); }
      if (comb === '~') {
        for (let p = el.previousElementSibling; p; p = p.previousElementSibling)
          if (matchCompound(p, c, scope) && rec(p, k - 1)) return true;
        return false;
      }
      for (let p = el.parentNode; p && p.nodeType === 1; p = p.parentNode)
        if (matchCompound(p, c, scope) && rec(p, k - 1)) return true;
      return false;
    };
    return rec(e, k - 1);
  }
  function qsa(root, s, first) {
    const sels = parseSelector(s), out = [];
    const scope = root.nodeType === 1 ? root : null;
    const walk = (n) => {
      for (const c of n.childNodes) {
        if (c.nodeType !== 1) continue;
        if (sels.some((sel) => matchComplex(c, sel, scope))) {
          out.push(c);
          if (first) return true;
        }
        if (walk(c)) return true;
      }
      return false;
    };
    walk(root);
    return out;
  }

  // ---------------------------------------------------- serializer / parser
  const escText = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const escAttr = (s) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  // XML output (lunasvg reads it): SVG elements self-close when empty.
  function serialize(n) {
    if (n.nodeType === 3) return escText(n.data);
    if (n.nodeType === 8) return '<!--' + n.data + '-->';
    if (n.nodeType === 11 || n.nodeType === 9) { let s = ''; for (const c of n.childNodes) s += serialize(c); return s; }
    const tag = n.namespaceURI === HTML_NS ? n.localName : n.nodeName;
    let s = '<' + tag;
    for (const a of n._attrs) s += ' ' + a.name + '="' + escAttr(a.value) + '"';
    if (!n.childNodes.length) return s + (n.namespaceURI === HTML_NS && !VOID.has(tag) ? '></' + tag + '>' : '/>');
    s += '>';
    for (const c of n.childNodes) s += serialize(c);
    return s + '</' + tag + '>';
  }
  const ENT = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' };
  const decode = (s) => s.replace(/&(#x[0-9a-f]+|#\d+|\w+);/gi, (m, e) => {
    if (e[0] !== '#') return ENT[e] ?? m;
    const cp = e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
    return cp >= 0 && cp <= 0x10ffff ? String.fromCodePoint(cp) : '�';
  });
  // A forgiving tag-soup parser for innerHTML (mermaid builds a few
  // fragments from strings): elements, attributes, text, comments;
  // <style> / <script> bodies are raw text. Never throws.
  function parseInto(parent, html) {
    const doc = parent.ownerDocument || parent;
    let cur = parent, i = 0;
    const nsFor = (tag, p) => (tag === 'svg' ? SVG_NS
      : (p.namespaceURI === SVG_NS && p.localName !== 'foreignObject' ? SVG_NS : HTML_NS));
    while (i < html.length) {
      const lt = html.indexOf('<', i);
      if (lt < 0 || lt > i) {
        const t = html.slice(i, lt < 0 ? html.length : lt);
        if (t) cur.appendChild(doc.createTextNode(decode(t)));
        if (lt < 0) break;
        i = lt;
      }
      if (html.startsWith('<!--', i)) {
        const e = html.indexOf('-->', i);
        cur.appendChild(doc.createComment(html.slice(i + 4, e < 0 ? html.length : e)));
        i = e < 0 ? html.length : e + 3;
        continue;
      }
      if (html.startsWith('<!', i) || html.startsWith('<?', i)) {
        const e = html.indexOf('>', i);
        i = e < 0 ? html.length : e + 1;
        continue;
      }
      if (html[i + 1] === '/') {
        const e = html.indexOf('>', i);
        const name = html.slice(i + 2, e < 0 ? html.length : e).trim().toLowerCase();
        for (let n = cur; n && n !== parent; n = n.parentNode)
          if (n.localName.toLowerCase() === name) { cur = n.parentNode; break; }
        i = e < 0 ? html.length : e + 1;
        continue;
      }
      const m = /^<([A-Za-z][\w:.-]*)/.exec(html.slice(i, i + 200));
      if (!m) { cur.appendChild(doc.createTextNode('<')); i++; continue; }
      i += m[0].length;
      const el = doc.createElementNS(nsFor(m[1].toLowerCase(), cur), m[1]);
      const re = /\s*([^\s=\/>]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?|\s*(\/?)>/y;
      let selfClose = false;
      for (;;) {
        re.lastIndex = i;
        const a = re.exec(html);
        if (!a) { i = html.length; break; }
        i = re.lastIndex;
        if (a[0].trim().endsWith('>') && a[1] === undefined) { selfClose = a[5] === '/'; break; }
        if (a[1] === '/') continue;
        el.setAttribute(a[1], decode(a[2] ?? a[3] ?? a[4] ?? ''));
      }
      cur.appendChild(el);
      const ln = el.localName.toLowerCase();
      if (ln === 'style' || ln === 'script') {
        const e = html.toLowerCase().indexOf('</' + ln, i);
        const t = html.slice(i, e < 0 ? html.length : e);
        if (t) el.appendChild(doc.createTextNode(t));
        i = e < 0 ? html.length : html.indexOf('>', e) + 1;
        if (i <= 0) i = html.length;
        continue;
      }
      if (!selfClose && !(el.namespaceURI === HTML_NS && VOID.has(ln))) cur = el;
    }
  }

  // ------------------------------------- tiny CSS cascade (text properties)
  // Only the inherited properties text geometry needs. Rules come from
  // the <style> children of the enclosing <svg> elements (where mermaid
  // puts its theme CSS); @-rules are skipped.
  const CSS_PROPS = new Set(['font-size', 'font-weight', 'font-family', 'font-style', 'text-anchor',
    'dominant-baseline', 'alignment-baseline']);
  const sheetCache = new Map();
  function parseSheet(css) {
    if (sheetCache.has(css)) return sheetCache.get(css);
    const rules = [];
    let i = 0, order = 0;
    css = css.replace(/\/\*[\s\S]*?\*\//g, '');
    while (i < css.length) {
      const ob = css.indexOf('{', i);
      if (ob < 0) break;
      const pre = css.slice(i, ob).trim();
      let d = 1, j = ob + 1;
      for (; j < css.length && d; j++) { if (css[j] === '{') d++; else if (css[j] === '}') d--; }
      const body = css.slice(ob + 1, j - 1);
      i = j;
      if (pre.startsWith('@')) continue; // @keyframes / @media / @font-face
      const decls = [...parseStyle(body)].filter(([k]) => CSS_PROPS.has(k));
      if (!decls.length) continue;
      for (const sel of pre.split(',')) {
        let parsed;
        try { parsed = parseSelector(sel); } catch (e) { continue; }
        for (const cx of parsed) {
          let a = 0, b = 0, c = 0;
          for (const { comp } of cx) {
            if (comp.id != null) a++;
            b += comp.cls.length + comp.attrs.length + comp.pseudo.length;
            if (comp.tag) c++;
          }
          rules.push({ cx, spec: a * 1e6 + b * 1e3 + c, order: order++, decls });
        }
      }
    }
    sheetCache.set(css, rules);
    return rules;
  }
  // Rules indexed by their rightmost compound (id / first class / tag /
  // universal) so an element only tries the rules that could match it.
  const indexCache = new Map();
  let styleSerial = 0;
  function sheetsFor(el) {
    const styles = [];
    for (let e = el; e && e.nodeType === 1; e = e.parentNode)
      if (e.localName === 'svg')
        for (const c of e.childNodes) if (c.nodeType === 1 && c.localName === 'style') styles.push(c);
    let key = '';
    for (const st of styles) {
      if (!st._sid) st._sid = ++styleSerial;
      const t = st.textContent;
      key += st._sid + ':' + t.length + ':' + t.slice(-16) + ';';
    }
    let idx = indexCache.get(key);
    if (!idx) {
      idx = { id: new Map(), cls: new Map(), tag: new Map(), any: [] };
      const add = (m, k, r) => { let l = m.get(k); if (!l) m.set(k, (l = [])); l.push(r); };
      let order = 0;
      for (const st of styles) {
        for (const r of parseSheet(st.textContent)) {
          const rr = { ...r, order: order++ }, c = r.cx[r.cx.length - 1].comp;
          if (c.id != null) add(idx.id, c.id, rr);
          else if (c.cls.length) add(idx.cls, c.cls[0], rr);
          else if (c.tag) add(idx.tag, c.tag, rr);
          else idx.any.push(rr);
        }
      }
      if (indexCache.size > 8) indexCache.clear();
      indexCache.set(key, idx);
    }
    return idx;
  }
  function styleOf(e, idx, memo) {
    let m = memo.get(e);
    if (m) return m;
    m = new Map();
    const cands = [...(idx.id.get(e.getAttribute('id')) || []), ...idx.any, ...(idx.tag.get(e.localName.toLowerCase()) || [])];
    for (const c of (e.getAttribute('class') || '').split(/\s+/)) if (c) cands.push(...(idx.cls.get(c) || []));
    for (const r of cands) {
      if (!matchComplex(e, r.cx, null)) continue;
      for (const [k, d] of r.decls) {
        const b = m.get(k);
        if (!b || r.spec > b.spec || (r.spec === b.spec && r.order > b.order)) m.set(k, { spec: r.spec, order: r.order, v: d.v });
      }
    }
    memo.set(e, m);
    return m;
  }
  // An inherited text property: inline style, then the stylesheet, then
  // the presentation attribute; nearest ancestor first.
  function textProp(el, prop, ctx) {
    for (let e = el; e && e.nodeType === 1; e = e.parentNode) {
      const inl = parseStyle(e.getAttribute('style')).get(prop);
      if (inl && inl.v) return inl.v;
      const c = styleOf(e, ctx.idx, ctx.memo).get(prop);
      if (c) return c.v;
      const at = e.getAttribute(prop);
      if (at) return at;
    }
    return null;
  }

  // ------------------------------------------------------------- geometry
  const geom = {};
  geom.fontOf = function (el) {
    if (el && el.nodeType !== 1) el = el.parentNode;
    const rules = { idx: sheetsFor(el), memo: new Map() };
    const fs = textProp(el, 'font-size', rules), fw = textProp(el, 'font-weight', rules);
    const ff = textProp(el, 'font-family', rules), fst = textProp(el, 'font-style', rules);
    let px = 16;
    if (fs) {
      const f = parseFloat(fs);
      if (!isNaN(f)) px = /em$/.test(fs) ? f * 16 : /pt$/.test(fs) ? (f * 4) / 3 : f;
    }
    const weight = !fw ? 400 : /bold/.test(fw) ? 700 : parseInt(fw, 10) || 400;
    return { px, weight, family: ff || 'sans-serif', italic: !!fst && /italic|oblique/.test(fst), rules };
  };
  // Advance width in px: the host's faces, else a rough per-character guess.
  geom.measure = function (text, f) {
    text = String(text || '');
    if (typeof g.__hostMeasure === 'function') return g.__hostMeasure(text, f.px, f.weight, f.family, f.italic);
    let w = 0;
    for (const ch of text) {
      const c = ch.codePointAt(0);
      w += c >= 0x2e80 && c <= 0xffef ? 1.0 : c === 32 ? 0.26 : 0.55;
    }
    return w * f.px;
  };
  const num = (el, a) => { const v = parseFloat(el.getAttribute(a)); return isNaN(v) ? 0 : v; };
  function union(a, b) {
    if (!a) return b;
    if (!b) return a;
    const x = Math.min(a.x, b.x), y = Math.min(a.y, b.y);
    return { x, y, width: Math.max(a.x + a.width, b.x + b.width) - x, height: Math.max(a.y + a.height, b.y + b.height) - y };
  }
  // The `transform` attribute as a matrix [a b c d e f] (translate, scale,
  // rotate about a point, matrix; skews are rare in mermaid output).
  function transformOf(el) {
    const t = el.getAttribute && el.getAttribute('transform');
    if (!t) return null;
    let m = [1, 0, 0, 1, 0, 0];
    const mul = (a, b) => [a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3],
      a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]];
    for (const f of t.matchAll(/(\w+)\s*\(([^)]*)\)/g)) {
      const v = f[2].split(/[\s,]+/).filter(Boolean).map(Number);
      if (f[1] === 'translate') m = mul(m, [1, 0, 0, 1, v[0] || 0, v[1] || 0]);
      else if (f[1] === 'scale') m = mul(m, [v[0], 0, 0, v[1] ?? v[0], 0, 0]);
      else if (f[1] === 'rotate') {
        const r = ((v[0] || 0) * Math.PI) / 180, c = Math.cos(r), s = Math.sin(r), cx = v[1] || 0, cy = v[2] || 0;
        m = mul(m, [1, 0, 0, 1, cx, cy]);
        m = mul(m, [c, s, -s, c, 0, 0]);
        m = mul(m, [1, 0, 0, 1, -cx, -cy]);
      } else if (f[1] === 'matrix' && v.length === 6) m = mul(m, v);
    }
    return m;
  }
  function applyT(m, b) {
    if (!m || !b) return b;
    const pts = [[b.x, b.y], [b.x + b.width, b.y], [b.x, b.y + b.height], [b.x + b.width, b.y + b.height]]
      .map(([x, y]) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]]);
    const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
    return { x: Math.min(...xs), y: Math.min(...ys), width: Math.max(...xs) - Math.min(...xs), height: Math.max(...ys) - Math.min(...ys) };
  }
  // Path data -> polylines: M L H V C S Q T A Z, absolute and relative.
  // Curves are sampled (12 steps); arcs go through the SVG 1.1 F.6.5
  // centre form and are sampled too, since getBBox must include the
  // bulge (cylinders, rounded boxes).
  function flatten(d) {
    const t = String(d).match(/[a-zA-Z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?/gi) || [];
    const N = { M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2, A: 7, Z: 0 };
    const polys = [];
    let pts = null, i = 0, cmd = '', x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0, qx = 0, qy = 0;
    const push = (px, py) => { if (!pts) { pts = [[x, y]]; polys.push(pts); } pts.push([px, py]); };
    const cubic = (x1, y1, x2, y2, ex, ey) => {
      for (let k = 1; k <= 12; k++) {
        const s = k / 12, u = 1 - s;
        push(u * u * u * x + 3 * u * u * s * x1 + 3 * u * s * s * x2 + s * s * s * ex,
          u * u * u * y + 3 * u * u * s * y1 + 3 * u * s * s * y2 + s * s * s * ey);
      }
    };
    const arc = (rx, ry, phi, fa, fs, ex, ey) => {
      rx = Math.abs(rx);
      ry = Math.abs(ry);
      if (!rx || !ry || (x === ex && y === ey)) { push(ex, ey); return; }
      const p = (phi * Math.PI) / 180, cp = Math.cos(p), sp = Math.sin(p);
      const dx = (x - ex) / 2, dy = (y - ey) / 2, x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
      const L = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
      if (L > 1) { rx *= Math.sqrt(L); ry *= Math.sqrt(L); }
      let k = (rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1) / (rx * rx * y1 * y1 + ry * ry * x1 * x1);
      k = Math.sqrt(Math.max(0, k)) * (fa == fs ? -1 : 1); // eslint-disable-line eqeqeq
      const cxp = (k * rx * y1) / ry, cyp = (-k * ry * x1) / rx;
      const ccx = cp * cxp - sp * cyp + (x + ex) / 2, ccy = sp * cxp + cp * cyp + (y + ey) / 2;
      const ang = (ux, uy, vx, vy) => Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
      const t1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
      let dt = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
      if (!+fs && dt > 0) dt -= 2 * Math.PI;
      else if (+fs && dt < 0) dt += 2 * Math.PI;
      for (let j = 1; j <= 16; j++) {
        const th = t1 + (dt * j) / 16;
        push(ccx + cp * rx * Math.cos(th) - sp * ry * Math.sin(th), ccy + sp * rx * Math.cos(th) + cp * ry * Math.sin(th));
      }
    };
    while (i < t.length) {
      if (/[a-z]/i.test(t[i])) {
        cmd = t[i++];
        if (/z/i.test(cmd)) { if (pts) push(sx, sy); x = sx; y = sy; pts = null; continue; }
      }
      const U = cmd.toUpperCase(), rel = cmd !== U, n = N[U];
      if (n === undefined) { i++; continue; }
      const a = t.slice(i, i + n).map(Number);
      i += n;
      if (a.length < n || a.some(isNaN)) break;
      const ox = rel ? x : 0, oy = rel ? y : 0;
      if (U === 'M') {
        x = ox + a[0]; y = oy + a[1]; sx = x; sy = y;
        pts = [[x, y]];
        polys.push(pts);
        cmd = rel ? 'l' : 'L'; // implicit lineto after a moveto
        cx = x; cy = y;
        continue;
      }
      if (U === 'L') { push(ox + a[0], oy + a[1]); x = ox + a[0]; y = oy + a[1]; cx = x; cy = y; }
      else if (U === 'H') { push(ox + a[0], y); x = ox + a[0]; cx = x; cy = y; }
      else if (U === 'V') { push(x, oy + a[0]); y = oy + a[0]; cx = x; cy = y; }
      else if (U === 'C') {
        cubic(ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]);
        cx = ox + a[2]; cy = oy + a[3]; x = ox + a[4]; y = oy + a[5];
      } else if (U === 'S') {
        const x1 = 2 * x - cx, y1 = 2 * y - cy;
        cubic(x1, y1, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
        cx = ox + a[0]; cy = oy + a[1]; x = ox + a[2]; y = oy + a[3];
      } else if (U === 'Q' || U === 'T') {
        const [x1, y1] = U === 'Q' ? [ox + a[0], oy + a[1]] : [2 * x - qx, 2 * y - qy];
        const ex = ox + a[n - 2], ey = oy + a[n - 1];
        cubic(x + (2 / 3) * (x1 - x), y + (2 / 3) * (y1 - y), ex + (2 / 3) * (x1 - ex), ey + (2 / 3) * (y1 - ey), ex, ey);
        qx = x1; qy = y1; x = ex; y = ey; cx = x; cy = y;
        continue;
      } else if (U === 'A') {
        const ex = ox + a[5], ey = oy + a[6];
        arc(a[0], a[1], a[2], a[3], a[4], ex, ey);
        x = ex; y = ey; cx = x; cy = y;
      }
      qx = x; qy = y;
    }
    return polys;
  }
  function polysBox(polys) {
    let b = null;
    for (const p of polys) for (const [x, y] of p) b = union(b, { x, y, width: 0, height: 0 });
    return b;
  }
  geom.pathLength = (d) => {
    let L = 0;
    for (const p of flatten(d)) for (let k = 1; k < p.length; k++) L += Math.hypot(p[k][0] - p[k - 1][0], p[k][1] - p[k - 1][1]);
    return L;
  };
  geom.pointAtLength = (d, l) => {
    let last = { x: 0, y: 0 };
    for (const p of flatten(d)) {
      for (let k = 1; k < p.length; k++) {
        const s = Math.hypot(p[k][0] - p[k - 1][0], p[k][1] - p[k - 1][1]);
        last = { x: p[k][0], y: p[k][1] };
        if (l <= s && s > 0) {
          const f = l / s;
          return { x: p[k - 1][0] + f * (p[k][0] - p[k - 1][0]), y: p[k - 1][1] + f * (p[k][1] - p[k - 1][1]) };
        }
        l -= s;
      }
    }
    return last;
  };
  // A <text> (or <tspan>) box: rows are child tspans carrying `x` (how
  // mermaid's createText lays lines out); each row is the sum of its runs.
  // Height from the face's ascent / descent (hhea / OS/2, as browsers do)
  // plus the line advance of the rows below the first.
  function textBox(el) {
    const f = geom.fontOf(el);
    const len = (v, d = 0) => {
      if (v == null || v === '') return d;
      const n = parseFloat(v);
      return isNaN(n) ? d : /em$/.test(v) ? n * f.px : n;
    };
    const runW = (n) => {
      if (n.nodeType === 3) return geom.measure(n.data, geom.fontOf(n.parentNode));
      if (n.nodeType !== 1) return 0;
      if (!n.childNodes.some((c) => c.nodeType === 1)) return geom.measure(n.textContent, geom.fontOf(n));
      let w = 0;
      for (const c of n.childNodes) w += runW(c);
      return w;
    };
    const kids = el.childNodes.filter((c) => c.nodeType === 1 && c.localName === 'tspan' && c.getAttribute('x') != null);
    const rows = kids.length && el.localName === 'text' ? kids.map((k) => ({ node: k, w: runW(k) })) : [{ node: el, w: runW(el) }];
    const w = Math.max(0, ...rows.map((r) => r.w));
    const r0 = rows[0] && rows[0].node !== el ? rows[0].node : null;
    let y0 = len(el.getAttribute('y'));
    if (r0 && r0.getAttribute('y') != null) y0 = len(r0.getAttribute('y'));
    y0 += len(el.getAttribute('dy')) + (r0 ? len(r0.getAttribute('dy')) : 0);
    const [asc, desc] = typeof g.__hostFontMetrics === 'function'
      ? g.__hostFontMetrics(f.px, f.weight, f.family) : [0.93 * f.px, 0.24 * f.px];
    const dbl = textProp(r0 || el, 'dominant-baseline', f.rules) || textProp(r0 || el, 'alignment-baseline', f.rules) || '';
    let top = y0 - asc;
    if (/central|middle/.test(dbl)) top = y0 - (asc + desc) / 2;
    else if (/hanging|before-edge/.test(dbl)) top = y0;
    let lh = 1.1 * f.px;
    if (rows.length > 1 && rows[1].node.getAttribute) lh = len(rows[1].node.getAttribute('dy'), lh) || lh;
    const anchor = textProp(r0 || el, 'text-anchor', f.rules) || 'start';
    let x0 = len(el.getAttribute('x'));
    if (r0 && r0.getAttribute('x') != null) x0 = len(r0.getAttribute('x'));
    x0 += len(el.getAttribute('dx'));
    const x = x0 - (anchor === 'middle' ? w / 2 : anchor === 'end' ? w : 0);
    return { x, y: top, width: w, height: asc + desc + lh * (rows.length - 1) };
  }
  const NO_BOX = /^(style|defs|marker|title|desc|clippath|mask|lineargradient|radialgradient|pattern|filter|symbol)$/;
  geom.bbox = function bbox(el) {
    const tag = (el.localName || '').toLowerCase();
    if (tag === 'text' || tag === 'tspan') return textBox(el);
    let b = null;
    if (tag === 'rect' || tag === 'foreignobject' || tag === 'image' || tag === 'use')
      b = { x: num(el, 'x'), y: num(el, 'y'), width: num(el, 'width'), height: num(el, 'height') };
    else if (tag === 'circle') { const r = num(el, 'r'); b = { x: num(el, 'cx') - r, y: num(el, 'cy') - r, width: 2 * r, height: 2 * r }; }
    else if (tag === 'ellipse') {
      const rx = num(el, 'rx'), ry = num(el, 'ry');
      b = { x: num(el, 'cx') - rx, y: num(el, 'cy') - ry, width: 2 * rx, height: 2 * ry };
    } else if (tag === 'line') b = union({ x: num(el, 'x1'), y: num(el, 'y1'), width: 0, height: 0 }, { x: num(el, 'x2'), y: num(el, 'y2'), width: 0, height: 0 });
    else if (tag === 'path') b = polysBox(flatten(el.getAttribute('d') || ''));
    else if (tag === 'polygon' || tag === 'polyline') b = polysBox(flatten('M' + (el.getAttribute('points') || '')));
    else if (!NO_BOX.test(tag)) {
      // A container: the union of its children's boxes, each through its
      // own transform (empty boxes at the origin are skipped).
      for (const c of el.childNodes) {
        if (c.nodeType !== 1) continue;
        const cb = bbox(c);
        if (cb && (cb.width || cb.height || cb.x || cb.y)) b = union(b, applyT(transformOf(c), cb));
      }
    }
    return b;
  };

  // --------------------------------------------------------------- globals
  const document = new Document();
  Object.assign(g, {
    window: g, self: g, document, Node, Element, HTMLElement: Element, SVGElement: Element,
    SVGGraphicsElement: Element, SVGSVGElement: Element, Text, Comment, DocumentFragment, Document,
    HTMLTemplateElement: Element, HTMLFormElement: class {},
    NodeFilter: { SHOW_ALL: 0xffffffff, SHOW_ELEMENT: 1, SHOW_TEXT: 4, SHOW_COMMENT: 128, FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3 },
    navigator: { userAgent: 'cctext-render', language: 'en-US', platform: '' },
    location: { href: 'about:blank', protocol: 'about:', host: '', hostname: '', origin: 'null', pathname: 'blank', search: '', hash: '' },
    getComputedStyle: (el) => (el && el.style) || { getPropertyValue: () => '' },
    requestAnimationFrame: (f) => setTimeout(f, 0),
    cancelAnimationFrame: () => {},
    addEventListener() {},
    removeEventListener() {},
    dispatchEvent() { return true; },
    devicePixelRatio: 1, innerWidth: 1280, innerHeight: 800,
    matchMedia: () => ({ matches: false, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }),
    Event: class Event { constructor(t) { this.type = t; } },
    CustomEvent: class CustomEvent { constructor(t, o) { this.type = t; this.detail = o && o.detail; } },
    MutationObserver: class { observe() {} disconnect() {} takeRecords() { return []; } },
    ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
    CSSStyleSheet: class {
      constructor() { this.cssRules = []; }
      insertRule(t, i = this.cssRules.length) { this.cssRules.splice(i, 0, { cssText: t }); return i; }
      replaceSync() {}
    },
    XMLSerializer: class { serializeToString(n) { return serialize(n); } },
    // Images never load here (no network, no files): icon / image shapes
    // get a zero-size image whose decode() fails, and draw without it.
    Image: class Image {
      constructor(w, h) { this.width = w || 0; this.height = h || 0; this.naturalWidth = 0; this.naturalHeight = 0; this.complete = true; }
      decode() { return Promise.reject(new Error('images are not loaded in cctext-render')); }
      addEventListener() {}
      removeEventListener() {}
    },
    DOMParser: class { parseFromString(s) { const d = new Document(); d.body.innerHTML = s; return d; } },
  });
  if (typeof g.structuredClone !== 'function') g.structuredClone = (o) => JSON.parse(JSON.stringify(o));

  // For the glue (mm_glue.js): per-job caches keyed by stylesheet text and
  // selector are dropped after each render so a long-lived engine does
  // not grow with every diagram it has seen.
  g.__cctextDom = {
    serialize, parseSelector, geom,
    reset() {
      sheetCache.clear();
      indexCache.clear();
      if (selCache.size > 2000) selCache.clear();
    },
  };
})();
