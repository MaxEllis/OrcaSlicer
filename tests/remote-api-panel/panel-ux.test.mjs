// The usability fixes of 2026-11-09 (usability/SYNTHESIS.md): keyboard access (B2), reverted rows (M1), live state per
// setting (M2 / R43), stale slice results (M3), the restore control and question (M7), theme colours (M8), Ctrl+P /
// Ctrl+S (M11), wrapping (M14) and the minors. Same fake DOM as panel-view.test.mjs, plus keydown events.
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

function element() {
  return { innerHTML: '', scrollTop: 0, style: {}, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k]; } };
}

// A fresh page. bg: the body's computed background ("rgb(r, g, b)"), for the theme check.
function page(bg, docExtra) {
  const els = { list: element(), fold: element(), gear: element() };
  const root = element();
  const sent = [];
  const window = {
    orca: { postMessage: (m) => sent.push(JSON.parse(JSON.stringify(m))), onMessage: () => {} },
    setTimeout: () => 0,
    clearTimeout: () => {},
    print: () => { throw new Error('the page printed'); },
  };
  if (bg) window.getComputedStyle = () => ({ backgroundColor: bg });
  const handlers = {};
  const document = { getElementById: (id) => els[id], addEventListener: (type, fn) => { handlers[type] = fn; },
                     body: {}, documentElement: root, ...(docExtra || {}) };
  const ctx = vm.createContext({ window, document });
  vm.runInContext(core, ctx);
  vm.runInContext(view, ctx);
  const rowTarget = (id, extra = {}) => ({
    getAttribute: (k) => (k === 'data-row' ? String(id) : k === 'role' ? 'button' : null),
    hasAttribute: (k) => k === 'data-row' || k === 'role',
    closest: (sel) => (sel === '.row' ? rowTarget(id) : null), ...extra });
  function key(k, target, mods = {}) {
    let prevented = false;
    handlers.keydown({ key: k, target: target || {}, preventDefault() { prevented = true; }, ...mods });
    return prevented;
  }
  function click(attr, value) {
    const el = { getAttribute: (k) => (k === attr ? String(value) : null), hasAttribute: (k) => k === attr, parentNode: null };
    handlers.click({ target: { ...el, closest: () => null }, stopPropagation() {} });
  }
  function clickTarget(target) { handlers.click({ target, stopPropagation() {} }); }
  function clickRow(id) { handlers.click({ target: { getAttribute: () => null, hasAttribute: () => false, closest: (sel) => (sel === '.row' ? rowTarget(id) : null) }, stopPropagation() {} }); }
  return { sent, window, root, receive: (m) => window.__mcpPanelReceive(m), list: () => els.list.innerHTML, key, rowTarget, click, clickRow, clickTarget };
}

const rowOf = (o) => ({ id: 1, group: 1, ts: 100, time: '10:00', kind: 'setting', label: 'Wall loops', value: '3', before: '2',
  where: '', undo: { verb: 'Revert' }, undone_at: '', title_hint: null, result: null, error: '', on_disk: false, ...o });
const g0 = { id: 0, ts: 50, time: '09:50', restore: { available: true, why: '' } };
const historyOf = (o) => ({ type: 'history', version: 1, rows: [rowOf({})], groups: [g0, { id: 1, ts: 100, time: '10:00', restore: null }],
  capabilities: { restore: true, open_settings: true, settings_hint: '', tracked_live: true }, restored_to: null, can_undo_restore: false, ...o });
// Two groups: wall loops (group 1, older) and infill (group 2, newest).
const twoGroups = (rows, caps) => historyOf({
  rows: rows || [rowOf({ id: 1, group: 1, ts: 100 }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05', label: 'Infill', value: '20%', before: '15%' })],
  groups: [g0, { id: 1, ts: 100, time: '10:00', restore: { available: true, why: '' } }, { id: 2, ts: 200, time: '10:05', restore: null }],
  capabilities: { restore: true, open_settings: true, settings_hint: '', tracked_live: true, ...(caps || {}) } });

// ---- B2: keyboard access
test('controls_are_buttons_or_focusable_rows', () => {
  const p = page();
  p.receive(twoGroups());
  const l = p.list();
  assert.match(l, /<div class="row" data-row="1" data-fk="r:1" role="button" tabindex="0" aria-expanded="false" aria-label="Wall loops 3"/);
  assert.match(l, /<button type="button" class="gt" data-toggle="2" data-fk="g:2" aria-expanded="true"/);
  assert.match(l, /<button type="button" class="gt" data-toggle="1" data-fk="g:1" aria-expanded="false"/);
  assert.match(l, /<button type="button" class="lnk tip-left" data-undo="2" data-fk="u:2"/);
  assert.match(l, /<button type="button" class="act tip-left" data-ask="1" data-fk="a:1"/);
  assert.match(html, /<button type="button" class="ibtn tip-left" id="fold"><\/button><button type="button" class="ibtn tip-left" id="gear"><\/button>/);
  assert.doesNotMatch(l, /<a[ >]/, 'no links without an href: every action is a button');
});

test('focus_ring_from_theme_tokens', () => {
  assert.match(css, /:focus-visible \{ outline: 2px solid var\(--focus\)/);
  assert.match(css, /--focus: #5ad1c4/);
  assert.match(css, /\[data-theme="light"\] \{[^}]*--focus: #00695c/);
});

test('enter_and_space_open_a_row', () => {
  const p = page();
  p.receive(twoGroups());
  assert.equal(p.key('Enter', p.rowTarget(2)), true);
  assert.match(p.list(), /class="row open" data-row="2"[^>]*aria-expanded="true"/);
  assert.equal(p.key(' ', p.rowTarget(2)), true);
  assert.match(p.list(), /class="row" data-row="2"/);
});

test('esc_closes_the_open_row', () => {
  const p = page();
  p.receive(twoGroups());
  p.key('Enter', p.rowTarget(2));
  assert.equal(p.key('Escape', p.rowTarget(2)), true);
  assert.match(p.list(), /class="row" data-row="2"/);
  // From the row's undo link (in its detail line) too.
  p.key('Enter', p.rowTarget(2));
  const undoLink = { getAttribute: (k) => (k === 'data-undo' ? '2' : k === 'data-fk' ? 'u:2' : null), hasAttribute: () => false,
                     closest: (sel) => (sel === '.more' ? {} : null) };
  assert.equal(p.key('Escape', undoLink), true);
  assert.match(p.list(), /class="row" data-row="2"/);
});

test('esc_answers_no_to_the_restore_question', () => {
  const p = page();
  p.receive(twoGroups());
  p.click('data-ask', 1);
  assert.match(p.list(), /group older asking/);
  assert.match(p.list(), /<button type="button" class="lnk" data-yes="1" data-fk="y:1">Yes<\/button><button type="button" class="lnk no" data-no="1" data-fk="n:1">No<\/button>/);
  const n = p.sent.length;
  assert.equal(p.key('Escape', {}), true);
  assert.doesNotMatch(p.list(), /asking/);
  assert.equal(p.sent.length, n, 'Esc restores nothing');
});

test('ctrl_p_and_ctrl_s_do_nothing', () => {
  const p = page();
  assert.equal(p.key('p', {}, { ctrlKey: true }), true);
  assert.equal(p.key('s', {}, { ctrlKey: true }), true);
  assert.equal(p.key('P', {}, { metaKey: true }), true);
  assert.equal(p.key('p', {}), false, 'plain p is typing');
  assert.doesNotThrow(() => p.window.print(), 'window.print is a no-op');
});

// ---- M1: reverted rows
test('reverted_row_shows_the_current_value_struck_through_undone_one', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ undone_at: '10:03' })] }));
  assert.match(p.list(), /<span class="chg"><s class="old">3<\/s> reverted<\/span><\/span><span class="v">2<\/span>/);
  assert.match(p.list(), /class="row reverted"/);
  assert.match(p.list(), />Reverted<\/span>/);
});

test('reverted_here_before_history_catches_up', () => {
  const p = page();
  p.receive(historyOf({}));
  p.click('data-undo', 1);
  p.receive({ type: 'result', action: 'undo', row: 1, status: 'ok' });
  assert.match(p.list(), /<s class="old">3<\/s> reverted<\/span><\/span><span class="v">2<\/span>/);
});

// ---- M2 / R43: live state for the latest row of each setting
test('older_tracked_row_shows_the_users_value', () => {
  const p = page();
  p.receive(twoGroups());
  p.receive({ type: 'live', values: { 1: { value: '5', present: true }, 2: { value: '20%', present: true } } });
  assert.match(p.list(), /data-row="1"[^>]*><span class="ic"[^]*?<span class="chg">changed by you<\/span><\/span><span class="v">5<\/span>/);
  p.clickRow(1);
  assert.match(p.list(), /data-tip="You changed it since"/);
});

test('superseded_row_says_changed_again_never_by_you', () => {
  const p = page();
  const rows = [rowOf({ id: 1, group: 1, ts: 100, superseded: true }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05', value: '4', before: '3' })];
  p.receive(twoGroups(rows));
  p.receive({ type: 'live', values: { 2: { value: '4', present: true } } }); // no entry for the superseded row
  p.clickRow(1);
  const l = p.list();
  assert.doesNotMatch(l, /changed by you/);
  assert.match(l, /aria-disabled="true" data-fk="u:1" data-tip="Changed again since; restore an earlier point instead"/);
});

test('without_tracked_live_only_the_newest_group_shows_live', () => {
  const p = page();
  p.receive(twoGroups(undefined, { tracked_live: false }));
  p.receive({ type: 'live', values: { 1: { value: '5', present: true } } });
  assert.doesNotMatch(p.list(), /changed by you/);
});

test('core_tracked_and_superseded', () => {
  const C = vm.runInNewContext(core + ';window.McpPanelCore', { window: {} });
  const r = rowOf({ id: 7 });
  assert.equal(C.liveDisplay(r, { 7: { value: '5', present: true } }, false, true).note, 'changed by you');
  assert.equal(C.liveDisplay(r, { 7: { value: '5', present: true } }, false).note, '', 'old rule without tracking');
  assert.equal(C.undoState(r, { 7: { value: '5', present: true } }, false, true).why, 'You changed it since');
  assert.equal(C.undoState(rowOf({ id: 8, superseded: true }), {}, true, true).why, 'Changed again since; restore an earlier point instead');
});

// ---- M3: stale slice results
const sliceRow = (o) => rowOf({ kind: 'slice', label: 'Sliced', value: '26m · 3.8 g', before: '', where: 'plate 1', undo: null,
  result: { seconds: 1560, grams: 3.8 }, ...o });

test('slice_with_a_later_change_is_out_of_date', () => {
  const p = page();
  p.receive(twoGroups([sliceRow({ id: 1, group: 1, ts: 100 }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05' })]));
  const l = p.list();
  assert.match(l, /<span class="res stale" data-tip="Out of date: something changed after this slice">26m · out of date<\/span>/);
  assert.match(l, /class="row stale" data-row="1"/);
  assert.match(l, /<span class="chg">out of date<\/span>/);
});

test('an_older_result_is_faint_without_words', () => {
  const p = page();
  const rows = [sliceRow({ id: 1, group: 1, ts: 100 }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05' }),
                sliceRow({ id: 3, group: 2, ts: 200, time: '10:05', result: { seconds: 1380, grams: 3.6 } })];
  p.receive(twoGroups(rows));
  const l = p.list();
  assert.match(l, /<span class="res stale" data-tip="Out of date: something changed after this slice">26m<\/span>/, 'older: faint, no words');
  assert.match(l, /<span class="res">23m<\/span>/, 'the newest result is current');
  assert.doesNotMatch(l, /out of date<\/span>/);
  // Order is by time, not id: a row with a higher id but an earlier time does not age a slice.
  const q = page();
  q.receive(historyOf({ rows: [sliceRow({ id: 1, ts: 100 }), rowOf({ id: 9, group: 1, ts: 50 })] }));
  assert.doesNotMatch(q.list(), /stale/);
});

test('latest_slice_is_not_out_of_date', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ id: 1 }), sliceRow({ id: 2 })] }));
  assert.doesNotMatch(p.list(), /out of date/);
  assert.match(p.list(), /<span class="res">26m<\/span>/);
});

test('a_revert_after_the_slice_makes_it_out_of_date', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ id: 1 }), sliceRow({ id: 2 })] }));
  p.click('data-undo', 1);
  p.receive({ type: 'result', action: 'undo', row: 1, status: 'ok' });
  assert.match(p.list(), /26m · out of date/);
});

test('core_slice_stale_ignores_refusals_notes_and_slices', () => {
  const C = vm.runInNewContext(core + ';window.McpPanelCore', { window: {} });
  const s = sliceRow({ id: 1 });
  const later = (o) => [s, rowOf({ id: 2, ...o })];
  assert.equal(C.sliceStale(s, later({ kind: 'refused', error: 'x' })), false);
  assert.equal(C.sliceStale(s, later({ kind: 'slice', title_hint: 'note', result: null })), false);
  assert.equal(C.sliceStale(s, later({ kind: 'model' })), true);
  assert.equal(C.sliceStale(s, [s], 1), true, 'staleUpTo covers it');
  assert.equal(C.sliceStale(sliceRow({ id: 1, error: 'failed' }), later({})), false, 'a failed slice has no result to age');
});

// ---- M4 / m12: a connector's note names its group
test('note_names_its_group', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ kind: 'slice', label: 'Compared 3 options', value: 'nothing kept', before: '', undo: null, title_hint: 'note' })] }));
  assert.match(p.list(), /<span class="name">Compared 3 options · nothing kept<\/span>/);
});

// ---- M7: the restore control and its question
test('restore_control_always_there_on_older_groups', () => {
  const p = page();
  p.receive(twoGroups());
  const l = p.list();
  assert.match(l, /data-ask="1" data-fk="a:1" data-tip="Restore the project to how it was at 10:00 \(undoes later changes\)" aria-label="Restore the project to how it was at 10:00 \(undoes later changes\)"/);
  assert.equal((l.match(/class="act /g) || []).length, 1, 'none on the newest group');
  assert.match(css, /\.ghead \.act \{[^}]*opacity: \.8/);
  const actRule = css.match(/\n  \.ghead \.act \{[^}]*\}/)[0];
  assert.match(actRule, /display: grid/, 'not hover-only');
  assert.doesNotMatch(css, /\.ghead:hover \.act \{ display/, 'hover does not reveal it any more');
});

test('question_shows_under_the_title', () => {
  const p = page();
  p.receive(twoGroups());
  p.click('data-ask', 1);
  const l = p.list();
  assert.match(l, /<span class="name">Wall loops → 3<\/span><\/span><\/button>.*<div class="ask" role="group" aria-label="Restore to 10:00\?">/s);
  assert.doesNotMatch(css, /\.group\.asking \.ghead \.nm/, 'the title stays');
});

// ---- M8: colours follow the background
test('theme_follows_the_background', () => {
  const dark = page('rgb(43, 43, 47)');
  dark.receive(historyOf({}));
  assert.equal(dark.root.attrs['data-theme'], 'dark');
  const light = page('rgb(245, 245, 245)');
  light.receive(historyOf({}));
  assert.equal(light.root.attrs['data-theme'], 'light');
  assert.match(css, /\.lnk \{ color: var\(--link\); text-decoration: underline/);
  assert.match(css, /\.lnk\.off \{ color: var\(--faint\); text-decoration: none; cursor: default; \}/);
});

// ---- M14: long values wrap, nothing scrolls sideways
test('long_values_wrap', () => {
  assert.match(css, /\.list \{ flex: 1; overflow-y: auto; overflow-x: hidden; \}/);
  assert.match(css, /\.row \{ display: grid; grid-template-columns: 14px minmax\(4em, 1fr\) fit-content\(60%\);/);
  assert.match(css, /\.row \.v \{[^}]*overflow-wrap: anywhere/);
  assert.match(css, /\.more \.d \{[^}]*overflow-wrap: anywhere/);
});

// ---- minors
test('m3_undo_link_never_reveals', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ reveal: true })], capabilities: { restore: true, open_settings: true, reveal: true, tracked_live: true } }));
  p.click('data-undo', 1);
  assert.equal(p.sent.filter((m) => m.action === 'reveal').length, 0);
  assert.equal(p.sent.filter((m) => m.action === 'undo').length, 1);
});

test('m4_preset_goes_to_the_tooltip', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ where: '0.20mm Standard @Printer (unsaved)', reveal: true })] }));
  p.clickRow(1);
  assert.match(p.list(), /<span class="d" data-tip="0.20mm Standard @Printer \(unsaved\)">Was 2<\/span>/);
  const q = page();
  q.receive(historyOf({ rows: [rowOf({ kind: 'setting', where: 'bracket.stl only' })] }));
  q.clickRow(1);
  assert.match(q.list(), /<span class="d">Was 2 · bracket.stl only<\/span>/, 'other details stay on the line');
});

test('m5_refusals_after_the_changes', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ id: 1, kind: 'refused', label: 'Wall loops', value: 'Invalid value', undo: null, error: 'x' }),
                               rowOf({ id: 2, label: 'Sparse infill density', value: '25%', before: '15%' })] }));
  const l = p.list();
  assert.ok(l.indexOf('data-row="2"') < l.indexOf('data-row="1"'), 'the change first');
  assert.match(l, /<span class="name">Sparse infill density → 25%<\/span>/);
});

test('m6_restore_undone_line_until_the_next_change', () => {
  const p = page();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: true }));
  p.click('data-undo-restore', 1);
  p.receive(historyOf({ restored_to: null, can_undo_restore: false }));
  p.receive({ type: 'result', action: 'undo_restore', status: 'ok' });
  assert.match(p.list(), /<span class="name faint">Restore undone<\/span>/);
  p.receive(historyOf({ rows: [rowOf({ id: 1 }), rowOf({ id: 2, label: 'Infill' })] }));
  assert.doesNotMatch(p.list(), /Restore undone/);
});

// ---- fix round 1
test('superseded_without_restore_points_does_not_send_the_user_to_one', () => {
  const p = page();
  const rows = [rowOf({ id: 1, group: 1, ts: 100, superseded: true }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05', value: '4', before: '3' })];
  p.receive(twoGroups(rows, { restore: false }));
  p.clickRow(1);
  assert.match(p.list(), /aria-disabled="true" data-fk="u:1" data-tip="Changed again since"/);
  assert.doesNotMatch(p.list(), /restore an earlier point/);
  const C = vm.runInNewContext(core + ';window.McpPanelCore', { window: {} });
  // the non-tracked older-group branch too
  assert.equal(C.undoState(rowOf({ id: 3 }), { 3: { value: '9', present: true } }, false, false, false).why, 'Changed again since');
  assert.equal(C.undoState(rowOf({ id: 3 }), { 3: { value: '9', present: true } }, false, false).why,
    'Changed again since; restore an earlier point instead', 'default: the host has restore points');
});

test('disabled_restore_icon_does_nothing', () => {
  const p = page();
  const h = twoGroups();
  h.groups[1].restore = { available: false, why: 'No restore point here' };
  p.receive(h);
  assert.match(p.list(), /class="act tip-left off" aria-disabled="true"/);
  const before = p.list();
  const target = { getAttribute: (k) => (k === 'aria-disabled' ? 'true' : null), hasAttribute: (k) => k === 'aria-disabled',
                   parentNode: null, closest: (sel) => (sel === '[data-toggle]' ? { getAttribute: () => '1' } : null) };
  p.window.__click = target;
  p.clickTarget(target);
  assert.equal(p.list(), before, 'the group did not toggle');
});

test('long_values_go_under_the_label', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ value: '0.82 mm · a long value that wraps', before: '0.8 mm' })] }));
  assert.match(p.list(), /class="row long" data-row="1"/);
  assert.match(css, /\.row\.long \.v \{ grid-column: 2; text-align: left; \}/);
});

test('focus_rules_stand_alone', () => {
  assert.match(css, /\n  :focus-visible \{ outline: 2px solid var\(--focus\)/);
  assert.match(css, /:focus:not\(:focus-visible\) \{ outline: none; \}/);
  assert.doesNotMatch(css, /:hover[^{]*,[^{]*:focus-visible|:focus-visible[^{]*,[^{]*:hover/, 'no hover rule shares a list with :focus-visible');
  assert.match(css, /\n  \[data-tip\]:hover::after \{/);
});

test('refocus_only_when_the_page_has_focus', () => {
  for (const focused of [false, true]) {
    let focusCalls = 0;
    const p = page(undefined, { hasFocus: () => focused, activeElement: { getAttribute: (k) => (k === 'data-fk' ? 'r:1' : null) },
                                querySelector: () => ({ focus: () => { focusCalls++; } }) });
    p.receive(historyOf({}));
    p.receive({ type: 'live', values: { 1: { value: '5', present: true } } });
    assert.equal(focusCalls > 0, focused, focused ? 'focus kept while the page has it' : 'no focus taken from OrcaSlicer');
  }
});
