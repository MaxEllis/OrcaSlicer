#!/usr/bin/env python3
"""The OrcaSlicer MCP panel's translations (orcaslicer-mcp #21).

    python3 scripts/mcp_panel_i18n.py update   # rewrite i18n/catalog.json and the panel's translations block
    python3 scripts/mcp_panel_i18n.py check    # exit 1 when either is out of date or a language file has a problem

The panel's phrases come from three places:
  resources/web/orcaslicer_mcp/panel.html   t('...') and N_('...') in its scripts (the page's own words)
  src/slic3r/GUI/RemoteAPI/*.cpp|hpp        MCP_T("...") (text the build sends; templates hold {placeholders})
  i18n/external.json                        text other programs send (the orcaslicer-mcp connector's notes)
Each language file i18n/<code>.json maps that English to the language. The page gets them all inline (one file for
the build and the plugin), between the BEGIN/END PANEL I18N lines of its panel-i18n script.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "resources/web/orcaslicer_mcp/panel.html"
I18N = ROOT / "resources/web/orcaslicer_mcp/i18n"
CPP_DIR = ROOT / "src/slic3r/GUI/RemoteAPI"
# OrcaSlicer's own languages (localization/i18n), English aside.
LANGS = ["ca", "cs", "de", "es", "eu", "fr", "hu", "it", "ja", "ko", "lt", "nl", "pl", "pt_BR", "ru", "sv", "th",
         "tr", "uk", "vi", "zh_CN", "zh_TW"]
BEGIN, END = "// BEGIN PANEL I18N\n", "// END PANEL I18N"
PLACEHOLDER = re.compile(r"\{([a-z_]+)\}")


def js_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "u":
                out.append(chr(int(s[i + 2:i + 6], 16)))
                i += 6
                continue
            out.append({"n": "\n", "t": "\t"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def c_unescape(s):
    out, i = bytearray(), 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]+", s[i + 2:])
                out.append(int(m.group(0), 16))
                i += 2 + len(m.group(0))
                continue
            out += {"n": b"\n", "t": b"\t"}.get(n, n.encode())
            i += 2
            continue
        out += c.encode("utf-8")
        i += 1
    return out.decode("utf-8")


def page_scripts(html):
    parts = re.findall(r'<script id="panel-(?:core|view)">([\s\S]*?)</script>', html)
    if len(parts) != 2:
        raise SystemExit("panel.html: panel-core and panel-view scripts not found")
    return parts


def page_phrases(html):
    found = []
    for js in page_scripts(html):
        js = re.sub(r"(?m)^\s*//.*$", "", js)
        for m in re.finditer(r"""(?<![\w.$])(?:t|N_)\(\s*(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)")""", js):
            found.append(js_unescape(m.group(1) if m.group(1) is not None else m.group(2)))
    return found


def cpp_phrases():
    found = []
    for f in sorted(list(CPP_DIR.glob("*.cpp")) + list(CPP_DIR.glob("*.hpp"))):
        text = re.sub(r"(?m)^\s*//.*$", "", f.read_text(encoding="utf-8"))  # comment lines (examples) do not count
        for m in re.finditer(r'MCP_T\(\s*"((?:[^"\\]|\\.)*)"\s*\)', text):
            found.append((c_unescape(m.group(1)), f.name))
    return found


def external_phrases():
    p = I18N / "external.json"
    if not p.exists():
        return []
    return [(e["en"], e.get("from", "external")) for e in json.loads(p.read_text(encoding="utf-8"))["phrases"]]


def catalogue():
    html = PAGE.read_text(encoding="utf-8")
    entries = {}
    def add(en, origin, host):
        e = entries.setdefault(en, {"en": en, "host": False, "from": []})
        e["host"] = e["host"] or host
        if origin not in e["from"]:
            e["from"].append(origin)
    for en in page_phrases(html):
        add(en, "panel.html", False)
    for en, origin in cpp_phrases():
        add(en, origin, True)
    for en, origin in external_phrases():
        add(en, origin, True)
    return [entries[k] for k in sorted(entries)]


def catalogue_json(cat):
    return json.dumps({"note": "Every phrase the OrcaSlicer MCP panel shows, in English. Made by "
                               "scripts/mcp_panel_i18n.py update: do not edit. {name} parts are filled in when shown "
                               "and must stay in a translation as they are.",
                       "phrases": cat}, ensure_ascii=False, indent=1) + "\n"


def load_lang(code):
    p = I18N / f"{code}.json"
    if not p.exists():
        return None
    return json.loads(p.read_text(encoding="utf-8"))


def lang_problems(code, data, cat):
    if data is None:
        return [f"{code}: no i18n/{code}.json"]
    phrases = data.get("phrases", {})
    known = {e["en"] for e in cat}
    out = []
    for e in cat:
        tr = phrases.get(e["en"])
        if not isinstance(tr, str) or not tr.strip():
            out.append(f"{code}: missing {e['en']!r}")
        elif set(PLACEHOLDER.findall(tr)) != set(PLACEHOLDER.findall(e["en"])):
            out.append(f"{code}: placeholders differ in {e['en']!r} -> {tr!r}")
    for en in phrases:
        if en not in known:
            out.append(f"{code}: not in the catalogue {en!r}")
    return out


def inline_block(cat):
    # The English once ("keys"), then per language its translations in the same order ("" = none): half the size of
    # an object per language, which matters to the plugin that carries the page in one file.
    keys = [e["en"] for e in cat]
    host = sorted(e["en"] for e in cat if e["host"] and PLACEHOLDER.search(e["en"]))
    lines = ["window.McpPanelI18n = {\"keys\": " + json.dumps(keys, ensure_ascii=False, separators=(",", ":")) + ",",
             "\"host\": " + json.dumps(host, ensure_ascii=False, separators=(",", ":")) + ",", "\"langs\": {"]
    codes = [c for c in LANGS if load_lang(c) is not None]
    for i, code in enumerate(codes):
        phrases = load_lang(code).get("phrases", {})
        row = [phrases[k] if isinstance(phrases.get(k), str) and phrases[k].strip() else "" for k in keys]
        lines.append(json.dumps(code) + ": " + json.dumps(row, ensure_ascii=False, separators=(",", ":")) +
                     ("," if i + 1 < len(codes) else ""))
    lines.append("}};")
    # A phrase holding "</script>" must not end the script element.
    return "\n".join(lines).replace("</", "<\\/") + "\n"


def page_with_block(html, block):
    a, b = html.find(BEGIN), html.find(END)
    if a < 0 or b < a:
        raise SystemExit("panel.html: BEGIN/END PANEL I18N lines not found")
    return html[:a + len(BEGIN)] + block + html[b:]


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "check"
    cat = catalogue()
    html = PAGE.read_text(encoding="utf-8")
    want_cat = catalogue_json(cat)
    want_html = page_with_block(html, inline_block(cat))
    if cmd == "update":
        (I18N / "catalog.json").write_text(want_cat, encoding="utf-8")
        PAGE.write_text(want_html, encoding="utf-8")
        print(f"{len(cat)} phrases ({sum(e['host'] for e in cat)} from the host); catalogue and panel updated")
        problems = [p for c in LANGS for p in lang_problems(c, load_lang(c), cat)]
        print(f"{len(problems)} language problems" + ("" if not problems else ": run check for the list"))
        return 0
    if cmd != "check":
        raise SystemExit(__doc__)
    problems = []
    cat_path = I18N / "catalog.json"
    if not cat_path.exists() or cat_path.read_text(encoding="utf-8") != want_cat:
        problems.append("i18n/catalog.json is out of date: run update")
    if html != want_html:
        problems.append("panel.html's translations block is out of date: run update")
    for code in LANGS:
        problems += lang_problems(code, load_lang(code), cat)
    for p in problems:
        print(p)
    print(f"{len(cat)} phrases, {len(LANGS)} languages, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
