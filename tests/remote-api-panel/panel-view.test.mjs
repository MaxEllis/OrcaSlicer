// The panel's view script (#panel-view) against a minimal fake DOM: what it renders for host messages and what it
// sends to the host. Timers are fake (run by hand).
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

function element() {
  return { innerHTML: '', scrollTop: 0, style: {}, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; } };
}

// A fresh page: returns {sent, receive, list(), tick(ms)}.
function page() {
  const els = { list: element(), fold: element(), gear: element() };
  const sent = [];
  const timers = new Map();
  let now = 0, nextId = 1;
  const window = {
    orca: { postMessage: (m) => sent.push(JSON.parse(JSON.stringify(m))), onMessage: () => {} },
    setTimeout: (fn, ms) => { const id = nextId++; timers.set(id, { at: now + ms, fn }); return id; },
    clearTimeout: (id) => { timers.delete(id); },
  };
  let onClick = null;
  const document = { getElementById: (id) => els[id], addEventListener: (type, fn) => { if (type === 'click') onClick = fn; } };
  const ctx = vm.createContext({ window, document });
  vm.runInContext(core, ctx);
  vm.runInContext(view, ctx);
  function tick(ms) {
    const end = now + ms;
    for (;;) {
      let due = null;
      for (const [id, t] of timers) if (t.at <= end && (!due || t.at < due[1].at)) due = [id, t];
      if (!due) break;
      timers.delete(due[0]);
      now = due[1].at;
      due[1].fn();
    }
    now = end;
  }
  // A click on a row (anywhere but its detail line), as the page's click handler sees it.
  function clickRow(id) {
    const row = { getAttribute: (k) => (k === 'data-row' ? String(id) : null) };
    const target = { getAttribute: () => null, hasAttribute: () => false, closest: (sel) => (sel === '.row' ? row : null) };
    onClick({ target, stopPropagation() {} });
  }
  return { sent, receive: (m) => window.__mcpPanelReceive(m), list: () => els.list.innerHTML, gearTip: () => els.gear.attrs['data-tip'],
           foldTip: () => els.fold.attrs['data-tip'], clickRow, tick };
}

const readies = (sent) => sent.filter((m) => m.action === 'ready').length;

const rowOf = (o) => ({ id: 1, group: 1, ts: 100, time: '10:00', kind: 'setting', label: 'Wall loops', value: '3', before: '2',
  where: '', undo: { verb: 'Revert' }, undone_at: '', title_hint: null, result: null, error: '', on_disk: false, ...o });
const historyOf = (o) => ({ type: 'history', version: 1, rows: [rowOf({})],
  groups: [{ id: 0, ts: 50, time: '09:50', restore: { available: true, why: '' } }, { id: 1, ts: 100, time: '10:00', restore: null }],
  capabilities: { restore: true, open_settings: true, settings_hint: '' }, restored_to: null, can_undo_restore: false, ...o });

test('ready_and_live_sent_at_load', () => {
  const p = page();
  assert.deepEqual(p.sent, [{ action: 'ready' }, { action: 'live' }]);
});

test('ready_resent_until_history_arrives', () => {
  const p = page();
  p.tick(499);
  assert.equal(readies(p.sent), 1);
  p.tick(1);
  assert.equal(readies(p.sent), 2);
  p.tick(1000);
  assert.equal(readies(p.sent), 4);
  p.receive(historyOf({}));
  const n = readies(p.sent);
  p.tick(10000);
  assert.equal(readies(p.sent), n, 'no ready after the first history');
});

test('ready_resend_stops_after_20_tries', () => {
  const p = page();
  p.tick(60000);
  assert.equal(readies(p.sent), 21, 'the first ready and 20 more');
});

test('state_alone_does_not_stop_ready', () => {
  const p = page();
  p.receive({ type: 'state', api: 'on' });
  p.tick(500);
  assert.equal(readies(p.sent), 2);
});

test('off_text_default', () => {
  const p = page();
  p.receive({ type: 'state', api: 'off' });
  assert.match(p.list(), /The Remote API is off\./);
  p.receive(historyOf({ rows: [], groups: [] }));
  assert.match(p.list(), /The Remote API is off\./);
});

test('off_text_supplied', () => {
  const p = page();
  p.receive(historyOf({ rows: [], groups: [], capabilities: { restore: false, open_settings: true, off_text: 'Assistant access is off.' } }));
  p.receive({ type: 'state', api: 'off' });
  assert.match(p.list(), /Assistant access is off\./);
  assert.doesNotMatch(p.list(), /Remote API is off/);
  assert.match(p.list(), /data-settings/);
});

test('off_text_is_escaped', () => {
  const p = page();
  p.receive(historyOf({ rows: [], groups: [], capabilities: { off_text: '<b>x</b>' } }));
  p.receive({ type: 'state', api: 'off' });
  assert.match(p.list(), /&lt;b&gt;x&lt;\/b&gt;/);
});

test('colour_row_has_swatch', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ label: 'Filament 1 colour', value: '#F2754E', before: '#FFFFFF' })] }));
  assert.match(p.list(), /<span class="v"><span class="sw" style="background:#F2754E"><\/span>#F2754E<\/span>/);
});

test('swatch_follows_live_value', () => {
  const p = page();
  p.receive(historyOf({ rows: [rowOf({ label: 'Filament 1 colour', value: '#F2754E', before: '#FFFFFF' })] }));
  p.receive({ type: 'live', values: { 1: { value: '#00FF00', present: true } } });
  assert.match(p.list(), /background:#00FF00"><\/span>#00FF00/);
});

test('no_swatch_for_other_rows', () => {
  const p = page();
  p.receive(historyOf({}));
  assert.doesNotMatch(p.list(), /class="sw"/);
  p.receive(historyOf({ rows: [rowOf({ kind: 'preset', label: 'Filament 1', value: '#123456' })] }));
  assert.doesNotMatch(p.list(), /class="sw"/);
});

test('restored_line', () => {
  const p = page();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: true }));
  // ' · Undo' right after the text, in the same cell, outside the ellipsised .name
  assert.match(p.list(), /<span class="rl"><span class="name">Restored to 09:50<\/span><span class="sep">·<\/span><a [^>]*data-undo-restore="1"[^>]*>Undo<\/a><\/span><\/div>/);
  assert.match(p.list(), /group undone/);
});

test('failed_restore_line', () => {
  const p = page();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: true, restore_failed: true }));
  assert.match(p.list(), /<span class="rl"><span class="name">Couldn&#39;t restore to 09:50<\/span><span class="sep">·<\/span><a [^>]*data-undo-restore="1"[^>]*>Undo<\/a><\/span><\/div>/);
  assert.doesNotMatch(p.list(), /Restored to/);
  assert.doesNotMatch(p.list(), /group undone/, 'nothing struck: the host marked nothing undone');
});

test('restore_failed_without_restored_to_is_ignored', () => {
  const p = page();
  p.receive(historyOf({ restored_to: null, restore_failed: true }));
  assert.doesNotMatch(p.list(), /restore to|Restored to/);
});

test('repeated_live_messages', () => {
  const p = page();
  p.receive(historyOf({}));
  p.receive({ type: 'live', values: { 1: { value: '3', present: true } } });
  p.receive({ type: 'live', values: { 1: { value: '4', present: true } } });
  assert.match(p.list(), /changed by you/);
});

test('restored_line_without_undo_has_no_dot', () => {
  const p = page();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: false }));
  assert.match(p.list(), /<span class="rl"><span class="name">Restored to 09:50<\/span><\/span>/);
  assert.doesNotMatch(p.list(), /class="sep"/);
});

test('undo_restore_error_sits_in_column_3', () => {
  const p = page();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: true }));
  p.receive({ type: 'result', action: 'undo_restore', status: 'failed', reason: 'The snapshot before the restore is gone' });
  assert.match(p.list(), /Restored to 09:50<\/span><\/span><span class="ask"[^>]*><span class="line"><span class="bad">The snapshot before the restore is gone/);
  assert.doesNotMatch(p.list(), /class="sep"/);
});

test('settings_label_default', () => {
  const p = page();
  p.receive(historyOf({ rows: [], groups: [] }));
  p.receive({ type: 'state', api: 'off' });
  assert.equal(p.gearTip(), 'Remote API settings');
  assert.match(p.list(), /<a data-settings="1">Open the Remote API settings<\/a>/);
});

test('settings_label_supplied', () => {
  const p = page();
  p.receive(historyOf({ rows: [], groups: [], capabilities: { open_settings: true, off_text: 'Assistant access is off.', settings_label: 'Assistant settings' } }));
  p.receive({ type: 'state', api: 'off' });
  assert.equal(p.gearTip(), 'Assistant settings');
  assert.match(p.list(), /Assistant access is off\. <a data-settings="1">Open the assistant settings<\/a>/);
  assert.doesNotMatch(p.list(), /Remote API/);
});

// ---- the newest group stands out; the start point is not listed
const twoGroups = (o) => historyOf({ rows: [rowOf({ id: 1, group: 1, ts: 100 }), rowOf({ id: 2, group: 2, ts: 200, time: '10:05', label: 'Infill' })],
  groups: [{ id: 0, ts: 50, time: '09:50', restore: { available: true, why: '' } }, { id: 1, ts: 100, time: '10:00', restore: { available: true, why: '' } },
    { id: 2, ts: 200, time: '10:05', restore: null }], ...o });

test('newest_group_has_newest_class_older_have_older', () => {
  const p = page();
  p.receive(twoGroups({}));
  assert.match(p.list(), /<div class="group open newest" data-g="2">/);
  assert.match(p.list(), /<div class="group older" data-g="1">/);
  assert.equal((p.list().match(/ newest"/g) || []).length, 1);
});

test('start_point_not_listed', () => {
  const p = page();
  p.receive(twoGroups({}));
  assert.doesNotMatch(p.list(), /data-g="0"/);
  assert.doesNotMatch(p.list(), /Before the assistant started/);
});

test('restored_line_is_not_the_newest_group', () => {
  const p = page();
  p.receive(twoGroups({ restored_to: 1, can_undo_restore: true }));
  assert.match(p.list(), /<div class="group restored">/, 'the restored line keeps its own classes');
  assert.match(p.list(), /<div class="group undone newest" data-g="2">/, 'the first real group is the newest, struck out');
  assert.match(p.list(), /<div class="group older" data-g="1">/);
});

test('restored_to_start_point_names_its_time', () => {
  const p = page();
  p.receive(twoGroups({ restored_to: 0, can_undo_restore: false }));
  assert.match(p.list(), /Restored to 09:50/);
  assert.doesNotMatch(p.list(), /data-g="0"/);
  assert.match(p.list(), /group undone newest" data-g="2"/);
  assert.match(p.list(), /group undone older" data-g="1"/);
});

test('only_the_start_point_shows_the_empty_text', () => {
  const p = page();
  p.receive(historyOf({ rows: [], groups: [{ id: 0, ts: 50, time: '09:50', restore: { available: true, why: '' } }] }));
  assert.match(p.list(), /When an assistant changes something/);
  assert.doesNotMatch(p.list(), /data-g=/);
});

test('fold_ignores_the_start_point', () => {
  const p = page();
  p.receive(historyOf({})); // group 1 (open, the newest) and the start point
  assert.equal(p.foldTip(), 'Collapse all', 'every listed group is open');
});

// ---- reveal: opening a setting row asks the host to show the setting in its sidebar
const revealHistory = (caps, row) => historyOf({ rows: [rowOf(row)],
  capabilities: { restore: true, open_settings: true, settings_hint: '', ...caps } });
const reveals = (sent) => sent.filter((m) => m.action === 'reveal');

test('reveal_sent_when_a_revealable_row_opens', () => {
  const p = page();
  p.receive(revealHistory({ reveal: true }, { reveal: true }));
  p.clickRow(1);
  assert.deepEqual(reveals(p.sent), [{ action: 'reveal', row: 1 }]);
  assert.match(p.list(), /class="row open" data-row="1"/, 'the click still opens the detail line');
});

test('reveal_not_sent_when_the_row_closes', () => {
  const p = page();
  p.receive(revealHistory({ reveal: true }, { reveal: true }));
  p.clickRow(1);
  p.clickRow(1);
  assert.equal(reveals(p.sent).length, 1);
  assert.match(p.list(), /class="row" data-row="1"/);
});

test('reveal_needs_the_capability', () => {
  const p = page();
  p.receive(revealHistory({}, { reveal: true }));
  p.clickRow(1);
  assert.equal(reveals(p.sent).length, 0);
  assert.match(p.list(), /class="row open" data-row="1"/);
  const q = page();
  q.receive(revealHistory({ reveal: false }, { reveal: true }));
  q.clickRow(1);
  assert.equal(reveals(q.sent).length, 0);
});

test('reveal_needs_a_revealable_row', () => {
  const p = page();
  p.receive(revealHistory({ reveal: true }, {}));
  p.clickRow(1);
  assert.equal(reveals(p.sent).length, 0);
  const q = page();
  q.receive(revealHistory({ reveal: true }, { kind: 'model', label: 'Rotated', reveal: false }));
  q.clickRow(1);
  assert.equal(reveals(q.sent).length, 0);
});

test('reveal_hover_tag', () => {
  const p = page();
  p.receive(revealHistory({ reveal: true }, { reveal: true }));
  assert.match(p.list(), /<span class="l" data-tip="Show in settings"><span class="lt">Wall loops<\/span>/);
  p.clickRow(1);
  assert.doesNotMatch(p.list(), /Show in settings/, 'an open row closes on click: no tag');
  const q = page();
  q.receive(revealHistory({}, { reveal: true }));
  assert.doesNotMatch(q.list(), /Show in settings/);
  const r = page();
  r.receive(revealHistory({ reveal: true }, {}));
  assert.doesNotMatch(r.list(), /Show in settings/);
});
