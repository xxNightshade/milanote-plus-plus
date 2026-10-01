// Milanote++ bridge
// ------------------------------------------------------------------
// This script is injected by Milanote++.exe into a WebView2 page that
// lives on https://app.milanote.com (the same origin as the Milanote web
// app).  Because it runs on that origin, the browser attaches the user's
// Milanote session cookies to every request, so the user only ever signs
// in through Milanote's own login page - Milanote++ never sees a password.
//
// It talks to Milanote's private (unofficial) API:
//   * REST endpoints under /api/*                      -> reads
//   * socket.io "action" events                        -> writes
//   * media-service /media-service/api/link            -> link previews
// See docs/milanote-api.md for the captured protocol.
//
// Host protocol (C++ <-> JS):
//   C++ -> JS : window.chrome.webview message {callId, tool, args}
//   JS  -> C++: window.chrome.webview.postMessage({callId, result} | {callId, error})
//               window.chrome.webview.postMessage({type:'ready'})
//               window.chrome.webview.postMessage({type:'log', level, message})
//
// The same file can be evaluated in a normal browser tab on app.milanote.com
// for debugging; it then exposes window.MilanoteBridge.call(tool, args).
// ------------------------------------------------------------------
(function () {
  'use strict';
  if (window.MilanoteBridge) return;

  const ORIGIN = 'https://app.milanote.com';
  const API = ORIGIN + '/api';
  const MEDIA_API = ORIGIN + '/media-service/api';
  const SOCKET_URL = 'wss://app.milanote.com/socket.io/';
  const A62 = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ';
  const SCORE_STEP = 65536;        // z-order step used by the web client
  const PLATFORM = 'Milanote++';
  const ACK_TIMEOUT_MS = 15000;
  const NOTE_WIDTH_GU = 34;        // a default note/column is 304px wide; 1 grid unit = 9px at 100% zoom
  const DEFAULT_GAP_GU = 4;

  // ---------------------------------------------------------------- logging
  function post(msg) {
    try {
      if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(msg);
    } catch (e) { /* ignore */ }
  }
  function log(level, message) {
    post({ type: 'log', level: level, message: String(message) });
    if (!(window.chrome && window.chrome.webview)) console.log('[Milanote++]', level, message);
  }

  class BridgeError extends Error {
    constructor(message, code) { super(message); this.code = code || 'error'; }
  }

  // ---------------------------------------------------------------- ids
  function enc62(n) {
    n = Math.floor(n);
    if (n <= 0) return '0';
    let s = '';
    while (n > 0) { s = A62[n % 62] + s; n = Math.floor(n / 62); }
    return s;
  }
  function pad(s, len) { return ('0'.repeat(len) + s).slice(-len); }
  function rnd62(len) {
    let s = '';
    const buf = new Uint32Array(len);
    crypto.getRandomValues(buf);
    for (let i = 0; i < len; i++) s += A62[buf[i] % 62];
    return s;
  }
  function nowSeconds() { return Math.floor(Date.now() / 1000); }

  const client = {
    clientId: rnd62(6),
    clientTick: 1,
    deviceId: null,
    sessionId: 'msid-' + enc62(nowSeconds()) + rnd62(4),
    versionCounter: 0,
    userId: null,
    user: null,
  };
  // Milanote++ identifies itself as its own "device" (the web app keeps its id under
  // localStorage.mdid; we deliberately use a separate, persistent one).
  try {
    client.deviceId = localStorage.getItem('milanotepp.deviceId');
  } catch (e) { /* storage may be unavailable */ }
  if (!client.deviceId) {
    client.deviceId = 'mdid-' + enc62(nowSeconds()) + rnd62(4);
    try { localStorage.setItem('milanotepp.deviceId', client.deviceId); } catch (e) { /* ignore */ }
  }
  const versionSeed = client.sessionId.slice(5);

  function newElementId() {
    client.clientTick += 1;
    return enc62(nowSeconds()) + client.clientId + pad(enc62(client.clientTick % 3844), 2);
  }
  function newVersionId() {
    client.versionCounter += 1;
    return versionSeed + '-' + client.versionCounter;
  }
  function newBlockKey() { return rnd62(5); }

  // ---------------------------------------------------------------- HTTP
  async function ensureWafToken() {
    try {
      if (!window.AwsWafIntegration) {
        // The app loads AWS WAF's challenge script from static.milanote.com; find its current id.
        const html = await (await fetch(ORIGIN + '/', { credentials: 'include' })).text();
        const m = html.match(/https:\/\/static\.milanote\.com\/awswaf\/([0-9a-f]+)\/challenge[\w.]*\.js/);
        if (!m) return false;
        await new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = m[0];
          s.onload = resolve;
          s.onerror = () => reject(new Error('failed to load WAF script'));
          (document.head || document.documentElement).appendChild(s);
        });
      }
      if (window.AwsWafIntegration && typeof window.AwsWafIntegration.getToken === 'function') {
        await window.AwsWafIntegration.getToken();
        return true;
      }
    } catch (e) {
      log('warn', 'WAF token refresh failed: ' + e.message);
    }
    return false;
  }

  // Milanote's session cookie is a browser-session cookie, so it disappears whenever the WebView2
  // browser process restarts. The web app survives that by keeping a long-lived JWT in
  // localStorage ("token") and turning it back into a session; do the same.
  let restoring = null;   // parallel 401s share one restore attempt
  function restoreSession() {
    if (restoring) return restoring;
    restoring = (async () => {
      let token = null;
      try { token = localStorage.getItem('token'); } catch (e) { /* storage unavailable */ }
      if (!token) return false;
      try {
        const res = await fetch(API + '/auth/upgrade-better-auth-session', {
          method: 'POST',
          credentials: 'include',
          headers: { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json', 'Accept': 'application/json' },
          body: '{}',
        });
        if (res.ok) { log('info', 'Milanote session restored from the stored token'); return true; }
        log('warn', 'session restore failed with HTTP ' + res.status);
      } catch (e) {
        log('warn', 'session restore failed: ' + e.message);
      }
      return false;
    })();
    return restoring.finally(() => { restoring = null; });
  }

  async function http(url, opts, retry) {
    opts = opts || {};
    const init = {
      method: opts.method || 'GET',
      credentials: 'include',
      headers: Object.assign({ 'Accept': 'application/json' }, opts.headers || {}),
    };
    if (opts.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(opts.body);
    }
    let full = url;
    if (opts.params) {
      const q = Object.entries(opts.params)
        .filter(([, v]) => v !== undefined && v !== null)
        .map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(String(v)))
        .join('&');
      if (q) full += (full.includes('?') ? '&' : '?') + q;
    }
    const res = await fetch(full, init);
    const text = await res.text();
    let data = null;
    try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }

    if (res.status === 401) {
      if (!retry && await restoreSession()) return http(url, opts, true);
      throw new BridgeError('Not signed in to Milanote. Run "Milanote++.exe --login" (or use the milanote_login tool) and sign in, then try again.', 'auth');
    }
    const wafAction = res.headers.get('x-amzn-waf-action');
    if ((res.status === 202 || res.status === 405) && wafAction && !retry) {
      log('info', 'WAF challenge received, refreshing token');
      await ensureWafToken();
      return http(url, opts, true);
    }
    if (!res.ok) {
      const msg = (data && data.error && (data.error.message || data.error.code)) || text.slice(0, 200) || res.statusText;
      throw new BridgeError('Milanote API ' + init.method + ' ' + url.replace(ORIGIN, '') + ' failed (' + res.status + '): ' + msg, 'http');
    }
    return data;
  }
  const api = (path, opts) => http(API + path, opts);

  // ---------------------------------------------------------------- socket.io (Engine.IO v4, websocket only)
  const socket = {
    ws: null,
    state: 'closed',        // closed | connecting | open
    ack: 0,
    pending: new Map(),     // ackId -> {resolve, reject, timer}
    openPromise: null,
  };

  function socketConnect() {
    if (socket.state === 'open') return Promise.resolve();
    if (socket.openPromise) return socket.openPromise;
    socket.state = 'connecting';
    socket.openPromise = new Promise((resolve, reject) => {
      const url = SOCKET_URL + '?userId=' + encodeURIComponent(client.userId) + '&EIO=4&transport=websocket';
      let ws;
      try { ws = new WebSocket(url); } catch (e) { socket.state = 'closed'; socket.openPromise = null; reject(e); return; }
      socket.ws = ws;
      let settled = false;
      const fail = (err) => {
        if (!settled) { settled = true; reject(err); }
        socket.state = 'closed';
        socket.openPromise = null;
        for (const [, p] of socket.pending) { clearTimeout(p.timer); p.reject(new BridgeError('socket closed', 'socket')); }
        socket.pending.clear();
      };
      const timer = setTimeout(() => { if (!settled) { try { ws.close(); } catch (e) { } fail(new BridgeError('socket connect timeout', 'socket')); } }, ACK_TIMEOUT_MS);
      ws.onmessage = (ev) => {
        const d = typeof ev.data === 'string' ? ev.data : '';
        if (d.startsWith('0')) {            // engine.io OPEN -> connect to default namespace
          ws.send('40');
        } else if (d === '2') {             // ping -> pong
          ws.send('3');
        } else if (d.startsWith('40')) {    // socket.io CONNECT ack
          clearTimeout(timer);
          socket.state = 'open';
          socket.openPromise = null;
          if (!settled) { settled = true; resolve(); }
        } else if (d.startsWith('44')) {    // CONNECT_ERROR
          clearTimeout(timer);
          fail(new BridgeError('socket connect error: ' + d.slice(2, 300), 'socket'));
        } else if (d.startsWith('43')) {    // ACK
          const m = d.match(/^43(\d+)([\s\S]*)$/);
          if (!m) return;
          const id = Number(m[1]);
          const p = socket.pending.get(id);
          if (!p) return;
          socket.pending.delete(id);
          clearTimeout(p.timer);
          let payload = null;
          try { payload = JSON.parse(m[2] || '[]'); } catch (e) { payload = [m[2]]; }
          p.resolve(payload);
        } else if (d.startsWith('42')) {    // broadcast event from the server (remote actions) - ignored
        }
      };
      ws.onerror = () => { clearTimeout(timer); fail(new BridgeError('socket error', 'socket')); };
      ws.onclose = () => { clearTimeout(timer); fail(new BridgeError('socket closed', 'socket')); };
    });
    return socket.openPromise;
  }

  async function socketEmit(event, payload) {
    try {
      await socketConnect();
    } catch (e) {
      // the socket authenticates with the same cookie as the REST API; re-create it and try once more
      if (!(await restoreSession())) throw e;
      await socketConnect();
    }
    return new Promise((resolve, reject) => {
      const id = ++socket.ack;
      const timer = setTimeout(() => {
        socket.pending.delete(id);
        reject(new BridgeError('Milanote did not acknowledge the "' + event + '" event in time', 'timeout'));
      }, ACK_TIMEOUT_MS);
      socket.pending.set(id, { resolve, reject, timer });
      try {
        socket.ws.send('42' + id + JSON.stringify([event, payload]));
      } catch (e) {
        clearTimeout(timer);
        socket.pending.delete(id);
        reject(e);
      }
    });
  }

  // Sends one Milanote action (ELEMENT_CREATE, ELEMENT_UPDATE, ...) and checks the ack.
  async function sendAction(action) {
    await ensureUser();
    const envelope = Object.assign({
      sync: true,
      timestamp: Date.now(),
      user: { _id: client.userId, clientId: client.clientId, clientTick: client.clientTick },
      tokens: [],
      deviceId: client.deviceId,
      sessionId: client.sessionId,
    }, action);
    const ack = await socketEmit('action', envelope);
    const status = ack && ack[0] && ack[0].status;
    if (status !== 200) {
      throw new BridgeError('Milanote rejected ' + action.type + ': ' + JSON.stringify(ack).slice(0, 300), 'rejected');
    }
    return ack[0];
  }

  // ---------------------------------------------------------------- user
  async function ensureUser() {
    if (client.user) return client.user;
    const data = await api('/users/me');
    if (!data || !data.user || !data.user._id) throw new BridgeError('Unexpected /api/users/me response', 'http');
    client.user = data.user;
    client.userId = data.user._id;
    return client.user;
  }

  // ---------------------------------------------------------------- element access
  const boardCache = new Map(); // boardId -> {time, elements}

  async function fetchBoard(boardId, useCache) {
    if (useCache) {
      const c = boardCache.get(boardId);
      if (c && Date.now() - c.time < 5000) return c.elements;
    }
    const data = await api('/boards', { params: { excludeSelf: false, loadAncestors: false, ids: boardId } });
    if (data && data.errors && data.errors[boardId]) {
      throw new BridgeError('Board ' + boardId + ' could not be loaded: ' + JSON.stringify(data.errors[boardId]).slice(0, 200), 'notfound');
    }
    const elements = (data && data.elements) || {};
    if (!elements[boardId]) throw new BridgeError('Board ' + boardId + ' was not found (or you do not have access to it).', 'notfound');
    boardCache.set(boardId, { time: Date.now(), elements });
    return elements;
  }

  function childrenOf(elements, parentId, section) {
    return Object.values(elements).filter(e => e.location && e.location.parentId === parentId && (!section || e.location.section === section));
  }

  function sortByPosition(list) {
    return list.slice().sort((a, b) => {
      const pa = (a.location && a.location.position) || {}, pb = (b.location && b.location.position) || {};
      if (pa.index !== undefined || pb.index !== undefined) return (pa.index || 0) - (pb.index || 0);
      if ((pa.y || 0) !== (pb.y || 0)) return (pa.y || 0) - (pb.y || 0);
      return (pa.x || 0) - (pb.x || 0);
    });
  }

  function boardIdOf(elements, id) {
    // Walks up parents until a BOARD is found (columns / task lists live inside boards).
    let cur = elements[id];
    let guard = 0;
    while (cur && guard++ < 20) {
      if (cur.elementType === 'BOARD') return cur._id;
      const pid = cur.location && cur.location.parentId;
      if (!pid) return null;
      if (!elements[pid]) return pid;
      cur = elements[pid];
    }
    return null;
  }

  // Finds the board that contains an element, fetching it if needed. Returns {boardId, elements}.
  // The board that *holds* an element: for a sub-board that is its parent board, not the board itself
  // (moving or trashing a board must target the parent's canvas/trash - "an element cannot be dropped onto itself").
  function holderBoardOf(ctx) {
    const el = ctx.element;
    if (el && el.elementType === 'BOARD' && el.location && el.location.parentId) return el.location.parentId;
    return ctx.boardId;
  }

  async function contextOf(id) {
    const data = await api('/elements', { params: { loadAncestors: true, ids: id } });
    const elements = (data && data.elements) || {};
    if (!elements[id]) throw new BridgeError('Element ' + id + ' was not found (or you do not have access to it).', 'notfound');
    const el = elements[id];
    if (el.elementType === 'BOARD') return { boardId: id, elements, element: el };
    const boardId = boardIdOf(elements, id) || (el.location && el.location.parentId);
    return { boardId, elements, element: el };
  }

  // Picks a free spot on a board canvas: to the right of everything already there.
  function nextCanvasPosition(elements, boardId, x, y) {
    const kids = childrenOf(elements, boardId, 'CANVAS');
    let maxScore = 0, maxX = -Infinity, minY = Infinity;
    for (const k of kids) {
      const p = (k.location && k.location.position) || {};
      if ((p.score || 0) > maxScore) maxScore = p.score || 0;
      const w = (k.content && k.content.width) ? Number(k.content.width) : NOTE_WIDTH_GU;
      const right = (p.x || 0) + (isFinite(w) ? w : NOTE_WIDTH_GU);
      if (right > maxX) maxX = right;
      if ((p.y || 0) < minY) minY = p.y || 0;
    }
    if (kids.length === 0) { maxX = 0; minY = 2; }
    return {
      x: (typeof x === 'number') ? x : Math.round(maxX + DEFAULT_GAP_GU),
      y: (typeof y === 'number') ? y : Math.max(2, Math.round(minY)),
      score: maxScore + SCORE_STEP,
    };
  }

  function nextListPosition(elements, parentId, index) {
    const kids = sortByPosition(childrenOf(elements, parentId, 'INBOX'));
    const i = (typeof index === 'number' && index >= 0 && index <= kids.length) ? index : kids.length;
    return { index: i, score: i * SCORE_STEP };
  }

  // ---------------------------------------------------------------- text <-> TipTap document
  function inlineNodes(text) {
    // Supports **bold**, *italic*, `code` and bare URLs (turned into links).
    const nodes = [];
    const re = /(\*\*([^*]+)\*\*)|(\*([^*\n]+)\*)|(`([^`]+)`)|(https?:\/\/[^\s<>()]+)/g;
    let last = 0, m;
    while ((m = re.exec(text)) !== null) {
      if (m.index > last) nodes.push({ type: 'text', text: text.slice(last, m.index) });
      if (m[1]) nodes.push({ type: 'text', text: m[2], marks: [{ type: 'bold' }] });
      else if (m[3]) nodes.push({ type: 'text', text: m[4], marks: [{ type: 'italic' }] });
      else if (m[5]) nodes.push({ type: 'text', text: m[6], marks: [{ type: 'code' }] });
      else if (m[7]) nodes.push({ type: 'text', text: m[7], marks: [{ type: 'link', attrs: { href: m[7], target: '_blank', rel: 'noopener noreferrer nofollow', class: null } }] });
      last = re.lastIndex;
    }
    if (last < text.length) nodes.push({ type: 'text', text: text.slice(last) });
    return nodes;
  }
  function paragraph(text) {
    // Lines inside one paragraph are kept as line breaks (what you type is what you see in the note).
    const p = { type: 'paragraph', attrs: { key: newBlockKey(), textAlign: 'left' } };
    const content = [];
    String(text).split('\n').forEach((line, i) => {
      if (i > 0) content.push({ type: 'hardBreak' });
      content.push(...inlineNodes(line));
    });
    if (content.length) p.content = content;
    return p;
  }

  // Plain text / light Markdown -> TipTap doc used by Milanote notes (CARD.textContent).
  function textToDoc(text) {
    const lines = String(text || '').replace(/\r\n?/g, '\n').split('\n');
    const blocks = [];
    let list = null; // {type:'bulletList'|'taskList', items:[]}
    let para = [];   // lines of the paragraph being built
    const flushPara = () => {
      if (para.length) { blocks.push(paragraph(para.join('\n'))); para = []; }
    };
    const flushList = () => {
      if (list) { blocks.push(list.node); list = null; }
    };
    for (const raw of lines) {
      const line = raw.replace(/\s+$/, '');
      let m;
      if (!line.trim()) { flushPara(); flushList(); continue; }
      if ((m = line.match(/^(#{1,3})\s+(.*)$/))) {
        flushPara(); flushList();
        blocks.push({ type: 'heading', attrs: { key: newBlockKey(), textAlign: 'left', level: m[1].length }, content: inlineNodes(m[2]) });
        continue;
      }
      if ((m = line.match(/^\s*[-*+]\s+\[( |x|X)\]\s+(.*)$/))) {
        flushPara();
        if (!list || list.type !== 'taskList') { flushList(); list = { type: 'taskList', node: { type: 'taskList', attrs: { key: newBlockKey() }, content: [] } }; }
        list.node.content.push({ type: 'taskItem', attrs: { key: newBlockKey(), checked: m[1].toLowerCase() === 'x' }, content: [paragraph(m[2])] });
        continue;
      }
      if ((m = line.match(/^\s*[-*+]\s+(.*)$/)) || (m = line.match(/^\s*\d+[.)]\s+(.*)$/))) {
        flushPara();
        if (!list || list.type !== 'bulletList') { flushList(); list = { type: 'bulletList', node: { type: 'bulletList', attrs: { key: newBlockKey() }, content: [] } }; }
        list.node.content.push({ type: 'listItem', attrs: { key: newBlockKey() }, content: [paragraph(m[1])] });
        continue;
      }
      if ((m = line.match(/^>\s?(.*)$/))) {
        flushPara(); flushList();
        blocks.push({ type: 'blockquote', attrs: { key: newBlockKey() }, content: [paragraph(m[1])] });
        continue;
      }
      if (/^(-{3,}|\*{3,})$/.test(line.trim())) {
        flushPara(); flushList();
        blocks.push({ type: 'horizontalRule', attrs: { key: newBlockKey() } });
        continue;
      }
      flushList();
      para.push(line.trim());
    }
    flushPara(); flushList();
    if (!blocks.length) blocks.push(paragraph(''));
    return { type: 'doc', content: blocks };
  }

  // TipTap doc -> readable Markdown-ish text.
  function docToText(doc) {
    if (!doc) return '';
    if (typeof doc === 'string') return doc;
    const inline = (nodes) => (nodes || []).map(n => {
      if (n.type === 'text') {
        let t = n.text || '';
        for (const mk of (n.marks || [])) {
          if (mk.type === 'bold') t = '**' + t + '**';
          else if (mk.type === 'italic') t = '*' + t + '*';
          else if (mk.type === 'code') t = '`' + t + '`';
          else if (mk.type === 'link' && mk.attrs && mk.attrs.href && mk.attrs.href !== n.text) t = '[' + t + '](' + mk.attrs.href + ')';
        }
        return t;
      }
      if (n.type === 'hardBreak') return '\n';
      if (n.type === 'mention' || n.type === 'textMention') return '@' + ((n.attrs && n.attrs.label) || 'user');
      if (n.type === 'inlineBoardLink') return '[' + ((n.attrs && n.attrs.title) || 'board') + '](' + ((n.attrs && n.attrs.url) || '') + ')';
      return n.content ? inline(n.content) : '';
    }).join('');
    const block = (n, depth) => {
      const ind = '  '.repeat(depth || 0);
      switch (n.type) {
        case 'paragraph': case 'smallText': return ind + inline(n.content);
        case 'heading': return ind + '#'.repeat((n.attrs && n.attrs.level) || 1) + ' ' + inline(n.content);
        case 'bulletList': return (n.content || []).map(li => ind + '- ' + (li.content || []).map((c, i) => i === 0 ? inline(c.content) : '\n' + block(c, (depth || 0) + 1)).join('')).join('\n');
        case 'orderedList': return (n.content || []).map((li, i) => ind + (i + 1) + '. ' + (li.content || []).map((c, j) => j === 0 ? inline(c.content) : '\n' + block(c, (depth || 0) + 1)).join('')).join('\n');
        case 'taskList': return (n.content || []).map(ti => ind + '- [' + (ti.attrs && ti.attrs.checked ? 'x' : ' ') + '] ' + (ti.content || []).map((c, i) => i === 0 ? inline(c.content) : '\n' + block(c, (depth || 0) + 1)).join('')).join('\n');
        case 'blockquote': case 'callout': return (n.content || []).map(c => ind + '> ' + inline(c.content)).join('\n');
        case 'codeBlock': return ind + '```\n' + inline(n.content) + '\n```';
        case 'horizontalRule': return ind + '---';
        default: return n.content ? (n.content.map(c => block(c, depth)).join('\n')) : '';
      }
    };
    return (doc.content || []).map(b => block(b, 0)).join('\n\n');
  }

  // ---------------------------------------------------------------- element operations
  async function createElement(opts) {
    // opts: {elementType, parentId, section, position, content, boardId}
    const id = newElementId();
    const now = Date.now();
    const boardId = opts.boardId;
    await sendAction({
      id,
      location: { parentId: opts.parentId, section: opts.section, position: opts.position },
      content: opts.content || {},
      meta: {
        creator: client.userId, modifiedBy: client.userId,
        createdTime: now, modifiedTime: now, platform: PLATFORM,
        locationSectionModifiedTime: now, versionId: versionSeed + '-1',
      },
      elementType: opts.elementType,
      type: 'ELEMENT_CREATE',
      timestamp: now,
      creationSource: 'TOOLBAR',
      // A board created here is filled with content afterwards. If other clients marked it as
      // "already fetched" (what the web app does for its own empty boards) they would show it
      // empty until re-opened, so ask them to fetch it when they navigate to it.
      markAsFetched: opts.elementType !== 'BOARD',
      activity: { track: false, boardId },
      channels: [boardId],
    });
    boardCache.delete(boardId);
    return id;
  }

  async function updateElement(id, changes, ctx) {
    ctx = ctx || await contextOf(id);
    const el = ctx.elements[id] || ctx.element;
    const prev = (el.meta && el.meta.versionId) || null;
    const update = { id, changes, meta: { modifiedVersionId: prev, versionId: newVersionId() } };
    await sendAction({
      type: 'ELEMENT_UPDATE',
      updates: [update],
      recalculateChannels: false,
      activity: { track: false, boardId: holderBoardOf(ctx), elementTypes: { [id]: el.elementType } },
      channels: Array.from(new Set([holderBoardOf(ctx), ctx.boardId, id])),
    });
    boardCache.delete(ctx.boardId);
    boardCache.delete(holderBoardOf(ctx));
    return el;
  }

  async function moveElements(moves, moveOperation, sourceBoardId, destinationBoardId, elementTypes) {
    await sendAction({
      type: 'ELEMENT_MOVE_MULTI',
      moves,
      moveOperation,
      monitoring: { operation: moveOperation },
      activity: {
        track: false,
        sourceBoardId, isSourceShared: false,
        destinationBoardId, isDestinationShared: false,
        elementTypes,
      },
      channels: Array.from(new Set([sourceBoardId, destinationBoardId].filter(Boolean))),
    });
    boardCache.delete(sourceBoardId);
    boardCache.delete(destinationBoardId);
  }

  async function setElementType(id, elementType, changes, boardId) {
    await sendAction({
      type: 'ELEMENT_SET_TYPE',
      id, elementType, changes,
      activity: { track: false, boardId },
      channels: [boardId],
    });
    boardCache.delete(boardId);
  }

  // ---------------------------------------------------------------- summaries returned to the model
  function summarize(elements, id, depth) {
    const el = elements[id];
    if (!el) return null;
    const c = el.content || {};
    const out = { id: el._id, type: el.elementType };
    if (c.title) out.title = c.title;
    if (el.elementType === 'CARD' || el.elementType === 'TASK') {
      const text = docToText(c.textContent);
      if (text) out.text = text;
    }
    if (el.elementType === 'TASK' && c.isComplete) out.done = true;
    if (el.elementType === 'LINK') {
      out.url = (c.link && c.link.url) || c.url || null;
      if (c.link && c.link.title) out.linkTitle = c.link.title;
      if (c.caption) out.caption = docToText(c.caption);
    }
    if (el.elementType === 'IMAGE' || el.elementType === 'FILE') {
      if (c.image && (c.image.original || c.image.regular)) out.imageUrl = c.image.original || c.image.regular;
      if (c.file && c.file.filename) out.filename = c.file.filename;
      if (c.caption) out.caption = docToText(c.caption);
    }
    if (el.elementType === 'COLOR_SWATCH' && c.color) out.color = c.color.value || c.color;
    if (c.color && typeof c.color === 'string') out.color = c.color;
    if (el.location) {
      out.section = el.location.section;
      const p = el.location.position || {};
      if (p.x !== undefined) out.position = { x: p.x, y: p.y };
      if (p.index !== undefined) out.index = p.index;
    }
    if (el.elementType === 'COLUMN' || el.elementType === 'TASK_LIST') {
      const kids = sortByPosition(childrenOf(elements, id, 'INBOX'));
      out.children = depth > 0 ? kids.map(k => summarize(elements, k._id, depth - 1)).filter(Boolean) : kids.map(k => k._id);
    }
    return out;
  }

  function boardSummary(elements, boardId) {
    const board = elements[boardId];
    const canvas = sortByPosition(childrenOf(elements, boardId, 'CANVAS'));
    const inbox = sortByPosition(childrenOf(elements, boardId, 'INBOX'));
    return {
      id: boardId,
      title: (board.content && board.content.title) || 'Untitled board',
      parentId: board.location && board.location.parentId,
      elementCount: Object.keys(elements).length - 1,
      canvas: canvas.map(e => summarize(elements, e._id, 2)),
      unsorted: inbox.map(e => summarize(elements, e._id, 2)),
    };
  }

  // ---------------------------------------------------------------- tools
  function req(args, name, type) {
    const v = args ? args[name] : undefined;
    if (v === undefined || v === null || v === '') throw new BridgeError('Missing required argument "' + name + '"', 'args');
    if (type && typeof v !== type) throw new BridgeError('Argument "' + name + '" must be a ' + type, 'args');
    return v;
  }

  async function resolveParent(args) {
    // Accepts parentId (board, column or task list). Returns {parentId, parentType, boardId, elements}.
    const user = await ensureUser();
    const parentId = (args && args.parentId) || user.rootBoardId;
    const ctx = await contextOf(parentId);
    const parentType = ctx.element.elementType;
    let elements = ctx.elements;
    if (parentType === 'BOARD') elements = await fetchBoard(parentId, false);
    else elements = await fetchBoard(ctx.boardId, false);
    if (!elements[parentId]) elements[parentId] = ctx.element;
    return { parentId, parentType, boardId: ctx.boardId, elements };
  }

  const BOARD_PALETTES = [
    ['#6592c5', '#918abc', '#b090c4', '#c17b8a', '#f47772', '#eab174', '#eaca8b'],
    ['#55b2a6', '#82b29b', '#e1ddca', '#e9c56a', '#f4a361', '#e07a60', '#e77051'],
    ['#98b3bc', '#a3bfcd', '#bbd2d8', '#cfddda', '#d69f9d', '#df8288', '#ef6660'],
    ['#e9785e', '#dda16d', '#d5b3a3', '#d1cbc6', '#accde1', '#98c1d9', '#839fc3'],
    ['#75a4be', '#8abbd6', '#6ab8b3', '#b4d9d7', '#db9db7', '#c889a8', '#b992b4'],
  ];

  // Creates a sub-board element on the canvas of parent board `p` (from resolveParent).
  async function createBoardElement(p, title, x, y, color) {
    const position = nextCanvasPosition(p.elements, p.parentId, x, y);
    const id = await createElement({
      elementType: 'BOARD', parentId: p.parentId, section: 'CANVAS', position, boardId: p.boardId,
      content: { title, icon: null, color: color || null, secondaryColor: null, defaultColorPalette: BOARD_PALETTES[Math.floor(Math.random() * BOARD_PALETTES.length)] },
    });
    return { id, title, url: ORIGIN + '/' + id, parentId: p.parentId, position };
  }

  // Asks Milanote's media service for a link preview (title, favicon, image) and applies it, like the web app does.
  async function decorateLink(id, url, caption, boardId) {
    let preview = null;
    try {
      const user = await ensureUser();
      preview = await http(MEDIA_API + '/link', { method: 'POST', body: { url, elementId: id, environmentFolder: 'p', userId: user._id, locale: 'en' } });
    } catch (e) {
      log('warn', 'link preview failed: ' + e.message);
    }
    if (preview && !preview.error) {
      const changes = {};
      for (const k of ['image', 'mediaType', 'link', 'provider', 'media']) if (preview[k] !== undefined) changes[k] = preview[k];
      const captionText = caption || preview.description || '';
      if (captionText) { changes.caption = textToDoc(captionText); changes.showCaption = true; }
      else changes.showCaption = false;
      try {
        await setElementType(id, preview.elementType || 'LINK', changes, boardId);
      } catch (e) {
        log('warn', 'link preview could not be applied: ' + e.message);
      }
    } else if (caption) {
      await updateElement(id, { caption: textToDoc(caption), showCaption: true });
    }
    return (preview && preview.link && preview.link.title) || null;
  }

  // ---------------------------------------------------------------- outlines (a whole board as Markdown)
  //
  //   # Board title            -> optional; creates a new sub-board. Plain text right under it becomes the title note.
  //   ## Column title          -> a column
  //   ### Note heading         -> a note; everything up to the next ### / ## belongs to it (blank lines allowed)
  //   ### To-do title          -> a section whose lines are ALL "- [ ] task" / "- [x] done" becomes a to-do list
  //   ###                      -> a bare ### starts a note (or link) without a heading
  //   https://example.com Cap  -> a section that is only a URL (+ optional caption) becomes a link card
  //   <!-- comment -->         -> ignored
  //
  // Sections before the first ## are placed on the board canvas; heading-less paragraphs there end at a blank line.
  const COLUMN_STEP_GU = NOTE_WIDTH_GU + DEFAULT_GAP_GU;
  const HEADER_HEIGHT_GU = 12;
  const TASK_LINE = /^\s*[-*+]\s+\[( |x|X)\]\s+(.*)$/;
  const URL_LINE = /^(https?:\/\/[^\s<>()]+)(?:\s+(.*))?$/;

  function sectionToItem(s) {
    const body = s.lines.slice();
    while (body.length && !body[body.length - 1].trim()) body.pop();
    while (body.length && !body[0].trim()) body.shift();
    const nonEmpty = body.filter(l => l.trim());
    if (nonEmpty.length === 1 && URL_LINE.test(nonEmpty[0].trim())) {
      const m = nonEmpty[0].trim().match(URL_LINE);
      const caption = ((m[2] || '').trim() || s.heading || '');
      return caption ? { link: m[1], caption } : { link: m[1] };
    }
    if (nonEmpty.length && nonEmpty.every(l => TASK_LINE.test(l))) {
      const tasks = nonEmpty.map(l => { const m = l.match(TASK_LINE); return { text: m[2].trim(), done: m[1].toLowerCase() === 'x' }; });
      return s.heading ? { todo: s.heading, tasks } : { todo: '', tasks };
    }
    const text = (s.heading ? '## ' + s.heading + (body.length ? '\n' : '') : '') + body.join('\n');
    if (!text.trim()) return null;
    return { note: text };
  }

  function outlineToSpec(text) {
    const lines = String(text || '').replace(/\r\n?/g, '\n').split('\n');
    const spec = { title: null, header: null, columns: [], items: [] };
    let column = null;      // current column, or null while on the canvas
    let section = null;     // { heading, lines, explicit }
    const container = () => (column ? column.items : spec.items);
    const close = () => {
      if (!section) return;
      const item = sectionToItem(section);
      if (item) container().push(item);
      section = null;
    };
    for (const raw of lines) {
      const line = raw.replace(/\s+$/, '');
      let m;
      if (/^\s*<!--.*-->\s*$/.test(line)) continue;
      if ((m = line.match(/^#\s+(.*)$/)) && spec.title === null && !column && !section && spec.items.length === 0) {
        spec.title = m[1].trim();
        continue;
      }
      if ((m = line.match(/^##\s+(.*)$/))) {
        close();
        column = { title: m[1].trim(), items: [] };
        spec.columns.push(column);
        continue;
      }
      if ((m = line.match(/^###(?:\s+(.*))?$/))) {
        close();
        section = { heading: (m[1] || '').trim(), lines: [], explicit: true };
        continue;
      }
      if (!line.trim()) {
        if (section && !section.explicit) close();     // a blank line ends a heading-less block
        else if (section) section.lines.push('');
        continue;
      }
      if (!section) section = { heading: '', lines: [], explicit: false };
      section.lines.push(line);
    }
    close();
    // Plain text right under the board title becomes the title note.
    if (spec.items.length && spec.items[0].note !== undefined && !/^##\s/.test(spec.items[0].note)) {
      spec.header = spec.items.shift().note;
    }
    return spec;
  }

  function boardToOutline(elements, boardId, withIds) {
    const board = elements[boardId];
    const title = (board.content && board.content.title) || 'Untitled board';
    const out = ['# ' + title, '<!-- board ' + boardId + ' · ' + ORIGIN + '/' + boardId + ' -->', ''];
    const tag = (el) => (withIds ? ' <!-- ' + el._id + ' -->' : '');
    let headerDone = false;
    const emit = (el, onCanvas) => {
      const c = el.content || {};
      switch (el.elementType) {
        case 'CARD': {
          const lines = docToText(c.textContent).split('\n');
          const m = lines[0].match(/^#{1,3}\s+(.*)$/);
          if (onCanvas && !headerDone && m && m[1].trim() === title) {
            // the title note: emitted as plain text so it round-trips as the header
            headerDone = true;
            const rest = lines.slice(1).join('\n').replace(/\n{2,}/g, '\n').trim();
            return [rest || title, ''];
          }
          if (m) return ['### ' + m[1] + tag(el), ...lines.slice(1), ''];
          return ['###' + tag(el), ...lines, ''];
        }
        case 'TASK_LIST': {
          const tasks = sortByPosition(childrenOf(elements, el._id, 'INBOX'));
          const lines = [(c.title ? '### ' + c.title : '###') + tag(el)];
          for (const t of tasks) lines.push('- [' + (t.content && t.content.isComplete ? 'x' : ' ') + '] ' + docToText(t.content && t.content.textContent).replace(/\s*\n\s*/g, ' '));
          if (!tasks.length) lines.push('- [ ] ');
          lines.push('');
          return lines;
        }
        case 'LINK': {
          const url = (c.link && c.link.url) || c.url;
          if (!url) return null;
          const caption = c.caption ? docToText(c.caption).replace(/\s*\n\s*/g, ' ') : '';
          return ['###' + tag(el), url + (caption ? ' ' + caption : ''), ''];
        }
        case 'IMAGE': case 'FILE': {
          const url = (c.image && (c.image.original || c.image.regular)) || (c.file && c.file.url) || (c.link && c.link.url);
          if (!url) return null;
          const caption = c.caption ? docToText(c.caption).replace(/\s*\n\s*/g, ' ') : (c.file && c.file.filename) || '';
          return ['###' + tag(el), url + (caption ? ' ' + caption : ''), ''];
        }
        case 'BOARD':
          return ['### Sub-board: ' + ((c.title) || 'Untitled board') + tag(el), ORIGIN + '/' + el._id, ''];
        default:
          return null;
      }
    };
    const canvas = sortByPosition(childrenOf(elements, boardId, 'CANVAS'));
    const columns = canvas.filter(e => e.elementType === 'COLUMN');
    for (const el of canvas) {
      if (el.elementType === 'COLUMN') continue;
      const lines = emit(el, true);
      if (lines) out.push(...lines);
    }
    for (const col of columns) {
      out.push('## ' + ((col.content && col.content.title) || 'Untitled column') + tag(col), '');
      for (const el of sortByPosition(childrenOf(elements, col._id, 'INBOX'))) {
        const lines = emit(el, false);
        if (lines) out.push(...lines);
      }
    }
    const unsorted = sortByPosition(childrenOf(elements, boardId, 'INBOX'));
    if (unsorted.length) {
      out.push('## Unsorted', '<!-- the board\'s Unsorted tray -->', '');
      for (const el of unsorted) { const lines = emit(el, false); if (lines) out.push(...lines); }
    }
    return out.join('\n').replace(/\n{3,}/g, '\n\n').trim() + '\n';
  }

  const tools = {
    async milanote_whoami(args) {
      if (args && args.fresh) {
        // re-check the session (the setup window polls this while the user signs in)
        const previousId = client.userId;
        client.user = null;
        client.userId = null;
        try { await ensureUser(); } catch (e) { client.user = null; client.userId = null; throw e; }
        if (previousId && previousId !== client.userId && socket.ws) {
          try { socket.ws.close(); } catch (e) { /* ignore */ }
        }
      }
      const u = await ensureUser();
      let usage = null;
      try {
        const c = await api('/users/me/counts');
        if (c) usage = { unlimited: !!c.isUnlimited, limit: c.contentLimit && c.contentLimit.current, used: c.totalUsage, exceeded: !!(c.contentLimit && c.contentLimit.exceeded), counts: c.counts };
      } catch (e) { log('warn', 'counts failed: ' + e.message); }
      return {
        userId: u._id,
        name: (u.name && u.name.displayName) || null,
        email: u.email || null,
        rootBoardId: u.rootBoardId,
        quickNotesBoardId: u.quickNotesRootId || null,
        usage,
        appUrl: ORIGIN + '/' + u.rootBoardId + '/home',
      };
    },

    async milanote_login() {
      return { action: 'open-login' }; // handled natively by the host
    },

    async milanote_list_boards(args) {
      const user = await ensureUser();
      const rootId = (args && args.boardId) || user.rootBoardId;
      const maxDepth = Math.min(Math.max((args && args.depth) || 3, 1), 6);
      const result = [];
      let requests = 0;
      const walk = async (boardId, path, depth) => {
        if (requests++ > 60) return;
        let elements;
        try { elements = await fetchBoard(boardId, true); } catch (e) { result.push({ id: boardId, path, error: e.message }); return; }
        const board = elements[boardId];
        const title = (board.content && board.content.title) || (boardId === user.rootBoardId ? 'Home' : 'Untitled board');
        const here = path.concat(title);
        const counts = {};
        for (const e of Object.values(elements)) { if (e._id !== boardId) counts[e.elementType] = (counts[e.elementType] || 0) + 1; }
        result.push({ id: boardId, title, path: here.join(' / '), url: ORIGIN + '/' + boardId, elements: counts });
        if (depth >= maxDepth) return;
        const subBoards = sortByPosition(Object.values(elements).filter(e => e.elementType === 'BOARD' && e._id !== boardId && e.location && e.location.section !== 'TRASH'));
        for (const b of subBoards) await walk(b._id, here, depth + 1);
      };
      await walk(rootId, [], 1);
      return { boards: result };
    },

    async milanote_get_board(args) {
      const user = await ensureUser();
      const boardId = (args && args.boardId) || user.rootBoardId;
      const elements = await fetchBoard(boardId, false);
      if (args && args.raw) return { elements };
      return boardSummary(elements, boardId);
    },

    async milanote_get_element(args) {
      const id = req(args, 'elementId', 'string');
      const ctx = await contextOf(id);
      if (args.raw) return ctx.element;
      const out = summarize(ctx.elements, id, 2) || {};
      out.boardId = ctx.boardId;
      return out;
    },

    async milanote_create_board(args) {
      const title = req(args, 'title', 'string');
      const p = await resolveParent(args);
      if (p.parentType !== 'BOARD') throw new BridgeError('Boards can only be created inside another board', 'args');
      return createBoardElement(p, title, args.x, args.y, args.color);
    },

    async milanote_create_note(args) {
      const text = req(args, 'text', 'string');
      const p = await resolveParent(args);
      const doc = textToDoc(text);
      const content = { textContent: doc };
      if (args.color) content.color = args.color;
      let location;
      if (p.parentType === 'BOARD') location = { section: 'CANVAS', position: nextCanvasPosition(p.elements, p.parentId, args.x, args.y) };
      else if (p.parentType === 'COLUMN') location = { section: 'INBOX', position: nextListPosition(p.elements, p.parentId, args.index) };
      else throw new BridgeError('Notes can be created in a board or a column (parent is a ' + p.parentType + ')', 'args');
      const id = await createElement({ elementType: 'CARD', parentId: p.parentId, boardId: p.boardId, section: location.section, position: location.position, content });
      return { id, parentId: p.parentId, section: location.section, position: location.position, text: docToText(doc) };
    },

    async milanote_create_column(args) {
      const title = req(args, 'title', 'string');
      const p = await resolveParent(args);
      if (p.parentType !== 'BOARD') throw new BridgeError('Columns can only be created on a board', 'args');
      const position = nextCanvasPosition(p.elements, p.parentId, args.x, args.y);
      const content = { title };
      if (args.color) content.color = args.color;
      const id = await createElement({ elementType: 'COLUMN', parentId: p.parentId, boardId: p.boardId, section: 'CANVAS', position, content });
      return { id, title, parentId: p.parentId, position };
    },

    async milanote_create_todo_list(args) {
      const p = await resolveParent(args);
      // tasks: strings, or {text, done} objects
      const tasks = (Array.isArray(args.tasks) ? args.tasks : [])
        .map(t => (t && typeof t === 'object') ? { text: String(t.text || ''), done: !!t.done } : { text: String(t), done: false })
        .filter(t => t.text.trim());
      let location;
      if (p.parentType === 'BOARD') location = { section: 'CANVAS', position: nextCanvasPosition(p.elements, p.parentId, args.x, args.y) };
      else if (p.parentType === 'COLUMN') location = { section: 'INBOX', position: nextListPosition(p.elements, p.parentId, args.index) };
      else throw new BridgeError('To-do lists can be created in a board or a column', 'args');
      const title = (args.title && String(args.title).trim()) || null;
      const listId = await createElement({
        elementType: 'TASK_LIST', parentId: p.parentId, boardId: p.boardId, section: location.section, position: location.position,
        content: { title, showTitle: !!title },
      });
      const taskIds = [];
      for (let i = 0; i < tasks.length; i++) {
        const content = { textContent: textToDoc(tasks[i].text) };
        if (tasks[i].done) content.isComplete = true;
        const taskId = await createElement({
          elementType: 'TASK', parentId: listId, boardId: p.boardId, section: 'INBOX',
          position: { index: i, score: i * SCORE_STEP },
          content,
        });
        taskIds.push(taskId);
      }
      return { id: listId, title, tasks: taskIds.map((id, i) => ({ id, text: tasks[i].text, done: tasks[i].done })), parentId: p.parentId };
    },

    async milanote_add_task(args) {
      const listId = req(args, 'listId', 'string');
      const text = req(args, 'text', 'string');
      const ctx = await contextOf(listId);
      if (ctx.element.elementType !== 'TASK_LIST') throw new BridgeError('listId must reference a to-do list (TASK_LIST), got ' + ctx.element.elementType, 'args');
      const elements = await fetchBoard(ctx.boardId, false);
      const position = nextListPosition(elements, listId, args.index);
      const content = { textContent: textToDoc(text) };
      if (args.done) content.isComplete = true;
      const id = await createElement({ elementType: 'TASK', parentId: listId, boardId: ctx.boardId, section: 'INBOX', position, content });
      return { id, listId, index: position.index, text };
    },

    async milanote_create_link(args) {
      const url = req(args, 'url', 'string');
      if (!/^https?:\/\//i.test(url)) throw new BridgeError('url must start with http:// or https://', 'args');
      const p = await resolveParent(args);
      let location;
      if (p.parentType === 'BOARD') location = { section: 'CANVAS', position: nextCanvasPosition(p.elements, p.parentId, args.x, args.y) };
      else if (p.parentType === 'COLUMN') location = { section: 'INBOX', position: nextListPosition(p.elements, p.parentId, args.index) };
      else throw new BridgeError('Links can be created in a board or a column', 'args');
      const id = await createElement({ elementType: 'LINK', parentId: p.parentId, boardId: p.boardId, section: location.section, position: location.position, content: { url } });
      const title = await decorateLink(id, url, args.caption, p.boardId);
      return { id, url, title, parentId: p.parentId };
    },

    // Builds a whole board (columns, notes, to-do lists, links) from a Markdown outline or a spec object in one call.
    async milanote_build_board(args) {
      let spec = args.spec;
      if (!spec && typeof args.outline === 'string') spec = outlineToSpec(args.outline);
      if (spec && typeof spec === 'object' && typeof spec.outline === 'string' && !spec.columns) spec = outlineToSpec(spec.outline);
      if (!spec || typeof spec !== 'object') throw new BridgeError('Pass "outline" (Markdown in the Milanote++ outline format) or "spec" (object with title/header/columns/items)', 'args');
      const columns = Array.isArray(spec.columns) ? spec.columns : [];
      const loose = Array.isArray(spec.items) ? spec.items : (Array.isArray(spec.notes) ? spec.notes : []);
      if (!columns.length && !loose.length && !spec.header && !spec.title) throw new BridgeError('The outline is empty: nothing to build', 'args');

      const p = await resolveParent(args);
      if (p.parentType !== 'BOARD') throw new BridgeError('Boards can only be built inside a board (parentId is a ' + p.parentType + ')', 'args');
      const makeNew = args.newBoard !== false && !!spec.title;
      const count = { boards: 0, columns: 0, notes: 0, todoLists: 0, tasks: 0, links: 0 };
      let boardId = p.parentId;
      let base;
      if (makeNew) {
        const b = await createBoardElement(p, String(spec.title), args.x, args.y, spec.color);
        boardId = b.id;
        count.boards++;
        base = { x: 2, y: 2, score: SCORE_STEP };
      } else {
        base = nextCanvasPosition(p.elements, boardId, args.x, args.y);
      }
      let score = base.score;
      const canvasPos = (x, y) => { const pos = { x, y, score }; score += SCORE_STEP; return pos; };

      const addItem = async (parentId, item, index, x, y) => {
        if (typeof item === 'string') item = { note: item };
        if (!item || typeof item !== 'object') throw new BridgeError('Invalid item: ' + JSON.stringify(item), 'args');
        const onCanvas = parentId === boardId;
        const loc = onCanvas ? { section: 'CANVAS', position: canvasPos(x, y) } : { section: 'INBOX', position: { index, score: index * SCORE_STEP } };
        if (item.note !== undefined) {
          const content = { textContent: textToDoc(String(item.note)) };
          if (item.color) content.color = item.color;
          await createElement({ elementType: 'CARD', parentId, boardId, section: loc.section, position: loc.position, content });
          count.notes++;
        } else if (item.todo !== undefined || Array.isArray(item.tasks)) {
          const title = String(item.todo !== undefined ? item.todo : (item.title || '')).trim();
          const listId = await createElement({ elementType: 'TASK_LIST', parentId, boardId, section: loc.section, position: loc.position, content: { title: title || null, showTitle: !!title } });
          count.todoLists++;
          const tasks = (Array.isArray(item.tasks) ? item.tasks : [])
            .map(t => (t && typeof t === 'object') ? { text: String(t.text || ''), done: !!t.done } : { text: String(t), done: false })
            .filter(t => t.text.trim());
          for (let i = 0; i < tasks.length; i++) {
            const content = { textContent: textToDoc(tasks[i].text) };
            if (tasks[i].done) content.isComplete = true;
            await createElement({ elementType: 'TASK', parentId: listId, boardId, section: 'INBOX', position: { index: i, score: i * SCORE_STEP }, content });
            count.tasks++;
          }
        } else if (item.link !== undefined || item.url !== undefined) {
          const url = String(item.link !== undefined ? item.link : item.url);
          if (!/^https?:\/\//i.test(url)) throw new BridgeError('Link "' + url + '" must start with http:// or https://', 'args');
          const id = await createElement({ elementType: 'LINK', parentId, boardId, section: loc.section, position: loc.position, content: { url } });
          await decorateLink(id, url, item.caption, boardId);
          count.links++;
        } else if (item.board !== undefined) {
          if (!onCanvas) throw new BridgeError('Sub-boards can only be placed on the canvas, not inside a column', 'args');
          await createElement({
            elementType: 'BOARD', parentId, boardId, section: 'CANVAS', position: loc.position,
            content: { title: String(item.board), icon: null, color: null, secondaryColor: null, defaultColorPalette: BOARD_PALETTES[Math.floor(Math.random() * BOARD_PALETTES.length)] },
          });
          count.boards++;
        } else {
          throw new BridgeError('Unknown item shape (expected note / todo+tasks / link / board): ' + JSON.stringify(item).slice(0, 120), 'args');
        }
      };

      // title note
      let y = base.y;
      let headerText = spec.header ? String(spec.header) : '';
      if (makeNew && headerText && !/^#\s/.test(headerText)) headerText = '# ' + spec.title + '\n' + headerText;
      if (headerText) {
        await createElement({ elementType: 'CARD', parentId: boardId, boardId, section: 'CANVAS', position: canvasPos(base.x, y), content: { textContent: textToDoc(headerText) } });
        count.notes++;
        y += HEADER_HEIGHT_GU;
      }
      // columns left to right, then loose items to their right
      let x = base.x;
      const made = [];
      for (const col of columns) {
        const title = String((col && col.title) || 'Untitled');
        const content = { title };
        if (col.color) content.color = col.color;
        const colId = await createElement({ elementType: 'COLUMN', parentId: boardId, boardId, section: 'CANVAS', position: canvasPos(x, y), content });
        count.columns++;
        made.push({ id: colId, title });
        const items = Array.isArray(col.items) ? col.items : [];
        for (let i = 0; i < items.length; i++) await addItem(colId, items[i], i);
        x += COLUMN_STEP_GU;
      }
      for (const item of loose) { await addItem(boardId, item, undefined, x, y); x += COLUMN_STEP_GU; }
      boardCache.delete(boardId);
      return {
        boardId, url: ORIGIN + '/' + boardId,
        title: makeNew ? spec.title : undefined,
        parentId: makeNew ? p.parentId : undefined,
        created: count,
        columns: made,
      };
    },

    // Reads a board back as a Markdown outline (same format milanote_build_board accepts).
    async milanote_export_board(args) {
      const user = await ensureUser();
      const boardId = (args && args.boardId) || user.rootBoardId;
      const elements = await fetchBoard(boardId, false);
      return boardToOutline(elements, boardId, !!(args && args.ids));
    },

    async milanote_update_element(args) {
      const id = req(args, 'elementId', 'string');
      const ctx = await contextOf(id);
      const el = ctx.element;
      const changes = {};
      if (args.text !== undefined) {
        if (el.elementType === 'CARD' || el.elementType === 'TASK') changes.textContent = textToDoc(String(args.text));
        else if (el.elementType === 'LINK' || el.elementType === 'IMAGE' || el.elementType === 'FILE') { changes.caption = textToDoc(String(args.text)); changes.showCaption = true; }
        else throw new BridgeError('"text" can only be set on notes, tasks, links and images', 'args');
      }
      if (args.title !== undefined) {
        if (!['BOARD', 'COLUMN', 'TASK_LIST'].includes(el.elementType)) throw new BridgeError('"title" can only be set on boards, columns and to-do lists', 'args');
        changes.title = String(args.title);
        if (el.elementType === 'TASK_LIST') changes.showTitle = !!changes.title;
      }
      if (args.done !== undefined) {
        if (el.elementType !== 'TASK') throw new BridgeError('"done" can only be set on tasks', 'args');
        changes.isComplete = !!args.done;
      }
      if (args.color !== undefined) changes.color = args.color || null;
      if (args.url !== undefined) {
        if (el.elementType !== 'LINK') throw new BridgeError('"url" can only be set on links', 'args');
        changes.url = String(args.url);
        changes.link = Object.assign({}, el.content && el.content.link, { url: String(args.url) });
      }
      if (!Object.keys(changes).length) throw new BridgeError('Nothing to update: pass text, title, done, color or url', 'args');
      await updateElement(id, changes, ctx);
      return { id, type: el.elementType, updated: Object.keys(changes) };
    },

    async milanote_move_element(args) {
      const id = req(args, 'elementId', 'string');
      const ctx = await contextOf(id);
      const el = ctx.element;
      const user = await ensureUser();
      const targetId = args.parentId || holderBoardOf(ctx) || user.rootBoardId;
      const target = await contextOf(targetId);
      const targetType = target.element.elementType;
      let boardElements = await fetchBoard(target.boardId, false);
      if (!boardElements[targetId]) boardElements[targetId] = target.element;
      let location;
      if (targetType === 'BOARD') {
        location = { parentId: targetId, section: 'CANVAS', position: nextCanvasPosition(boardElements, targetId, args.x, args.y) };
      } else if (targetType === 'COLUMN' || targetType === 'TASK_LIST') {
        if (targetType === 'TASK_LIST' && el.elementType !== 'TASK') throw new BridgeError('Only tasks can be moved into a to-do list', 'args');
        if (targetType === 'COLUMN' && (el.elementType === 'TASK' || el.elementType === 'COLUMN')) throw new BridgeError(el.elementType + ' elements cannot be placed inside a column', 'args');
        location = { parentId: targetId, section: 'INBOX', position: nextListPosition(boardElements, targetId, args.index) };
      } else {
        throw new BridgeError('Elements can be moved into a board, a column or a to-do list (target is a ' + targetType + ')', 'args');
      }
      if (el.elementType === 'BOARD' && targetType !== 'BOARD') throw new BridgeError('Boards can only be moved onto another board\'s canvas', 'args');
      if (el.elementType === 'BOARD' && (targetId === id)) throw new BridgeError('A board cannot be moved into itself', 'args');
      const from = { parentId: el.location.parentId, section: el.location.section, position: el.location.position };
      await moveElements([{ id, location, from }], 'DROP', holderBoardOf(ctx), target.boardId, { [id]: el.elementType });
      return { id, from, to: location };
    },

    async milanote_trash_element(args) {
      const ids = args.elementIds ? args.elementIds : [req(args, 'elementId', 'string')];
      if (!Array.isArray(ids) || !ids.length) throw new BridgeError('Pass elementId or a non-empty elementIds array', 'args');
      const user = await ensureUser();
      const now = Date.now();
      const byBoard = new Map();
      for (const id of ids) {
        const ctx = await contextOf(id);
        if (id === user.rootBoardId) throw new BridgeError('The Home board cannot be trashed', 'args');
        const el = ctx.element;
        const from = { parentId: el.location.parentId, section: el.location.section, position: el.location.position };
        const boardId = holderBoardOf(ctx);   // a sub-board goes to its parent board's trash
        if (!byBoard.has(boardId)) byBoard.set(boardId, { moves: [], types: {} });
        const b = byBoard.get(boardId);
        b.moves.push({ id, location: { parentId: boardId + '-trash', section: 'TRASH', position: { addedDate: now, restoreLocation: from } }, from });
        b.types[id] = el.elementType;
      }
      for (const [boardId, b] of byBoard) {
        await moveElements(b.moves, 'TRASH', boardId, boardId + '-trash', b.types);
      }
      return { trashed: ids, note: 'Elements were moved to the board trash and can be restored from Milanote.' };
    },

    async milanote_search(args) {
      const query = String(req(args, 'query', 'string')).toLowerCase();
      const user = await ensureUser();
      const rootId = (args && args.boardId) || user.rootBoardId;
      const maxDepth = Math.min(Math.max((args && args.depth) || 4, 1), 6);
      const hits = [];
      let requests = 0;
      const walk = async (boardId, path, depth) => {
        if (requests++ > 60 || hits.length >= 50) return;
        let elements;
        try { elements = await fetchBoard(boardId, true); } catch (e) { return; }
        const board = elements[boardId];
        const title = (board.content && board.content.title) || (boardId === user.rootBoardId ? 'Home' : 'Untitled board');
        const here = path.concat(title);
        for (const e of Object.values(elements)) {
          if (e.location && (e.location.section === 'TRASH' || e.location.section === 'DELETED')) continue;
          const s = summarize(elements, e._id, 0) || {};
          const hay = [s.title, s.text, s.url, s.linkTitle, s.caption, s.filename].filter(Boolean).join('\n').toLowerCase();
          if (hay && hay.includes(query)) {
            hits.push(Object.assign({ boardId, boardPath: here.join(' / ') }, s));
            if (hits.length >= 50) break;
          }
        }
        if (depth >= maxDepth) return;
        const subBoards = Object.values(elements).filter(e => e.elementType === 'BOARD' && e._id !== boardId && e.location && e.location.section !== 'TRASH');
        for (const b of subBoards) await walk(b._id, here, depth + 1);
      };
      await walk(rootId, [], 1);
      return { query, hits };
    },
  };

  async function call(tool, args) {
    const fn = tools[tool];
    if (!fn) throw new BridgeError('Unknown tool: ' + tool, 'args');
    return fn(args || {});
  }

  // ---------------------------------------------------------------- host wiring
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', (ev) => {
      const msg = ev.data;
      if (!msg || typeof msg !== 'object' || !msg.callId) return;
      Promise.resolve()
        .then(() => call(msg.tool, msg.args))
        .then((result) => post({ callId: msg.callId, result: result === undefined ? null : result }))
        .catch((err) => post({ callId: msg.callId, error: (err && err.message) || String(err), code: (err && err.code) || 'error' }));
    });
    post({ type: 'ready', origin: location.origin });
  }

  window.MilanoteBridge = {
    call, tools, textToDoc, docToText, outlineToSpec, boardToOutline, client,
    version: '1.1.1',
  };
})();
