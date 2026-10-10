# miniesp UI guide (for people and agents building web pages)

Every page served by the board uses one design system: **`/style.css`** (tokens + components) and **`/ui.js`** (theme, icons,
sidebar, helpers). Both live in `fs_image/www/` and ship with the OS. A package (e.g. `wtms` in miniesp-pkg) links them by absolute
path and adds only its own small stylesheet. Do not copy the tokens or the shell into a page: change `style.css` instead.

The look is an admin console: a left sidebar with grouped navigation, flat surfaces with 1 px hairline borders, a single blue accent,
uppercase table headers, generous whitespace, dark by default (follows the system; the user can pick Light/Dark in the menu at the
bottom of the sidebar).

## 1. Page skeleton
```html
<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Thing - miniesp</title>
<link rel="stylesheet" href="/style.css"><link rel="stylesheet" href="mypkg.css">   <!-- optional, page-specific only -->
<script src="/ui.js"></script>                                                     <!-- in <head>, no defer: avoids a theme flash -->
</head>
<body data-nav="ID">                         <!-- which sidebar entry is current, see 2. -->
<div class="app"><main class="main"><div class="page">        <!-- .page.narrow for settings/forms (max 840 px) -->
  <div class="ph"><div><h1>Title</h1><p>One sentence about the page. <a href="...">See how it works</a></p></div>
    <div class="actions"><button class="btn btn-primary">Main action</button></div></div>
  ... content ...
</div></main></div>
<script> /* page code; use UI.* helpers */ </script>
</body></html>
```
`ui.js` inserts the icon sprite, the sidebar (`<aside class="side">`) and the phone top bar into `.app`. Pages never write their own
header or navigation.

## 2. Navigation
- OS entries: `machine`, `services`, `programs`, `apps` (the console at `/`, hash routes). The console calls `UI.setActive(id)`.
- A package declares itself **before** the shell renders (define it in the package's shared JS loaded at the end of `<body>`, or in an
  inline script): `window.UI_APP={id:'wtms',label:'Water tank',href:'/tank/',icon:'droplet',pages:[{id:'tank',label:'Dashboard',href:'/tank/'},{id:'tank-settings',label:'Settings',href:'/tank/config.html'}]}`
  and sets `<body data-nav="tank">` (a page id). Its pages appear nested under *Apps > Water tank*.
- The console remembers installed apps (`UI.setApps`, localStorage `ui-apps`) so every page shows them; add new apps to the `APPS` map in
  `fs_image/www/index.html` (id = package name).
- `UI.logout = fn` adds *Log out* to the account menu (bottom of the sidebar).

## 3. Tokens (CSS custom properties; never hard-code colours)
| Token | Use |
|---|---|
| `--bg` `--side` `--surface` `--surface-2` `--hover` | page, sidebar, cards, fields/secondary buttons, hover/active rows |
| `--border` `--border-2` | hairlines; input/button borders |
| `--text` `--text-2` `--mute` `--faint` | body text, descriptions, labels/hints, placeholders |
| `--primary` `--primary-h` `--on-primary` `--link` | primary buttons, hover, text on primary, links and active tabs |
| `--ok` `--warn` `--bad` `--info` `--off` | status colours (dots, badges, alerts) |
| `--c1`..`--c5` | data series in charts (blue, teal, orange, violet, amber) |
| `--gap` (16) `--pad` (24) `--r` (6) `--r-lg` (8) | grid gaps, card padding, control / card radius |
| `--fs` 15, `--fs-sm` 13, `--fs-xs` 12, `--fs-h1` 28, `--fs-h2` 20, `--fs-h3` 16 | type scale (Inter, falling back to the system font) |
Light values are on `:root`; dark values apply for `prefers-color-scheme: dark` unless `<html data-theme="light">`, and always for
`data-theme="dark"`. Older names `--fg --card --line --soft --acc` are aliases (for `chart.js`); prefer the new ones.
Charts read colours with `getComputedStyle` at draw time and must redraw on the `ui-theme` window event.

## 4. Components (class names)
- **Page header** `.ph` (title block + `.actions`). **Section** `.section > h2 + p` (DNS-style stacked sections). Status line `.status` + `.dot.ok|warn|bad|info`.
- **Buttons** `.btn` (secondary), `.btn-primary` (one per view), `.btn-ghost` (link-like), `.btn-danger`, `.btn-ok` / `.btn-bad` (only for
  real "turn on/off" hardware actions), sizes `.btn-sm`, `.btn-icon`; group `.btns`. Icon-only button: `.iconbtn` with `aria-label`.
- **Cards** `.card` (`h2` with an icon inside; `.card-head` = title + controls on one line), `.card.flush`, `.split` (side-by-side cells
  with dividers), `.steps` (numbered how-to tiles), `.callout` (navy hero with an `<ol>` checklist; `li.done` strikes through), `.banner`.
- **Empty state** `.empty` > `svg.ic.ic-lg` + `h3` + `p` + `.btns` (primary action + `btn-ghost` "Learn more").
- **Forms** `.field` (label with `<small>` hint on the left, control on the right; stacks below 560 px), `.input`/native inputs,
  `.search` (icon + input), `.copyfield` (mono value + copy `.iconbtn`), `.switch` (`<label class="switch"><input type="checkbox"><span></span></label>`).
- **Tables** `.table-wrap > table.table` (uppercase headers), add `.rwd` to turn rows into label/value cards on phones (put
  `data-label` on each `td`). Above a table: `.toolbar` (search, filters, `.end` for the right-hand icon button) and a count `.badge`.
- **Badges / tags** `.badge` (`.ok .warn .bad .premium`), `.tag` (small square chip, e.g. a feature flag), `.chips`.
- **Tabs** `.tabs` (underlined, `aria-selected=true`/`aria-current=page`/`.cur`) for page sections; `.seg` (segmented buttons, `.cur`) for
  ranges and filters inside a card.
- **Data** `.kv` (key/value list), `.meter` > `.t` + `.track > i` (`.warn` > 75 %, `.bad` > 90 %), `.stats > .stat` (span label, b value, small note).
- **Layout** `.grid-2`, `.grid-3`, `.stack` (vertical gap), `.grid`; all use `--gap`. Never add ad-hoc margins between cards: use a grid
  or `.stack` so every gap is the same.
- **Feedback** `.alert` (+ `.warn`, `.info`) with a leading icon; `UI.toast('Saved')`; `.menu` popovers.
- **Charts** `.chartbox > canvas` + `.legend` + `.tip` (tooltip), drawn by `/tank/chart.js`-style canvas code (no CDN libraries: the
  board may have no internet).

Icons: `<svg class="ic"><use href="#i-NAME"/></svg>`; names are the keys of `P` in `ui.js` (network, server, activity, package, apps,
terminal, droplet, sliders, settings, globe, wifi, cpu, memory, clock, info, copy, check, external, user, lock, down, right, menu, x,
sun, moon, monitor, book, help, logout, search, download, alert, power, plus, save, gauge, disk). Add new ones there (24x24, stroke).

## 5. JavaScript helpers (`window.UI`)
`UI.ico(name)`, `UI.esc(text)`, `UI.copy(text, button?)`, `UI.toast(msg)`, `UI.ls(key, def)` / `UI.lset(key, value)` (safe localStorage),
`UI.setActive(id)`, `UI.setHost(name)`, `UI.setApps(list)`, `UI.logout`. Events: `ui-theme` (theme or system scheme changed).

## 6. Rules
1. **Size and requests matter**: the web server has one task and the runtime one program; keep pages small, avoid extra files, and never
   poll a CGI faster than every 5 s. Static JSON written by a service (like `/tank/state.json`) is better than a CGI call.
2. **CGI URLs**: arguments are words split by `+`; no cache-buster arguments (`&_=...`), use `{cache:'no-store'}`.
3. **Accessibility**: real `<button>`/`<a>`, labels for inputs, `aria-label` on icon buttons, visible focus (provided), colour is never the
   only signal (status dots come with text), `role="img"` + `aria-label` on canvases.
4. **Responsive**: the sidebar becomes a drawer at <= 900 px; test 1920, 1366, 1024, 820, 390 and 360 px wide, light and dark, and check
   `document.documentElement.scrollWidth <= innerWidth` (the body hides horizontal overflow, so also look at the screenshots).
5. **Copy**: sentence case, short descriptions under the title with one "See how ..." link, buttons say what they do ("Add nameserver",
   not "Submit").
6. **Hardware actions** (pump, GPIO, relays) need a confirmation dialog and `btn-ok`/`btn-bad`; everything else uses neutral buttons.

## 7. Testing a page without the board
Serve `fs_image/www/` (and a package's `web/` under its path) with any static server and stub the CGIs: `miniesp-pkg/tools/sim/mock_server.py`
does this for `/cgi-bin/wtms`; drop JSON captures of `/cgi-bin/console` and `/cgi-bin/sysinfo` into the served directory for the console.
Then use Playwright (`playwright-cli open`, `resize W H`, `set-color-scheme dark`, `screenshot --full-page`).

## 8. Shipping
`style.css`, `ui.js` and `index.html` are system files: `idf.py build` packs them into the image and `tools/sync.sh` (run by `tools/ota.sh`)
updates them on a board when they differ. A package page depends on them, so update the OS files before (or with) a package that uses
new classes.
