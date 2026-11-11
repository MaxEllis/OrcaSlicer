// The panel's translations (orcaslicer-mcp #21): the language pick, page and host phrases, and the shipped tables
// (every catalogue phrase in every language, placeholders kept, the inline block in step with the files).
import { test } from 'node:test';
import assert from 'node:assert';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '../..');
const html = fs.readFileSync(path.join(root, 'resources/web/orcaslicer_mcp/panel.html'), 'utf8');
const i18nJs = html.match(/<script id="panel-i18n">([\s\S]*?)<\/script>/)[1];
const core = html.match(/<script id="panel-core">([\s\S]*?)<\/script>/)[1];
const view = html.match(/<script id="panel-view">([\s\S]*?)<\/script>/)[1];

// A made-up language "xx" (marked words, so a test sees which ones went through) and a second one for the pick.
const FAKE = {
  host: ['Set {label} back to {before}', 'Plate {n}', 'Put {name} back on {plate}', 'Reverted at {time}', 'Sliced {where}',
         'plate {n}', '{label} (filament {n})'],
  langs: {
    xx: {
      'Expand all': '[Expand all]', 'Collapse all': '[Collapse all]', 'Assistant': '[Assistant]', 'Revert': '[Revert]',
      'Set {label} back to {before}': '[Set {label} to {before} again]', 'Plate {n}': '[Plate #{n}]',
      'Put {name} back on {plate}': '[Put {name} on {plate}]', 'Sliced {where}': '[Sliced {where}]', 'plate {n}': '[plate #{n}]',
      '{label} (filament {n})': '[{label} / filament {n}]', 'Reverted at {time}': '[Reverted {time}]', 'Sliced': '[Sliced]',
      'Show what it was and undo it': '[Show and undo]', 'Was {before}': '[Was {before}]', 'Setting': '[Setting]',
      'When an assistant changes something, it shows up here.': '[Empty]', 'On': '[On]', 'Not allowed': '[Not allowed]',
      '{title} and {n} more': '[{title} +{n}]', 'changed by you': '[changed by you]', 'Remote API settings': '[Remote API settings]'
    },
    pt_BR: { 'Expand all': 'Expandir tudo' },
    zh_TW: { 'Expand all': '全部展開' }
  }
};

function element() {
  return { innerHTML: '', textContent: '', scrollTop: 0, style: {}, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; } };
}
function load(i18n) {
  const els = { list: element(), fold: element(), gear: element(), ttl: element() };
  const sent = [];
  const window = {
    orca: { postMessage: (m) => sent.push(JSON.parse(JSON.stringify(m))), onMessage: () => {} },
    setTimeout: () => 0, clearTimeout: () => {},
  };
  const documentElement = { lang: '', getAttribute: () => null, setAttribute() {}, style: {} };
  const document = { getElementById: (id) => els[id], addEventListener: () => {}, documentElement };
  const ctx = vm.createContext({ window, document });
  if (i18n === 'shipped') vm.runInContext(i18nJs, ctx);
  else if (i18n) window.McpPanelI18n = i18n;
  vm.runInContext(core, ctx);
  vm.runInContext(view, ctx);
  return { window, els, Core: window.McpPanelCore, documentElement, receive: (m) => window.__mcpPanelReceive(m) };
}

const row = (o) => ({ id: 1, group: 1, ts: 100, time: '10:00', kind: 'setting', label: 'Wall loops', value: '3', before: '2',
  where: '', undo: { verb: 'Revert', tip: 'Set Wall loops back to 2' }, undone_at: '', title_hint: null, result: null, error: '',
  on_disk: false, ...o });
const history = (rows, language) => ({ type: 'history', rows, groups: [{ id: 1, ts: 100, time: '10:00', restore: null }],
  capabilities: { restore: false, language } });

test('the language pick: the code, its language, a table of that language, else English', () => {
  const { Core } = load(FAKE);
  assert.strictEqual(Core.setLanguage('xx_YY'), 'xx');
  assert.strictEqual(Core.setLanguage('xx'), 'xx');
  assert.strictEqual(Core.setLanguage('pt_PT'), 'pt_BR');
  assert.strictEqual(Core.setLanguage('pt-BR'), 'pt_BR');
  assert.strictEqual(Core.setLanguage('zh_TW'), 'zh_TW');
  assert.strictEqual(Core.setLanguage('zh_HK'), 'zh_TW');
  assert.strictEqual(Core.setLanguage('en_US'), '');
  assert.strictEqual(Core.setLanguage(''), '');
  assert.strictEqual(Core.setLanguage(undefined), '');
  assert.strictEqual(Core.t('Expand all'), 'Expand all');
});

test('t: a table entry with its placeholders filled, else the English', () => {
  const { Core } = load(FAKE);
  Core.setLanguage('xx');
  assert.strictEqual(Core.t('Expand all'), '[Expand all]');
  assert.strictEqual(Core.t('{title} and {n} more', { title: 'A', n: 2 }), '[A +2]');
  assert.strictEqual(Core.t('Not in the table {x}', { x: 1 }), 'Not in the table 1');
  assert.strictEqual(Core.t('Missing {y}', {}), 'Missing {y}');
});

test('ht: exact phrases, templates, nested parts, and words that are only text', () => {
  const { Core } = load(FAKE);
  Core.setLanguage('xx');
  assert.strictEqual(Core.ht('Sliced'), '[Sliced]');
  assert.strictEqual(Core.ht('Plate 3'), '[Plate #3]');
  assert.strictEqual(Core.ht('Put Cube back on Plate 2'), '[Put Cube on [Plate #2]]');
  assert.strictEqual(Core.ht('Reverted at 10:31'), '[Reverted 10:31]');
  // A number placeholder holds digits only: an object called "Plate 2 holder" stays as it is.
  assert.strictEqual(Core.ht('Plate 2 holder'), 'Plate 2 holder');
  assert.strictEqual(Core.ht('Cube'), 'Cube');
  assert.strictEqual(Core.ht(''), '');
  assert.strictEqual(Core.ht(null), null);
  // English: untouched.
  Core.setLanguage('en_US');
  assert.strictEqual(Core.ht('Plate 3'), 'Plate 3');
});

test('a row\'s local words (the host\'s own translation of setting names and choices) win, also inside templates', () => {
  const { Core } = load(FAKE);
  Core.setLanguage('xx');
  const r = row({ label: 'Wall loops (filament 2)', local: { 'Wall loops': 'Bucles', 'Classic': 'Clàssic' } });
  assert.strictEqual(Core.rowLabel(r), '[Bucles / filament 2]');
  assert.strictEqual(Core.valueText(r, 'Classic'), 'Clàssic');
  assert.strictEqual(Core.ht('Set Wall loops back to Classic', r), '[Set Bucles to Clàssic again]');
  assert.strictEqual(Core.rowLabel(row({ local: { 'Wall loops': 'Bucles' } })), 'Bucles');
  // English ignores them.
  Core.setLanguage('');
  assert.strictEqual(Core.rowLabel(row({ local: { 'Wall loops': 'Bucles' } })), 'Wall loops');
});

test('the drawn panel follows capabilities.language, and English is unchanged by it', () => {
  const en = load(FAKE);
  en.receive(history([row({})]));
  const enList = en.els.list.innerHTML;
  const enUs = load(FAKE);
  enUs.receive(history([row({})], 'en_US'));
  assert.strictEqual(enUs.els.list.innerHTML, enList);
  assert.strictEqual(enUs.els.fold.attrs['data-tip'], en.els.fold.attrs['data-tip']);

  const xx = load(FAKE);
  xx.receive(history([row({ local: { 'Wall loops': 'Bucles' } })], 'xx_YY'));
  const list = xx.els.list.innerHTML;
  assert.match(list, />\[Revert\]</);
  assert.match(list, /data-tip="\[Set Bucles to 2 again\]"/);
  assert.match(list, /<span class="lt">Bucles<\/span>/);
  assert.match(list, /\[Was 2\]/);
  assert.match(list, /data-tip="\[Setting\]"/);
  assert.match(xx.els.fold.attrs['data-tip'], /^\[(Expand|Collapse) all\]$/);
  assert.strictEqual(xx.els.ttl.textContent, '[Assistant]');
  assert.strictEqual(xx.documentElement.lang, 'xx');
  // The row data itself stays English (what the page sends back names rows by id only).
  assert.doesNotMatch(list, /Wall loops/);
});

test('the empty panel and a slice group title in the language', () => {
  const p = load(FAKE);
  p.receive({ type: 'history', rows: [], groups: [], capabilities: { language: 'xx' } });
  assert.match(p.els.list.innerHTML, /\[Empty\]/);
  const s = load(FAKE);
  s.receive(history([row({ kind: 'slice', label: 'Sliced', value: '5m', before: '', where: 'plate 1', undo: null,
                           result: { seconds: 300 } })], 'xx'));
  assert.match(s.els.list.innerHTML, /\[Sliced \[plate #1\]\]/);
});

// ---- The shipped tables ----------------------------------------------------------------------------------------
const I18N_DIR = path.join(root, 'resources/web/orcaslicer_mcp/i18n');
const catalogue = JSON.parse(fs.readFileSync(path.join(I18N_DIR, 'catalog.json'), 'utf8')).phrases;
const LANGS = ['ca', 'cs', 'de', 'es', 'eu', 'fr', 'hu', 'it', 'ja', 'ko', 'lt', 'nl', 'pl', 'pt_BR', 'ru', 'sv', 'th', 'tr',
               'uk', 'vi', 'zh_CN', 'zh_TW'];
const holes = (s) => (String(s).match(/\{[a-z_]+\}/g) || []).sort().join(',');

test('every catalogue phrase is translated in every language, with the same placeholders', () => {
  const problems = [];
  for (const code of LANGS) {
    const file = path.join(I18N_DIR, code + '.json');
    if (!fs.existsSync(file)) { problems.push(code + ': no file'); continue; }
    const phrases = JSON.parse(fs.readFileSync(file, 'utf8')).phrases || {};
    for (const e of catalogue) {
      const tr = phrases[e.en];
      if (typeof tr !== 'string' || !tr.trim()) problems.push(code + ': missing ' + e.en);
      else if (holes(tr) !== holes(e.en)) problems.push(code + ': placeholders in ' + e.en + ' -> ' + tr);
    }
  }
  assert.deepStrictEqual(problems.slice(0, 20), []);
});

test('the inline block holds exactly the language files, and every host template', () => {
  const { window } = load('shipped');
  // The page's objects belong to another context: compared as plain data.
  const inline = JSON.parse(JSON.stringify(window.McpPanelI18n));
  const known = new Set(catalogue.map((e) => e.en));
  assert.deepStrictEqual(inline.keys, catalogue.map((e) => e.en));
  const asObject = (row) => Object.fromEntries(inline.keys.map((k, i) => [k, row[i]]).filter(([, v]) => v));
  for (const code of LANGS) {
    const file = path.join(I18N_DIR, code + '.json');
    if (!fs.existsSync(file)) continue;
    const want = Object.fromEntries(Object.entries(JSON.parse(fs.readFileSync(file, 'utf8')).phrases || {})
      .filter(([k, v]) => known.has(k) && typeof v === 'string' && v.trim()));
    assert.deepStrictEqual(asObject(inline.langs[code]), want, code);
  }
  const hostTemplates = catalogue.filter((e) => e.host && /\{[a-z_]+\}/.test(e.en)).map((e) => e.en).sort();
  assert.deepStrictEqual([...inline.host].sort(), hostTemplates);
});

test('every shipped language draws the panel without leaving a placeholder unfilled', () => {
  for (const code of LANGS) {
    const p = load('shipped');
    if (!p.window.McpPanelI18n.langs[code]) continue;
    assert.strictEqual(p.Core.setLanguage(code), code);
    assert.notStrictEqual(p.Core.t('Expand all'), 'Expand all', code);
    p.receive(history([row({}), row({ id: 2, kind: 'slice', label: 'Sliced', value: '5m', before: '', where: 'plate 1', undo: null,
                                      result: { seconds: 300 } })], code));
    assert.doesNotMatch(p.els.list.innerHTML, /\{[a-z_]+\}/, code);
  }
});

test('a template that is nearly all placeholders never rewrites a user\'s own name', () => {
  const { Core } = load({ host: ['{value} in {preset}', '{name} → {plate}', 'Plate {n}', '{name} is gone'],
    langs: { xx: { '{value} in {preset}': '{value} dins {preset}', '{name} → {plate}': '{name} >> {plate}',
                   'Plate {n}': 'Placa {n}', '{name} is gone': '{name} ja no hi és' } } });
  Core.setLanguage('xx');
  assert.strictEqual(Core.ht('Box in a box'), 'Box in a box');
  assert.strictEqual(Core.ht('Cube → Plate 2'), 'Cube → Plate 2');
  assert.strictEqual(Core.ht('Plate 2'), 'Placa 2');
  assert.strictEqual(Core.ht('bracket.3mf is gone'), 'bracket.3mf ja no hi és');
});

test('a connector refusal listing several settings draws each name in the host\'s words; a plain comma name stays', () => {
  const { Core } = load(FAKE);
  Core.setLanguage('xx');
  const r = row({ kind: 'refused', label: 'Start G-code, End G-code', local: { 'Start G-code': 'Inici', 'End G-code': 'Final' } });
  assert.strictEqual(Core.rowLabel(r), 'Inici, Final');
  assert.strictEqual(Core.rowLabel(row({ kind: 'model', label: 'Box, small' })), 'Box, small');
});
