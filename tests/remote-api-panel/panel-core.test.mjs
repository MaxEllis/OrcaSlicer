import { test } from 'node:test';
import assert from 'node:assert'; // loose deepEqual: results come from another vm realm
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '../../resources/web/orcaslicer_mcp/panel.html'), 'utf8');
const m = html.match(/<script id="panel-core">([\s\S]*?)<\/script>/);
assert.ok(m, 'panel.html has #panel-core');
const window = {};
vm.runInNewContext(m[1], { window });
const C = window.McpPanelCore;

let n = 0;
const row = (o) => ({ id: ++n, group: 1, ts: 100, time: '10:00', kind: 'setting', label: 'Wall loops', value: '3',
  before: '2', where: '', undo: { verb: 'set' }, undone_at: null, title_hint: null, result: null, error: null, ...o });
const meta = (id, ts) => ({ id, ts, time: '10:00', restore: null });

test('protocol version', () => assert.equal(C.PROTOCOL_VERSION, 1));

test('groups_newest_first', () => {
  const g = C.groupRows({ rows: [row({ group: 1, ts: 100 }), row({ group: 2, ts: 200 })], groups: [meta(1, 100), meta(2, 200)] });
  assert.deepEqual(g.map((x) => x.id), [2, 1]);
});
test('start_point_is_last', () => {
  const g = C.groupRows({ rows: [row({ group: 1 })], groups: [meta(0, 0), meta(1, 100)] });
  assert.deepEqual(g.map((x) => x.id), [1, 0]);
  assert.equal(C.groupTitle(g[1]), 'Before the assistant started');
});

test('title_single_change', () => {
  const g = C.groupRows({ rows: [row({ kind: 'preset', label: 'Filament 1', value: 'Generic PETG', before: 'Generic PLA' })], groups: [meta(1, 100)] });
  assert.equal(C.groupTitle(g[0]), 'Filament 1 → Generic PETG');
});
test('title_several', () => {
  const g = C.groupRows({ rows: [row({}), row({ label: 'Infill' }), row({ label: 'Speed' })], groups: [meta(1, 100)] });
  assert.equal(C.groupTitle(g[0]), 'Wall loops and 2 more');
});
test('title_added_wins', () => {
  const g = C.groupRows({ rows: [row({}), row({ kind: 'file', label: 'Added', value: 'bracket.stl', before: '', title_hint: 'added' }), row({}), row({})], groups: [meta(1, 100)] });
  assert.equal(C.groupTitle(g[0]), 'Added bracket.stl and 3 more');
});
test('title_added_single', () => {
  const g = C.groupRows({ rows: [row({ kind: 'file', label: 'Added', value: 'bracket.stl', before: '', title_hint: 'added' })], groups: [meta(1, 100)] });
  assert.equal(C.groupTitle(g[0]), 'Added bracket.stl');
});
test('title_opened_wins', () => {
  const g = C.groupRows({ rows: [row({}), row({ label: 'Opened project', value: 'case', before: '', title_hint: 'opened' })], groups: [meta(1, 100)] });
  assert.equal(C.groupTitle(g[0]), 'Opened project case and 1 more');
});
test('title_slice_only', () => {
  const g = C.groupRows({ rows: [row({ kind: 'slice', label: 'Sliced', value: '', before: '', where: 'plate 1', undo: null })], groups: [meta(1, 100)] });
  assert.equal(C.groupTitle(g[0]), 'Sliced plate 1');
});

test('result_formats', () => {
  const r = (s) => C.groupResult(C.groupRows({ rows: [row({ kind: 'slice', undo: null, result: { seconds: s, grams: 5 } })], groups: [meta(1, 100)] })[0]);
  assert.equal(r(6480).text, '1h 48m');
  assert.equal(r(2880).text, '48m');
  assert.equal(r(40).text, '40s');
  assert.equal(r(6480).warn, false);
});
test('result_uses_last_slice_and_empty', () => {
  const g = C.groupRows({ rows: [row({ kind: 'slice', undo: null, result: { seconds: 60, grams: 1 } }), row({ kind: 'slice', undo: null, result: { seconds: 120, grams: 1 } })], groups: [meta(1, 100)] })[0];
  assert.equal(C.groupResult(g).text, '2m');
  const e = C.groupRows({ rows: [row({})], groups: [meta(1, 100)] })[0];
  assert.deepEqual(C.groupResult(e), { text: '', warn: false });
});
test('result_warns_on_refusal', () => {
  const g = C.groupRows({ rows: [row({ kind: 'refused', undo: null, error: 'blocked' })], groups: [meta(1, 100)] })[0];
  assert.equal(C.groupResult(g).warn, true);
});

test('newest_group_shows_live_value', () => {
  const r = row({ id: 900, value: '3' });
  assert.deepEqual(C.liveDisplay(r, { 900: { value: '4', present: true } }, true), { value: '4', note: 'changed by you' });
  assert.deepEqual(C.liveDisplay(r, { 900: { value: '3', present: true } }, true), { value: '3', note: '' });
});
test('older_group_keeps_logged_value', () => {
  const r = row({ id: 901, value: '3' });
  assert.deepEqual(C.liveDisplay(r, { 901: { value: '4', present: true } }, false), { value: '3', note: '' });
});
test('removed_since', () => {
  const r = row({ id: 902, kind: 'model', value: 'bracket.stl' });
  assert.deepEqual(C.liveDisplay(r, { 902: { value: '', present: false } }, true), { value: 'bracket.stl', note: 'removed since' });
});

test('undo_available_when_live_matches', () => {
  const r = row({ id: 910 });
  assert.deepEqual(C.undoState(r, { 910: { value: '3', present: true } }, true), { available: true, why: '' });
  assert.equal(C.undoState(r, {}, false).available, true);
});
test('undo_unavailable_after_user_change', () => {
  const r = row({ id: 911 });
  assert.deepEqual(C.undoState(r, { 911: { value: '5', present: true } }, true), { available: false, why: 'You changed it since' });
});
test('undo_unavailable_older_changed_again', () => {
  const r = row({ id: 912 });
  assert.deepEqual(C.undoState(r, { 912: { value: '5', present: true } }, false), { available: false, why: 'Changed again since; restore an earlier point instead' });
});
test('undo_unavailable_target_gone', () => {
  const r = row({ id: 913, kind: 'model', value: 'bracket.stl', where: 'bracket.stl' });
  assert.deepEqual(C.undoState(r, { 913: { value: '', present: false } }, true), { available: false, why: 'bracket.stl is no longer on the plate' });
});
test('undo_unavailable_when_reverted', () => {
  const r = row({ id: 914, undone_at: '10:42' });
  assert.deepEqual(C.undoState(r, { 914: { value: '3', present: true } }, true), { available: false, why: 'Reverted at 10:42' });
});
test('no_undo_for_slice_rows', () => {
  const r = row({ id: 915, kind: 'slice', undo: null });
  assert.equal(C.undoState(r, {}, true).available, false);
});

test('title_change_plus_slice', () => {
  const g = C.groupRows({ rows: [row({}), row({ kind: 'slice', label: 'Sliced', value: '', before: '', where: 'plate 1', undo: null })], groups: [meta(1, 100)] })[0];
  assert.equal(C.groupTitle(g), 'Wall loops \u2192 3');
});
test('title_three_settings_plus_slice', () => {
  const g = C.groupRows({ rows: [row({}), row({ label: 'Infill' }), row({ label: 'Speed' }), row({ kind: 'slice', where: 'plate 1', undo: null })], groups: [meta(1, 100)] })[0];
  assert.equal(C.groupTitle(g), 'Wall loops and 2 more');
});
test('title_refused_only', () => {
  const g = C.groupRows({ rows: [row({ kind: 'refused', label: 'Filament start G-code', undo: null, error: 'blocked' })], groups: [meta(1, 100)] })[0];
  assert.equal(C.groupTitle(g), 'Filament start G-code not allowed');
});
test('tolerant_numeric_compare', () => {
  const same = (live, logged) => C.undoState(row({ id: 950, value: logged }), { 950: { value: live, present: true } }, true).available;
  assert.equal(same('3.0', '3'), true);
  assert.equal(same('0.2000001', '0.2'), true);
  assert.equal(same('15.0%', '15%'), true);
  assert.equal(same('4', '3'), false);
  assert.equal(same('160 %', '160 mm/s'), false);
  assert.equal(C.liveDisplay(row({ id: 951, value: '3' }), { 951: { value: '3.0', present: true } }, true).note, '');
});

test('colour_values', () => {
  assert.equal(C.colourOf('#F2754E'), '#F2754E');
  assert.equal(C.colourOf('#f2754eff'), '#f2754eff');
  assert.equal(C.colourOf('#abc'), '#abc');
  assert.equal(C.colourOf(' #00FF00 '), '#00FF00');
  assert.equal(C.colourOf('#00FF0'), '');
  assert.equal(C.colourOf('3'), '');
  assert.equal(C.colourOf('#00FF00;background:url(x)'), '');
  assert.equal(C.colourOf(null), '');
});
test('off_text_default_and_supplied', () => {
  assert.equal(C.offText(undefined), 'The Remote API is off.');
  assert.equal(C.offText({}), 'The Remote API is off.');
  assert.equal(C.offText({ off_text: '' }), 'The Remote API is off.');
  assert.equal(C.offText({ off_text: 'Assistant access is off.' }), 'Assistant access is off.');
});
test('same_value_is_exported', () => {
  assert.equal(C.sameValue('3.0', '3'), true);
  assert.equal(C.sameValue('#FF0000', '#ff0000'), false);
});
