// miniesp UI shell (/ui.js): theme, icons, sidebar navigation and small helpers shared by every page.
// Load it in <head> (no defer) so the theme is applied before the first paint. Guide: docs/UI_GUIDE.md
//   <body data-nav="ID"> <div class="app"> <main class="main"><div class="page">...</div></main> </div>
//   optional, before ui.js: <script>UI_APP={id,label,href,icon,pages:[{id,label,href}]}</script> (a package's own pages)
(function () {
  const LS = { get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : v } catch (e) { return d } }, set(k, v) { try { localStorage.setItem(k, v) } catch (e) { } } };
  const root = document.documentElement;
  function applyTheme(t) { if (t === 'light' || t === 'dark') root.dataset.theme = t; else delete root.dataset.theme; dispatchEvent(new Event('ui-theme')) }
  applyTheme(LS.get('ui-theme', 'system'));
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => dispatchEvent(new Event('ui-theme')));

  // ---- icons (24x24 stroke icons, used as <svg class="ic"><use href="#i-NAME"/></svg>) ----
  const P = {
    network: '<rect x="9" y="3" width="6" height="5" rx="1"/><rect x="3" y="16" width="6" height="5" rx="1"/><rect x="15" y="16" width="6" height="5" rx="1"/><path d="M12 8v4M6 16v-2h12v2"/>',
    server: '<rect x="3" y="4" width="18" height="7" rx="2"/><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M7 7.5h.01M7 16.5h.01"/>',
    activity: '<path d="M3 12h4l3-8 4 16 3-8h4"/>',
    package: '<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9L12 3z"/><path d="M4 7.5l8 4.5 8-4.5M12 12v9"/>',
    apps: '<path d="M12 3l3.5 6h-7z"/><rect x="4" y="13" width="7" height="7" rx="1"/><circle cx="16.5" cy="16.5" r="3.5"/>',
    terminal: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/>',
    droplet: '<path d="M12 3c3.5 4.2 6 7 6 10.2A6 6 0 0 1 6 13.2C6 10 8.5 7.2 12 3z"/>',
    sliders: '<path d="M4 7h10M18 7h2M4 17h4M12 17h8"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/>',
    settings: '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>',
    globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c2.6 2.6 3.9 5.6 3.9 9s-1.3 6.4-3.9 9c-2.6-2.6-3.9-5.6-3.9-9s1.3-6.4 3.9-9z"/>',
    wifi: '<path d="M2.5 9a14 14 0 0 1 19 0M5.8 12.5a9.5 9.5 0 0 1 12.4 0M9 16a5 5 0 0 1 6 0"/><circle cx="12" cy="19.5" r=".9"/>',
    cpu: '<rect x="5" y="5" width="14" height="14" rx="2"/><rect x="9" y="9" width="6" height="6" rx="1"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/>',
    memory: '<rect x="3" y="7" width="18" height="10" rx="2"/><path d="M7 7v10M11 7v10M15 7v10M7 20v-3M12 20v-3M17 20v-3"/>',
    clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
    info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>',
    copy: '<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V6a2 2 0 0 1 2-2h8"/>',
    check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
    external: '<path d="M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/>',
    user: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',
    lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
    down: '<path d="M6 9l6 6 6-6"/>', right: '<path d="M9 6l6 6-6 6"/>',
    menu: '<path d="M4 6h16M4 12h16M4 18h16"/>', x: '<path d="M6 6l12 12M18 6L6 18"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
    moon: '<path d="M20 14.5A8 8 0 1 1 9.5 4 6.5 6.5 0 0 0 20 14.5z"/>',
    monitor: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>',
    book: '<path d="M4 5a2 2 0 0 1 2-2h5v16H6a2 2 0 0 0-2 2V5zM20 5a2 2 0 0 0-2-2h-5v16h5a2 2 0 0 1 2 2V5z"/>',
    help: '<circle cx="12" cy="12" r="9"/><path d="M9.5 9.5a2.5 2.5 0 1 1 3.5 2.3c-.6.3-1 .9-1 1.6v.6M12 17h.01"/>',
    logout: '<path d="M15 4h4a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1h-4M10 16l-4-4 4-4M6 12h10"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>',
    download: '<path d="M12 4v11M7 10l5 5 5-5M5 20h14"/>',
    alert: '<path d="M12 4l9 16H3z"/><path d="M12 10v4M12 17h.01"/>',
    power: '<path d="M12 3v8M6.3 7a8 8 0 1 0 11.4 0"/>',
    plus: '<path d="M12 5v14M5 12h14"/>', save: '<path d="M5 3h11l3 3v15H5V3z"/><path d="M8 3v5h7V3M8 21v-6h8v6"/>',
    gauge: '<path d="M4 18a8 8 0 1 1 16 0"/><path d="M12 14l4-5"/>', disk: '<ellipse cx="12" cy="6" rx="8" ry="3"/><path d="M4 6v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6"/>'
  };
  const sprite = '<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0" style="position:absolute" aria-hidden="true"><defs>' +
    Object.keys(P).map(k => '<symbol id="i-' + k + '" viewBox="0 0 24 24">' + P[k] + '</symbol>').join('') + '</defs></svg>';
  const ico = (n, c) => '<svg class="ic' + (c ? ' ' + c : '') + '" aria-hidden="true"><use href="#i-' + n + '"/></svg>';
  const esc = t => String(t).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const LOGO = '<svg class="logo" viewBox="0 0 24 24" aria-hidden="true"><g fill="currentColor"><circle cx="5" cy="5" r="2"/><circle cx="12" cy="5" r="2"/><circle cx="19" cy="5" r="2" opacity=".45"/><circle cx="5" cy="12" r="2"/><circle cx="12" cy="12" r="2"/><circle cx="19" cy="12" r="2"/><circle cx="5" cy="19" r="2" opacity=".45"/><circle cx="12" cy="19" r="2"/><circle cx="19" cy="19" r="2" opacity=".45"/></g></svg>';

  // ---- navigation model ----
  function apps() { let a = []; try { a = JSON.parse(LS.get('ui-apps', '[]')) } catch (e) { } const cur = window.UI_APP; if (cur && !a.some(x => x.id === cur.id)) a.push({ id: cur.id, label: cur.label, href: cur.href, icon: cur.icon }); return a }
  function model() {
    return [
      { id: 'g-system', label: 'System', icon: 'network', kids: [{ id: 'machine', label: 'Machine', href: '/#machine' }, { id: 'services', label: 'Services', href: '/#services' }, { id: 'programs', label: 'Programs', href: '/#programs' }] },
      { id: 'g-apps', label: 'Apps', icon: 'apps', kids: [{ id: 'apps', label: 'All apps', href: '/#apps' }].concat(apps().map(a => ({ id: 'app-' + a.id, label: a.label, href: a.href, pages: window.UI_APP && UI_APP.id === a.id ? UI_APP.pages : null }))) }
    ];
  }
  const host = () => (window.UI_HOST || LS.get('ui-host', '') || location.hostname || 'miniesp').replace(/\.local$/, '');   // the console stores the real name
  const isIp = h => /^[0-9.]+$|:/.test(h);
  function link(k, act) { return '<a href="' + esc(k.href) + '"' + (k.id === act ? ' aria-current="page"' : '') + '>' + esc(k.label) + '</a>' }
  function navHtml(act) {
    return model().map(g => {
      const open = LS.get('ui-nav-' + g.id, '1') === '1';
      return '<button type="button" data-g="' + g.id + '" aria-expanded="' + open + '">' + ico(g.icon) + '<span>' + g.label + '</span>' + ico('down', 'chev') + '</button>' +
        '<div class="kids"' + (open ? '' : ' hidden') + '>' + g.kids.map(k => link(k, act) + (k.pages ? '<div class="kids">' + k.pages.map(p => link(p, act)).join('') + '</div>' : '')).join('') + '</div>';
    }).join('');
  }
  function render() {
    const act = document.body.dataset.nav || '';
    const side = document.getElementById('ui-side'); if (!side) return;
    side.innerHTML = '<div class="side-head">' + LOGO + '<span class="name">' + esc(host()) + '</span><span class="badge">ESP32</span></div>' +
      '<nav class="nav" aria-label="Main">' + navHtml(act) + '</nav>' +
      '<div class="side-foot nav"><a href="https://github.com/hamimmahmud0/miniesp#readme" target="_blank" rel="noopener">' + ico('book') + '<span>Documentation</span></a>' +
      '<a href="https://github.com/hamimmahmud0/miniesp/issues" target="_blank" rel="noopener">' + ico('help') + '<span>Help</span></a></div>' +
      '<button type="button" class="who nav-who" id="ui-who" aria-haspopup="menu" style="border:0;border-top:1px solid var(--border);background:none;color:inherit;font:inherit;text-align:left;cursor:pointer;width:100%"><span class="av">' + ico('user') + '</span><div><b>esp</b><span>' + esc(host()) + (isIp(host()) ? '' : '.local') + '</span></div></button>';
    side.querySelectorAll('[data-g]').forEach(b => b.onclick = () => { const o = b.getAttribute('aria-expanded') !== 'true'; b.setAttribute('aria-expanded', o); b.nextElementSibling.hidden = !o; LS.set('ui-nav-' + b.dataset.g, o ? '1' : '0') });
    document.getElementById('ui-who').onclick = e => { e.stopPropagation(); whoMenu(e.currentTarget) };
    const tb = document.getElementById('ui-topname'); if (tb) tb.textContent = host();
  }
  function whoMenu(anchor) {
    closeMenu();
    const t = LS.get('ui-theme', 'system'), m = document.createElement('div');
    m.className = 'menu'; m.id = 'ui-menu'; m.setAttribute('role', 'menu');
    m.innerHTML = '<div class="mh">Appearance</div>' + [['system', 'monitor', 'Use system setting'], ['light', 'sun', 'Light'], ['dark', 'moon', 'Dark']].map(o =>
      '<button role="menuitemradio" aria-checked="' + (t === o[0]) + '" data-t="' + o[0] + '">' + ico(o[1]) + o[2] + ico('check', 'ok-mark') + '</button>').join('') +
      (window.UI.logout ? '<hr><button class="danger" data-out="1">' + ico('logout') + 'Log out</button>' : '');
    document.body.appendChild(m);
    const r = anchor.getBoundingClientRect(); m.style.left = Math.max(8, r.left + 8) + 'px'; m.style.width = Math.min(r.width - 16, 300) + 'px'; m.style.top = Math.max(8, r.top - m.offsetHeight - 6) + 'px'; m.style.position = 'fixed';
    m.querySelectorAll('[data-t]').forEach(b => b.onclick = () => { LS.set('ui-theme', b.dataset.t); applyTheme(b.dataset.t); closeMenu() });
    const o = m.querySelector('[data-out]'); if (o) o.onclick = () => { closeMenu(); window.UI.logout() };
    m.querySelector('button').focus();
  }
  function closeMenu() { const m = document.getElementById('ui-menu'); if (m) m.remove() }
  addEventListener('click', e => { const m = document.getElementById('ui-menu'); if (m && !m.contains(e.target)) closeMenu() });
  addEventListener('keydown', e => { if (e.key === 'Escape') { closeMenu(); drawer(false) } });
  function drawer(on) {
    const app = document.querySelector('.app'); if (!app) return;
    app.classList.toggle('open', on);
    let s = document.getElementById('ui-scrim');
    if (on && !s) { s = document.createElement('div'); s.id = 'ui-scrim'; s.className = 'scrim'; s.onclick = () => drawer(false); app.appendChild(s) }
    if (!on && s) s.remove();
  }

  function build() {
    document.body.insertAdjacentHTML('afterbegin', sprite);
    const app = document.querySelector('.app'); if (!app) return;
    app.insertAdjacentHTML('afterbegin', '<header class="topbar"><button class="iconbtn" type="button" id="ui-burger" aria-label="Open navigation">' + ico('menu') + '</button>' + LOGO + '<span class="name" id="ui-topname"></span></header><aside class="side" id="ui-side"></aside>');
    document.getElementById('ui-burger').onclick = () => drawer(true);
    render();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', build); else build();

  // ---- helpers for pages ----
  function copy(text, btn) {
    const done = () => { toast('Copied'); if (btn) { const old = btn.innerHTML; btn.classList.add('done'); btn.innerHTML = ico('check'); setTimeout(() => { btn.classList.remove('done'); btn.innerHTML = old }, 1400) } };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(done);
    else { const a = document.createElement('textarea'); a.value = text; a.style.position = 'fixed'; a.style.opacity = '0'; document.body.appendChild(a); a.select(); try { document.execCommand('copy'); done() } catch (e) { } a.remove() }
  }
  let tt; function toast(msg) { let t = document.getElementById('ui-toast'); if (!t) { t = document.createElement('div'); t.id = 'ui-toast'; t.className = 'toast'; t.setAttribute('role', 'status'); document.body.appendChild(t) } t.textContent = msg; t.hidden = false; clearTimeout(tt); tt = setTimeout(() => t.hidden = true, 1800) }
  window.UI = {
    ico, esc, copy, toast, ls: LS.get, lset: LS.set,
    setActive(id) { document.body.dataset.nav = id; render(); drawer(false) },
    setHost(name) { window.UI_HOST = name; LS.set('ui-host', name); render() },
    setApps(list) { LS.set('ui-apps', JSON.stringify(list)); render() },   // [{id,label,href,icon}] - the console stores the installed apps
    logout: null
  };
})();
