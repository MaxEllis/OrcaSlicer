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
  const document = { getElementById: (id) => els[id], addEventListener: () => {} };
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
  return { sent, receive: (m) => window.__mcpPanelReceive(m), list: () => els.list.innerHTML, tick };
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
  assert.match(p.list(), /Restored to 09:50 ·/);
  assert.match(p.list(), /data-undo-restore/);
  assert.match(p.list(), /group undone/);
});

test('failed_restore_line', () => {
  const p = page();
  p.receive(historyOf({ restored_to: 0, can_undo_restore: true, restore_failed: true }));
  assert.match(p.list(), /Couldn&#39;t restore to 09:50 ·/);
  assert.doesNotMatch(p.list(), /Restored to/);
  assert.match(p.list(), /data-undo-restore/);
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
