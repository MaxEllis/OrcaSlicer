// The polish pass after the usability re-check (usability/POLISH.md, P1-P6): rows the assistant's own preset switch
// dropped (P1), colours measured on the host's real backgrounds and the host's button defaults undone (P2), labels
// that wrap (P3), keyboard tags placed inside the pane and off the control (P5), and the review minors m14 / m15 (P6).
import { test } from 'node:test';
import assert from 'node:assert';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '../../resources/web/orcaslicer_mcp/panel.html'), 'utf8');
const core = html.match(/<script id="panel-core">([\s\S]*?)<\/script>/)[1];
const view = html.match(/<script id="panel-view">([\s\S]*?)<\/script>/)[1];
const css = html.match(/<style>([\s\S]*?)<\/style>/)[1];

function loadCore() {
  const ctx = vm.createContext({ window: {} });
  vm.runInContext(core, ctx);
  return ctx.window.McpPanelCore;
}
const Core = loadCore();

function element() {
  return { innerHTML: '', textContent: '', hidden: true, scrollTop: 0, style: {}, attrs: {}, offsetWidth: 0, offsetHeight: 0,
           setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k]; } };
}

// A page with a little more DOM than the other suites: querySelector finds a data-fk in the list's markup, focus is
// recorded, the root takes CSS properties, and the focus tag element exists.
function page(opts = {}) {
  const els = { list: element(), fold: element(), gear: element(), ftip: element() };
  const props = {};
  const root = element();
  root.style = { setProperty: (k, v) => { props[k] = v; } };
  root.clientWidth = opts.vw || 280;
  root.clientHeight = opts.vh || 500;
  const sent = [];
  const focused = [];
  let pageHasFocus = opts.hasFocus !== undefined ? opts.hasFocus : true;
  const window = {
    orca: { postMessage: (m) => sent.push(JSON.parse(JSON.stringify(m))), onMessage: () => {} },
    setTimeout: () => 0, clearTimeout: () => {}, print: () => {},
  };
  if (opts.bg) window.getComputedStyle = () => ({ backgroundColor: opts.bg });
  const winHandlers = {};
  window.addEventListener = (type, fn) => { winHandlers[type] = fn; };
  const handlers = {};
  const document = {
    getElementById: (id) => els[id], addEventListener: (type, fn) => { handlers[type] = fn; },
    body: {}, documentElement: root,
    hasFocus: () => pageHasFocus,
    activeElement: null,
    querySelector: (sel) => {
      const m = /^\[data-fk="([^"]*)"\]$/.exec(sel);
      if (!m || !els.list.innerHTML.includes('data-fk="' + m[1] + '"')) return null;
      return { focus: () => focused.push(m[1]) };
    },
  };
  const ctx = vm.createContext({ window, document });
  vm.runInContext(core, ctx);
  vm.runInContext(view, ctx);
  function click(attr, value) {
    const el = { getAttribute: (k) => (k === attr ? String(value) : null), hasAttribute: (k) => k === attr, parentNode: null };
    handlers.click({ target: { ...el, closest: () => null }, stopPropagation() {} });
  }
  return { sent, focused, props, root, els, handlers, winHandlers, list: () => els.list.innerHTML,
           receive: (m) => window.__mcpPanelReceive(m), click, setFocus: (f) => { pageHasFocus = f; } };
}

const rowOf = (o) => ({ id: 1, group: 1, ts: 100, time: '10:00', kind: 'setting', label: 'Bottom shell layers', value: '5', before: '4',
  where: '', undo: { verb: 'Revert' }, undone_at: '', title_hint: null, result: null, error: '', on_disk: false, ...o });
const g0 = { id: 0, ts: 50, time: '09:50', restore: { available: true, why: '' } };
const historyOf = (o) => ({ type: 'history', version: 1, rows: [rowOf({})], groups: [g0, { id: 1, ts: 100, time: '10:00', restore: null }],
  capabilities: { restore: true, open_settings: true, settings_hint: '', tracked_live: true }, restored_to: null, can_undo_restore: false, ...o });

// ---- P1: rows the assistant's own preset switch dropped
test('p1_dropped_row_says_so_and_never_shows_a_live_value', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ dropped: true, superseded: true })] }));
  // even a live entry (a host without the flag's rule) never turns it into "changed by you"
  p.receive({ type: 'live', values: { 1: { present: true, value: '4' } } });
  const l = p.list();
  assert.match(l, /class="row dropped"/);
  assert.match(l, /<span class="chg">dropped by a preset switch<\/span>/);
  assert.match(l, /<span class="v">5<\/span>/, 'the value it set, not the live one');
  assert.doesNotMatch(l, /changed by you/);
  assert.match(l, /aria-disabled="true" data-fk="u:1" data-tip="Dropped by a preset switch; restore an earlier point instead"/);
  assert.match(css, /\.row\.dropped \.v \{ color: var\(--faint\); \}/);
});

test('p1_core_dropped', () => {
  const r = rowOf({ dropped: true, superseded: true });
  assert.deepEqual({ ...Core.liveDisplay(r, { 1: { present: true, value: '4' } }, true, true) }, { value: '5', note: 'dropped by a preset switch' });
  assert.equal(Core.undoState(r, {}, true, true, true).why, 'Dropped by a preset switch; restore an earlier point instead');
  assert.equal(Core.undoState(r, {}, true, true, false).why, 'Dropped by a preset switch', 'no restore points: no pointer to one');
  // without the flag: unchanged (superseded only)
  assert.equal(Core.undoState(rowOf({ superseded: true }), {}, true, true, true).why, 'Changed again since; restore an earlier point instead');
});

// ---- P2: the host's button defaults, and colours measured on the real backgrounds
test('p2_host_button_defaults_are_undone', () => {
  // WebViewHostDialog::element_defaults_user_script injects, after the page's sheet:
  //   button{color:#fff;background:var(--orca-accent);border:1px solid ...;border-radius:4px;padding:5px 14px} and
  //   button:hover{filter:brightness(1.1)}. The page's reset must outrank `button` (0,0,1) and `button:hover` (0,1,1).
  assert.match(css, /\n  html body button \{ background: none; border: 0; border-radius: 0; padding: 0; margin: 0; color: inherit;/);
  assert.match(css, /\n  html body button:hover \{ filter: none; \}/);
});

// The host's theme values (GUI_App::init_label_colours, WebViewHostDialog host_theme_vars_css): dark window #2b2b2b,
// label #fafafa, muted #b2b3b5; light window = the system window colour (white on Windows), label and muted #363636.
const HOSTS = ['#2b2b2b', '#ffffff', '#f5f5f5', '#ececec', '#e0e0e0', '#3f3f3f', '#1e1e1e', '#808080', '#9a9a9a', '#cfd8dc'];
test('p2_every_colour_reaches_its_ratio_on_the_tinted_backgrounds', () => {
  for (const bg of HOSTS) {
    const t = Core.themeColours(bg);
    assert.ok(t, bg);
    const bgs = Core.backgroundsOf(Core.parseColour(bg));
    assert.equal(bgs.length, 3, 'the background, an open/hovered row and a hovered button');
    for (const [k, v] of Object.entries(t.vars)) {
      const need = ['link', 'faint', 'warn'].includes(k) ? 4.5 : 3;
      for (const b of bgs) {
        const r = Core.contrast(Core.parseColour(v), b);
        assert.ok(r >= need, `${k} ${v} on ${bg} (tinted ${b}): ${r.toFixed(2)} < ${need}`);
      }
    }
  }
});

test('p2_curated_colours_kept_where_they_already_pass', () => {
  assert.equal(Core.themeColours('#2b2b2b').vars.link, '#5ad1c4');
  assert.equal(Core.themeColours('rgb(255, 255, 255)').vars.link, '#00695c');
  assert.equal(Core.themeColours('#2b2b2b').theme, 'dark');
  assert.equal(Core.themeColours('#ffffff').theme, 'light');
  assert.equal(Core.themeColours('rgba(0, 0, 0, 0)'), null, 'transparent: nothing measured, the CSS defaults stay');
  assert.equal(Core.themeColours('nonsense'), null);
});

test('p2_view_sets_the_measured_colours_on_the_root', () => {
  const p = page({ bg: 'rgb(128, 128, 128)' });
  p.receive(historyOf({}));
  assert.equal(p.root.attrs['data-theme'], 'light');
  for (const k of ['--link', '--faint', '--warn', '--focus', '--t-set', '--t-slice']) assert.match(p.props[k] || '', /^#[0-9a-f]{6}$/, k);
  const link = Core.parseColour(p.props['--link']);
  for (const b of Core.backgroundsOf([128, 128, 128, 1])) assert.ok(Core.contrast(link, b) >= 4.5);
});

// ---- P3: long labels wrap to two lines instead of one truncated line
test('p3_long_labels_wrap_to_two_lines', () => {
  const rule = /\n  \.row \.l \.lt \{([^}]*)\}/.exec(css)[1];
  assert.match(rule, /-webkit-line-clamp: 2/);
  assert.doesNotMatch(rule, /nowrap|text-overflow: ellipsis/);
});

// ---- P5: keyboard tags
test('p5_no_focus_tags_in_css_any_more', () => {
  assert.doesNotMatch(css, /:focus-visible[^{]*::after/, 'focus tags are placed by the script, not by ::after');
  assert.match(css, /\n  \[data-tip\]:hover::after \{/, 'hover tags stay');
  assert.match(html, /<div class="ftip" id="ftip" role="tooltip" hidden><\/div>/);
});

test('p5_place_tip_inside_the_view_and_off_the_control', () => {
  const view = { w: 280, h: 500 };
  const cases = [
    { ctrl: { left: 200, top: 300, right: 270, bottom: 322 }, size: { w: 200, h: 20 } }, // a Revert at the right edge
    { ctrl: { left: 10, top: 2, right: 270, bottom: 30 }, size: { w: 240, h: 40 } },     // a group title at the top
    { ctrl: { left: 250, top: 6, right: 272, bottom: 28 }, size: { w: 180, h: 34 } },     // the gear
    { ctrl: { left: 51, top: 470, right: 270, bottom: 496 }, size: { w: 220, h: 50 } },   // a row at the bottom
    { ctrl: { left: 0, top: 100, right: 280, bottom: 126 }, size: { w: 600, h: 18 } },    // a tag wider than the pane
  ];
  for (const c of cases) {
    const p = Core.placeTip(c.ctrl, c.size, view, 6, 4);
    assert.ok(p.left >= 4 && p.left + p.width <= view.w - 4, `inside horizontally: ${JSON.stringify(p)}`);
    assert.ok(p.top >= 4 && p.top + c.size.h <= view.h - 4, `inside vertically: ${JSON.stringify(p)}`);
    const overlaps = p.top < c.ctrl.bottom && p.top + c.size.h > c.ctrl.top;
    assert.ok(!overlaps, `not over the control: ${JSON.stringify(p)}`);
  }
  assert.equal(Core.placeTip(cases[0].ctrl, cases[0].size, view).place, 'above');
  assert.equal(Core.placeTip(cases[1].ctrl, cases[1].size, view).place, 'below');
});

test('p5_tag_follows_keyboard_focus_only', () => {
  const p = page();
  p.receive(historyOf({}));
  const tip = p.els.ftip;
  tip.offsetWidth = 150; tip.offsetHeight = 20;
  const revert = { getAttribute: (k) => (k === 'data-tip' ? 'Set Bottom shell layers back to 4' : null),
                   getBoundingClientRect: () => ({ left: 220, top: 200, right: 272, bottom: 222 }) };
  p.handlers.mousedown();
  p.handlers.focusin({ target: revert });
  assert.equal(tip.hidden, true, 'a click shows no focus tag');
  p.handlers.keydown({ key: 'Tab', target: {}, preventDefault() {} });
  p.handlers.focusin({ target: revert });
  assert.equal(tip.hidden, false);
  assert.equal(tip.textContent, 'Set Bottom shell layers back to 4');
  const left = parseInt(tip.style.left, 10), top = parseInt(tip.style.top, 10);
  assert.ok(left + 150 <= 280 - 4, 'inside the pane on the right');
  assert.ok(top + 20 <= 200, 'above the control, not over it');
  // a row: its label's tag
  const row = { getAttribute: () => null, querySelector: (s) => (s === '.l[data-tip]' ? { getAttribute: () => 'Show what it was and undo it' } : null),
                getBoundingClientRect: () => ({ left: 0, top: 4, right: 280, bottom: 30 }) };
  p.handlers.focusin({ target: row });
  assert.equal(tip.textContent, 'Show what it was and undo it');
  assert.ok(parseInt(tip.style.top, 10) >= 30, 'no room above: below the row');
  p.handlers.focusout();
  assert.equal(tip.hidden, true);
});

// ---- P6 / m14: no "changed by you" between the live and the history update after Revert
test('p6_m14_reverted_row_never_flashes_changed_by_you', () => {
  const p = page();
  p.receive(historyOf({}));
  p.receive({ type: 'live', values: { 1: { present: true, value: '5' } } });
  p.click('data-undo', 1);
  p.receive({ type: 'result', action: 'undo', row: 1, status: 'ok' });
  // the live value (the earlier one) arrives before the history marks the row undone
  p.receive({ type: 'live', values: { 1: { present: true, value: '4' } } });
  const l = p.list();
  assert.doesNotMatch(l, /changed by you/);
  assert.match(l, /<span class="chg"><s class="old">5<\/s> reverted<\/span><\/span><span class="v">4<\/span>/);
  assert.match(l, /aria-label="Bottom shell layers 4, reverted"/);
});

// ---- P6 / m15: after Yes on a restore, the keyboard lands on the Undo line
test('p6_m15_focus_after_restore_lands_on_undo', () => {
  const two = (o) => historyOf({
    rows: [rowOf({ id: 1, group: 1, ts: 100 }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05', label: 'Infill', value: '20%', before: '15%' })],
    groups: [g0, { id: 1, ts: 100, time: '10:00', restore: { available: true, why: '' } }, { id: 2, ts: 200, time: '10:05', restore: null }], ...o });
  const p = page();
  p.receive(two({}));
  p.click('data-ask', 1);
  p.click('data-yes', 1);
  assert.equal(p.focused.at(-1), 'g:1', 'while restoring: the group title');
  // the host held the focus (its restore pumps a dialog) when the result came
  p.setFocus(false);
  p.receive({ type: 'result', action: 'restore', group: 1, status: 'ok' });
  p.setFocus(true);
  // the result came before the history: no Undo line yet, the target waits
  p.receive({ type: 'result', action: 'noop' });
  assert.notEqual(p.focused.at(-1), 'ur');
  p.receive(two({ restored_to: 1, can_undo_restore: true }));
  assert.equal(p.focused.at(-1), 'ur', 'the Undo of the restore');
  // consumed: a later live update does not pull the focus back there
  const n = p.focused.length;
  p.receive({ type: 'live', values: { 1: { present: true, value: '7' } } });
  assert.ok(p.focused.slice(n).every((k) => k !== 'ur'));
});

test('p6_m15_failed_restore_focuses_its_ok', () => {
  const p = page();
  p.receive(historyOf({
    rows: [rowOf({ id: 1, group: 1, ts: 100 }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05' })],
    groups: [g0, { id: 1, ts: 100, time: '10:00', restore: { available: true, why: '' } }, { id: 2, ts: 200, time: '10:05', restore: null }] }));
  p.click('data-ask', 1);
  p.click('data-yes', 1);
  p.receive({ type: 'result', action: 'restore', group: 1, status: 'failed', reason: 'The restore point is gone' });
  assert.equal(p.focused.at(-1), 'd:1');
});

test('p6_m15_a_click_drops_a_pending_focus', () => {
  const p = page();
  p.receive(historyOf({}));
  p.receive({ type: 'result', action: 'restore', group: 1, status: 'ok' }); // 'ur' pending, no Undo line yet
  p.handlers.mousedown();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: true }));
  assert.ok(!p.focused.includes('ur'), 'the user clicked elsewhere meanwhile');
});
