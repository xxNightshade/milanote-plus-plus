// Milanote++ blackout
// ------------------------------------------------------------------
// Injected by the setup window into every page it shows (setup page and Milanote). While Claude Desktop
// is not running it covers the whole window with an opaque panel that asks the user to open Claude Desktop
// (or to download it when it is not installed). The native side owns the facts and sends them as
// {type:'claude-status', running, installed, store, skipped, downloadUrl, error}; the buttons send
// {type:'claude', action:'open'|'download'|'recheck'|'skip'|'status'} back. Only DOM in here.
// ------------------------------------------------------------------
(function () {
  'use strict';
  if (window.__milanoteppBlackout) return;
  window.__milanoteppBlackout = true;

  const post = (msg) => { try { if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(msg); } catch (e) { /* ignore */ } };

  const host = document.createElement('div');
  host.id = 'milanotepp-blackout';
  host.style.cssText = 'all:initial;position:fixed;inset:0;z-index:2147483647;display:none;';
  const root = host.attachShadow({ mode: 'open' });
  root.innerHTML = `
    <style>
      :host { all: initial; }
      * { box-sizing: border-box; }
      .cover {
        position: absolute; inset: 0; background: #0b0c10; color: #f3f3f5;
        display: flex; align-items: center; justify-content: center; padding: 24px;
        font: 15px/1.5 "Segoe UI", system-ui, sans-serif; user-select: none;
      }
      .card { width: min(520px, 100%); text-align: center; }
      .logo { width: 72px; height: 72px; border-radius: 18px; box-shadow: 0 10px 30px rgba(0,0,0,.6); margin-bottom: 22px; }
      h1 { margin: 0 0 10px; font-size: 24px; font-weight: 600; letter-spacing: -0.01em; }
      p { margin: 0 0 22px; color: #b9bcc8; }
      .row { display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; }
      .btn {
        padding: 11px 20px; border-radius: 10px; border: 1px solid #3a3d4a; background: #22242d; color: #f3f3f5;
        font: 600 14px "Segoe UI", system-ui, sans-serif; cursor: pointer;
      }
      .btn:hover { background: #2b2e3a; }
      .btn.primary { background: #ff3d7f; border-color: #ff3d7f; color: #fff; }
      .btn.primary:hover { background: #ff5a93; border-color: #ff5a93; }
      .url { margin: 16px 0 0; font: 13px Consolas, "Cascadia Mono", monospace; color: #8e92a3; word-break: break-all; }
      .error { margin: 16px 0 0; color: #ff8a8a; font-size: 14px; }
      .skip { display: inline-block; margin-top: 28px; color: #8e92a3; font-size: 13px; cursor: pointer; text-decoration: underline; }
      .skip:hover { color: #c9cbd4; }
      .hidden { display: none !important; }
    </style>
    <div class="cover">
      <div class="card">
        <img class="logo" alt="">
        <h1 data-title>Claude Desktop isn't running</h1>
        <p data-text></p>
        <div class="row">
          <button class="btn primary" data-open>Open Claude Desktop</button>
          <button class="btn primary" data-download>Download Claude Desktop</button>
          <button class="btn" data-recheck>Check again</button>
        </div>
        <div class="url hidden" data-url></div>
        <div class="error hidden" data-error></div>
        <span class="skip" data-skip>Using Claude Code instead? Continue without Claude Desktop</span>
      </div>
    </div>
  `;

  const ICON = 'data:image/svg+xml;utf8,' + encodeURIComponent(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="14" fill="#1b1d25"/>' +
    '<path d="M16 46V18l16 16 16-16v28H16z" fill="none" stroke="#f3f3f5" stroke-width="5" stroke-linejoin="round"/>' +
    '<path d="M40 40h8M44 36v8M50 40h8M54 36v8" stroke="#ff3d7f" stroke-width="4" stroke-linecap="round"/></svg>');
  root.querySelector('.logo').src = ICON;

  const el = (sel) => root.querySelector(sel);
  const show = (node, on) => node.classList.toggle('hidden', !on);

  let overlayPanel = null;   // the small Milanote++ panel is hidden while the blackout is up
  function render(s) {
    const visible = !s.running && !s.skipped;
    host.style.display = visible ? 'block' : 'none';
    if (s.installed) {
      el('[data-title]').textContent = 'Claude Desktop isn’t running';
      el('[data-text]').textContent = 'Milanote++ works through Claude Desktop. Open it to use Claude with your Milanote boards — this screen goes away by itself once it is running.';
    } else {
      el('[data-title]').textContent = 'Claude Desktop isn’t installed';
      el('[data-text]').textContent = 'Milanote++ works through Claude Desktop. Download and install it, sign in with your Claude account, open it, and this screen goes away by itself.';
    }
    show(el('[data-open]'), !!s.installed);
    show(el('[data-download]'), !s.installed);
    show(el('[data-url]'), !s.installed);
    el('[data-url]').textContent = s.downloadUrl || '';
    show(el('[data-error]'), !!s.error);
    el('[data-error]').textContent = s.error || '';
    const panel = document.getElementById('milanotepp-overlay');
    if (panel) {
      if (visible) { overlayPanel = panel; panel.style.display = 'none'; }
      else panel.style.display = '';
    } else if (overlayPanel && !visible) {
      overlayPanel.style.display = '';
    }
  }

  el('[data-open]').addEventListener('click', () => post({ type: 'claude', action: 'open' }));
  el('[data-download]').addEventListener('click', () => post({ type: 'claude', action: 'download' }));
  el('[data-recheck]').addEventListener('click', () => post({ type: 'claude', action: 'recheck' }));
  el('[data-skip]').addEventListener('click', () => post({ type: 'claude', action: 'skip' }));

  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', (ev) => {
      const m = ev.data;
      if (m && m.type === 'claude-status') render(m);
    });
  }

  const attach = () => { if (!host.isConnected) (document.body || document.documentElement).appendChild(host); };
  attach();
  // Milanote re-renders its root on route changes; stay attached and on top.
  new MutationObserver(() => {
    attach();
    if (host.style.display !== 'none' && host.parentNode && host.parentNode.lastElementChild !== host) host.parentNode.appendChild(host);
  }).observe(document.documentElement, { childList: true, subtree: false });

  post({ type: 'claude', action: 'status' });   // ask the native side for the current facts
})();
