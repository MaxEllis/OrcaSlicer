// The follow-ups after the final real-product check (usability/verify2-report.md, "New things noticed"): the host's
// slice_valid marks the newest slice result out of date after the user's own edits (1), multi-line values carry their
// lines in a hover tag (2), and older groups close when a newer one arrives unless the user opened them (4).
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
  return { innerHTML: '', textContent: '', hidden: true, scrollTop: 0, style: {}, attrs: {},
           setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k]; } };
}

function page() {
  const els = { list: element(), fold: element(), gear: element(), ftip: element() };
  const sent = [];
  const window = {
    orca: { postMessage: (m) => sent.push(JSON.parse(JSON.stringify(m))), onMessage: () => {} },
    setTimeout: () => 0, clearTimeout: () => {},
  };
  const handlers = {};
  const document = { getElementById: (id) => els[id], addEventListener: (type, fn) => { handlers[type] = fn; } };
  const ctx = vm.createContext({ window, document });
  vm.runInContext(core, ctx);
  vm.runInContext(view, ctx);
  const none = { getAttribute: () => null, hasAttribute: () => false, parentNode: null };
  // A click on a group's title (data-toggle) or on the fold button, as the page's click handler sees it.
  function clickGroup(id) {
    const head = { getAttribute: (k) => (k === 'data-toggle' ? String(id) : null) };
    handlers.click({ target: { ...none, closest: (sel) => (sel === '[data-toggle]' ? head : null) }, stopPropagation() {} });
  }
  function clickFold() {
    handlers.click({ target: { ...none, closest: (sel) => (sel === '#fold' ? els.fold : null) }, stopPropagation() {} });
  }
  return { sent, receive: (m) => window.__mcpPanelReceive(m), list: () => els.list.innerHTML, foldTip: () => els.fold.attrs['data-tip'],
           clickGroup, clickFold };
}

const rowOf = (o) => ({ id: 1, group: 1, ts: 100, time: '10:00', kind: 'setting', label: 'Wall loops', value: '3', before: '2',
  where: '', undo: { verb: 'Revert' }, undone_at: '', title_hint: null, result: null, error: '', on_disk: false, ...o });
const g0 = { id: 0, ts: 50, time: '09:50', restore: { available: true, why: '' } };
const caps = { restore: true, open_settings: true, settings_hint: '', tracked_live: true };
const historyOf = (o) => ({ type: 'history', version: 1, rows: [rowOf({})], groups: [g0, { id: 1, ts: 100, time: '10:00', restore: null }],
  capabilities: caps, restored_to: null, can_undo_restore: false, ...o });

// ---- 1: the host's slice_valid
const sliceRow = (o) => rowOf({ id: 2, kind: 'slice', label: 'Sliced', value: '19m · 2.8 g', before: '', where: 'plate 1', undo: null,
  result: { seconds: 1140, grams: 2.8 }, ...o });
const sliced = (o) => historyOf({ rows: [rowOf({}), sliceRow({})], ...o });

test('core_slice_valid_false_marks_only_the_newest_result', () => {
  const a = sliceRow({ id: 2, ts: 100 }), b = sliceRow({ id: 4, ts: 200 });
  const rows = [a, b];
  assert.equal(Core.sliceStale(b, rows, null, false), true);
  assert.equal(Core.sliceStale(a, rows, null, false), false, 'an older result is judged by the rows alone');
  assert.equal(Core.sliceStale(b, rows, null, true), false);
  assert.equal(Core.sliceStale(b, rows, null), false, 'without slice_valid: as before');
  assert.equal(Core.sliceStale(b, rows, null, null), false);
  // a later change row still marks it, whatever slice_valid says
  assert.equal(Core.sliceStale(b, rows.concat([rowOf({ id: 5, ts: 300 })]), null, true), true);
  // never a failed slice
  assert.equal(Core.sliceStale(sliceRow({ id: 6, ts: 400, error: 'Failed', result: null }), rows, null, false), false);
  const g = { id: 1, rows: [b] };
  assert.equal(Core.groupResult(g, rows, null, false).stale, true);
  assert.equal(Core.groupResult(g, rows, null, true).stale, false);
});

test('slice_valid_false_in_history_says_out_of_date', () => {
  const p = page();
  p.receive(sliced({}));
  assert.doesNotMatch(p.list(), /out of date/, 'no field: the rows decide (nothing changed after the slice)');
  p.receive(sliced({ slice_valid: true }));
  assert.doesNotMatch(p.list(), /out of date/);
  p.receive(sliced({ slice_valid: false }));
  assert.match(p.list(), /<span class="res stale" data-tip="Out of date: something changed after this slice">19m · out of date<\/span>/);
  assert.match(p.list(), /class="row stale/);
  assert.match(p.list(), /<span class="chg">out of date<\/span>/);
});

test('slice_valid_follows_live_messages', () => {
  const p = page();
  p.receive(sliced({ slice_valid: true }));
  assert.doesNotMatch(p.list(), /out of date/);
  // a hand edit: no row, the host's live message says the slice no longer holds
  p.receive({ type: 'live', values: {}, slice_valid: false });
  assert.match(p.list(), /19m · out of date/);
  // a live message without the field keeps it
  p.receive({ type: 'live', values: { 1: { value: '3', present: true } } });
  assert.match(p.list(), /19m · out of date/);
  // sliced again by hand: current again
  p.receive({ type: 'live', values: { 1: { value: '3', present: true } }, slice_valid: true });
  assert.doesNotMatch(p.list(), /out of date/);
  // a history without the field: back to the rows alone
  p.receive({ type: 'live', values: {}, slice_valid: false });
  p.receive(sliced({}));
  assert.doesNotMatch(p.list(), /out of date/);
});

// ---- 2: multi-line values
const gcodeRow = (o) => rowOf({ label: 'Start G-code', value: 'line 3: M104 S210 (14 lines)', before: 'line 3: M104 S200 (14 lines)',
  value_tip: 'Lines 3 to 7 of 14:\nM104 S210\nG92 E0\nG1 Z5\nM83\nG1 E5\n…',
  before_tip: 'Lines 3 to 7 of 14:\nM104 S200\nG92 E0\nG1 Z5\nM83\nG1 E5\n…', ...o });

test('multiline_value_carries_its_lines_in_a_tag', () => {
  const p = page();
  p.receive(historyOf({ rows: [gcodeRow({})] }));
  const l = p.list();
  assert.match(l, /<span class="v" data-tip="Lines 3 to 7 of 14:\nM104 S210\nG92 E0\nG1 Z5\nM83\nG1 E5\n…">line 3: M104 S210 \(14 lines\)<\/span>/);
  assert.match(l, /<span class="d" data-tip="Lines 3 to 7 of 14:\nM104 S200[^"]*">Was line 3: M104 S200 \(14 lines\)<\/span>/);
  assert.doesNotMatch(l, /\\n/, 'never a literal backslash-n');
  // the tags keep their line breaks
  assert.match(css, /\.row \.v\[data-tip\]:hover::after \{[^}]*white-space: pre-line;/);
  assert.match(css, /\.more \.d\[data-tip\]:hover::after \{[^}]*white-space: pre-line;/);
  assert.match(css, /\.ftip \{[^}]*white-space: pre-line;/);
});

test('multiline_tag_follows_what_the_row_shows', () => {
  const p = page();
  p.receive(historyOf({ rows: [gcodeRow({})] }));
  // set back by the user: the row shows the before, with its lines
  p.receive({ type: 'live', values: { 1: { value: 'line 3: M104 S200 (14 lines)', present: true } } });
  assert.match(p.list(), /<span class="v" data-tip="Lines 3 to 7 of 14:\nM104 S200/);
  // changed otherwise: no tag of the assistant's value
  p.receive({ type: 'live', values: { 1: { value: 'changed since', present: true } } });
  assert.match(p.list(), /<span class="v">changed since<\/span>/);
  // reverted: the before is shown, with its lines
  p.receive(historyOf({ rows: [gcodeRow({ undone_at: '10:05' })] }));
  assert.match(p.list(), /<span class="v" data-tip="Lines 3 to 7 of 14:\nM104 S200[^"]*">line 3: M104 S200 \(14 lines\)<\/span>/);
});

test('change_and_change_back_read_differently', () => {
  const p = page();
  const change = gcodeRow({ id: 1 });
  const back = gcodeRow({ id: 2, ts: 101, value: change.before, before: change.value, value_tip: change.before_tip, before_tip: change.value_tip });
  p.receive(historyOf({ rows: [change, back] }));
  const l = p.list();
  assert.match(l, /data-row="1"[\s\S]*?<span class="v"[^>]*>line 3: M104 S210 \(14 lines\)<\/span>/);
  assert.match(l, /data-row="2"[\s\S]*?<span class="v"[^>]*>line 3: M104 S200 \(14 lines\)<\/span>/);
});

test('rows_without_tips_have_no_value_tag', () => {
  const p = page();
  p.receive(historyOf({}));
  assert.match(p.list(), /<span class="v">3<\/span>/);
  assert.match(p.list(), /<span class="d">Was 2<\/span>/);
});

// ---- 4: older groups close
const groupOf = (id) => ({ id, ts: 100 * id, time: '10:0' + id, restore: { available: true, why: '' } });
const rowsUpTo = (n) => Array.from({ length: n }, (_, i) => rowOf({ id: i + 1, group: i + 1, ts: 100 * (i + 1), time: '10:0' + (i + 1) }));
const historyUpTo = (n) => historyOf({ rows: rowsUpTo(n), groups: [g0].concat(Array.from({ length: n }, (_, i) => groupOf(i + 1))) });
function openState(list) {
  const out = {};
  for (const m of list.matchAll(/<div class="(group[^"]*)" data-g="(\d+)">/g)) out[m[2]] = / open( |$)/.test(m[1]);
  return out;
}

test('a_new_group_closes_the_previous_newest', () => {
  const p = page();
  p.receive(historyUpTo(1));
  assert.deepEqual(openState(p.list()), { 1: true });
  p.receive(historyUpTo(2));
  assert.deepEqual(openState(p.list()), { 2: true, 1: false });
  p.receive(historyUpTo(3));
  assert.deepEqual(openState(p.list()), { 3: true, 2: false, 1: false });
  // the same history again changes nothing
  p.receive(historyUpTo(3));
  assert.deepEqual(openState(p.list()), { 3: true, 2: false, 1: false });
});

test('groups_the_user_opened_stay_open', () => {
  const p = page();
  p.receive(historyUpTo(2));
  p.clickGroup(1); // opened by hand while older
  assert.deepEqual(openState(p.list()), { 2: true, 1: true });
  p.receive(historyUpTo(3));
  assert.deepEqual(openState(p.list()), { 3: true, 2: false, 1: true });
  // closed by hand again: no longer kept open
  p.clickGroup(1);
  p.clickGroup(3);
  p.clickGroup(3); // the newest, closed and opened by hand: kept open when it becomes older
  p.receive(historyUpTo(4));
  assert.deepEqual(openState(p.list()), { 4: true, 3: true, 2: false, 1: false });
});

test('a_newest_group_the_user_closed_stays_closed', () => {
  const p = page();
  p.receive(historyUpTo(1));
  p.clickGroup(1);
  p.receive(historyUpTo(2));
  assert.deepEqual(openState(p.list()), { 2: true, 1: false });
});

test('expand_all_and_collapse_all_still_work', () => {
  const p = page();
  p.receive(historyUpTo(3));
  assert.equal(p.foldTip(), 'Expand all');
  p.clickFold();
  assert.deepEqual(openState(p.list()), { 3: true, 2: true, 1: true });
  assert.equal(p.foldTip(), 'Collapse all');
  // after Expand all a new group keeps them open (the user asked to see them)
  p.receive(historyUpTo(4));
  assert.deepEqual(openState(p.list()), { 4: true, 3: true, 2: true, 1: true });
  p.clickFold();
  assert.deepEqual(openState(p.list()), { 4: false, 3: false, 2: false, 1: false });
  assert.equal(p.foldTip(), 'Expand all');
  // after Collapse all the rule is back: the new one open, the others closed
  p.receive(historyUpTo(5));
  assert.deepEqual(openState(p.list()), { 5: true, 4: false, 3: false, 2: false, 1: false });
});
