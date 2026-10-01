// Milanote++ overlay
// ------------------------------------------------------------------
// Injected by the setup window into Milanote pages it shows (login page and the app).
// Draws a small panel anchored to the bottom-right corner that can be collapsed to a round
// button; its "Back to Milanote++ setup" button tells the native side to show the setup page.
// Everything lives in a shadow root so Milanote's styles and ours never touch.
// ------------------------------------------------------------------
(function () {
  'use strict';
  if (document.getElementById('milanotepp-overlay')) return;

  const STORAGE_KEY = 'milanotepp.overlay';
  const post = (msg) => { try { if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(msg); } catch (e) { /* ignore */ } };

  let collapsed = false;
  try { collapsed = !!JSON.parse(localStorage.getItem(STORAGE_KEY) || '{}').collapsed; } catch (e) { /* ignore */ }
  const save = () => { try { localStorage.setItem(STORAGE_KEY, JSON.stringify({ collapsed })); } catch (e) { /* ignore */ } };

  const host = document.createElement('div');
  host.id = 'milanotepp-overlay';
  host.style.cssText = 'all:initial;position:fixed;right:16px;bottom:16px;z-index:2147483647;';
  const root = host.attachShadow({ mode: 'open' });
  root.innerHTML = `
    <style>
      :host { all: initial; }
      * { box-sizing: border-box; }
      .panel {
        width: 232px;
        background: #22242d; color: #f3f3f5; border: 1px solid #3a3d4a; border-radius: 14px;
        box-shadow: 0 10px 30px rgba(0,0,0,.45); font: 13px/1.4 "Segoe UI", system-ui, sans-serif;
        user-select: none; overflow: hidden;
      }
      .head {
        display: flex; align-items: center; gap: 8px; padding: 9px 10px 9px 12px;
        background: linear-gradient(180deg, #2b2e3a, #22242d); border-bottom: 1px solid #3a3d4a;
      }
      .logo { width: 20px; height: 20px; border-radius: 5px; flex: none; }
      .title { font-weight: 600; letter-spacing: .01em; flex: 1; white-space: nowrap; }
      .title b { color: #ff3d7f; font-weight: 700; }
      .iconbtn {
        width: 24px; height: 24px; border: 0; border-radius: 7px; background: transparent; color: #c9cbd4;
        font: 16px/24px "Segoe UI", system-ui, sans-serif; cursor: pointer; padding: 0;
      }
      .iconbtn:hover { background: rgba(255,255,255,.10); color: #fff; }
      .body { padding: 10px; display: grid; gap: 8px; }
      .btn {
        display: block; width: 100%; padding: 9px 12px; border-radius: 9px; border: 1px solid #ff3d7f;
        background: #ff3d7f; color: #fff; font: 600 13px "Segoe UI", system-ui, sans-serif; cursor: pointer; text-align: left;
      }
      .btn:hover { background: #ff5a93; border-color: #ff5a93; }
      .fab {
        width: 46px; height: 46px; border-radius: 50%; border: 1px solid #3a3d4a; padding: 0;
        background: #22242d; box-shadow: 0 8px 24px rgba(0,0,0,.45); cursor: pointer;
        display: flex; align-items: center; justify-content: center; margin-left: auto;
      }
      .fab:hover { background: #2b2e3a; }
      .fab img { width: 30px; height: 30px; border-radius: 7px; pointer-events: none; }
      .hidden { display: none; }
    </style>
    <div class="panel">
      <div class="head">
        <img class="logo" alt="">
        <div class="title">Milanote<b>++</b></div>
        <button class="iconbtn" data-collapse title="Collapse">&#8211;</button>
      </div>
      <div class="body">
        <button class="btn" data-back>&#8592; Back to Milanote++ setup</button>
      </div>
    </div>
    <button class="fab hidden" data-expand title="Milanote++ menu"><img alt=""></button>
  `;

  // Tiny inline version of the app icon so the overlay is self-contained.
  const ICON = 'data:image/svg+xml;utf8,' + encodeURIComponent(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="14" fill="#1b1d25"/>' +
    '<path d="M16 46V18l16 16 16-16v28H16z" fill="none" stroke="#f3f3f5" stroke-width="5" stroke-linejoin="round"/>' +
    '<path d="M40 40h8M44 36v8M50 40h8M54 36v8" stroke="#ff3d7f" stroke-width="4" stroke-linecap="round"/></svg>');
  root.querySelectorAll('img').forEach(img => { img.src = ICON; });

  const panel = root.querySelector('.panel');
  const fab = root.querySelector('.fab');
  function render() {
    panel.classList.toggle('hidden', collapsed);
    fab.classList.toggle('hidden', !collapsed);
  }
  root.querySelector('[data-collapse]').addEventListener('click', () => { collapsed = true; render(); save(); });
  fab.addEventListener('click', () => { collapsed = false; render(); save(); });
  root.querySelector('[data-back]').addEventListener('click', () => post({ type: 'setup', action: 'back' }));

  (document.body || document.documentElement).appendChild(host);
  render();
  // Milanote re-renders its root on route changes; make sure our host stays attached.
  new MutationObserver(() => { if (!host.isConnected) (document.body || document.documentElement).appendChild(host); }).observe(document.documentElement, { childList: true });
})();
