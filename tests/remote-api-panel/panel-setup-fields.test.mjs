// The setup view's form routes: an app whose MCP form has separate boxes (the ChatGPT desktop app: Name, Command to
// launch, Arguments) gets one line per box with its own Copy, so nothing pasted whole lands in the wrong box; and the
// line about uv with its link, which every route needs (an OrcaCloud comment, 2026-11).
import { test } from 'node:test';
import assert from 'node:assert';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '../../resources/web/orcaslicer_mcp/panel.html'), 'utf8');
const i18nJs = html.match(/<script id="panel-i18n">([\s\S]*?)<\/script>/)[1];
const core = html.match(/<script id="panel-core">([\s\S]*?)<\/script>/)[1];
const view = html.match(/<script id="panel-view">([\s\S]*?)<\/script>/)[1];

function element() {
  return { innerHTML: '', textContent: '', scrollTop: 0, style: {}, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; } };
}
function page(i18n) {
  const els = { list: element(), fold: element(), gear: element(), ttl: element() };
  const sent = [], copied = [], handlers = {};
  const window = {
    orca: { postMessage: (m) => sent.push(JSON.parse(JSON.stringify(m))), onMessage: () => {} },
    setTimeout: () => 0, clearTimeout: () => {},
  };
  const documentElement = { lang: '', getAttribute: () => null, setAttribute() {}, style: {} };
  const document = { getElementById: (id) => els[id], addEventListener: (type, fn) => { handlers[type] = fn; }, documentElement };
  const navigator = { clipboard: { writeText: (t) => { copied.push(t); return { then: (ok) => ok() }; } } };
  const ctx = vm.createContext({ window, document, navigator });
  if (i18n === 'shipped') vm.runInContext(i18nJs, ctx);
  else if (i18n) window.McpPanelI18n = i18n;
  vm.runInContext(core, ctx);
  vm.runInContext(view, ctx);
  function click(attr, value) {
    const el = { getAttribute: (k) => (k === attr ? String(value) : null), hasAttribute: (k) => k === attr, parentNode: null };
    handlers.click({ target: { ...el, closest: () => null }, stopPropagation() {} });
  }
  const receive = (m) => window.__mcpPanelReceive(m);
  return { sent, copied, click, receive, list: () => els.list.innerHTML };
}

const HINT = "Settings › Plugins › Add › Add MCP server, then choose STDIO and fill in each box below. Works in Codex chats; " +
  "ordinary ChatGPT chats can't use it yet. Needs uv.";
const chatgpt = { label: 'ChatGPT desktop app', hint: HINT, act: '', primary: true, copy: null, open: null, off: false, also: '',
  copy_label: '', fields: [{ label: 'Name', value: 'orcaslicer' }, { label: 'Command to launch', value: 'uvx' },
                           { label: 'Arguments', value: 'orcaslicer-mcp' }] };
const codex = { label: 'Codex in a terminal or editor', hint: 'Run this once.', act: 'Copy command', primary: false,
  copy: 'codex mcp add orcaslicer -- uvx orcaslicer-mcp', open: null, off: false, also: '', copy_label: '' };
const UV = 'https://docs.astral.sh/uv/getting-started/installation/';
const setup = (extra) => ({ type: 'setup', status: null, paused: false,
  apps: [{ id: 'openai', name: 'OpenAI', where: 'OpenAI', icon: '', routes: [chatgpt, codex] }], ...extra });
const empty = (language) => ({ type: 'history', rows: [], groups: [], capabilities: { restore: false, language } });
function open(p, language, extra) {
  p.receive({ type: 'state', api: 'on' });
  p.receive(empty(language));
  p.receive(setup(extra));
}

test('a form route shows one line per box, each value with its own Copy, and no button for the route itself', () => {
  const p = page('shipped');
  open(p, '');
  const h = p.list();
  assert.match(h, /<span>Name<\/span><code>orcaslicer<\/code>/);
  assert.match(h, /<span>Command to launch<\/span><code>uvx<\/code>/);
  assert.match(h, /<span>Arguments<\/span><code>orcaslicer-mcp<\/code>/);
  assert.strictEqual((h.match(/data-scopy="f0\.\d"/g) || []).length, 3);
  assert.doesNotMatch(h, /data-scopy="r0"/);                      // no whole-route Copy to paste into one box
  assert.match(h, /data-scopy="r1"[^>]*>Copy command</);          // the other routes keep their button
  assert.match(h, /aria-label="Copy Command to launch"/);
});

test('Copy on a box copies only that value and marks only that box', () => {
  const p = page('shipped');
  open(p, '');
  p.click('data-scopy', 'f0.1');
  assert.deepStrictEqual(p.copied, ['uvx']);
  p.click('data-scopy', 'f0.2');
  assert.deepStrictEqual(p.copied, ['uvx', 'orcaslicer-mcp']);
  const h = p.list();
  assert.match(h, /data-scopy="f0\.0"/);                          // the Name box still offers Copy
  assert.doesNotMatch(h, /data-scopy="f0\.1"/);
  assert.strictEqual((h.match(/class="sdone">Copied</g) || []).length, 2);
  p.click('data-scopy', 'r1');
  assert.strictEqual(p.copied[2], 'codex mcp add orcaslicer -- uvx orcaslicer-mcp');
});

test('the uv line shows only when the host names where to get uv, and its link goes through the host', () => {
  const none = page('shipped');
  open(none, '');
  assert.doesNotMatch(none.list(), /data-suv/);
  const p = page('shipped');
  open(p, '', { uv: UV });
  assert.match(p.list(), /Every app needs uv, a small free tool that runs the connector\. Install it once\./);
  p.click('data-suv', 1);
  assert.deepStrictEqual(p.sent.filter((m) => m.action === 'open'), [{ action: 'open', url: UV }]);
  assert.match(p.list(), /class="sdone">Opening…</);
});

test("in the user's language the path and the box names are the ChatGPT app's own words; the values never change", () => {
  const p = page('shipped');
  open(p, 'ca_ES', { uv: UV });
  const h = p.list();
  assert.match(h, /Ajustaments › Connectors › Afegeix › Afegeix un servidor MCP/);
  assert.match(h, /<span>Nom<\/span><code>orcaslicer<\/code>/);
  assert.match(h, /<span>Ordre per publicar<\/span><code>uvx<\/code>/);
  assert.match(h, /<span>Paràmetres<\/span><code>orcaslicer-mcp<\/code>/);
  assert.match(h, /data-suv="1"[^>]*>Obtenir uv</);
  const de = page('shipped');
  open(de, 'de_DE');
  assert.match(de.list(), /Einstellungen › Plugins › Hinzufügen › MCP-Server hinzufügen/);
  assert.match(de.list(), /<span>Befehl zum Starten<\/span><code>uvx<\/code>/);
});
