# /// script
# requires-python = ">=3.11"
# dependencies = ["jsonschema>=4.20"]
# ///
"""Checks the standard's JSON files against its schemas.

    uv run tools/validate/validate.py

- every schema in docs/schemas is a valid JSON Schema (draft 2020-12);
- every example in docs/schemas/examples validates, and every entry of invalid.json is refused;
- every layout in engine/layouts validates against the layout schema, and the stock eleven are there;
- what each layout types on the 48 keys: the ASCII symbols it has no key for (design §6.4);
- the settings example only names layouts, profiles and actions that exist;
- every file in docs/cases validates, with its tables and profile lists.

It checks shapes and references. It does not run an engine or a harness: what a case expects is
checked by each harness's own tests.
"""
import json
import pathlib
import sys

from jsonschema import Draft202012Validator

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMAS = ROOT / "docs" / "schemas"
EXAMPLES = SCHEMAS / "examples"
LAYOUTS = ROOT / "engine" / "layouts"
CASES = ROOT / "docs" / "cases"

STOCK = ["en-qwerty", "en-qwerty-us", "en-dvorak", "en-colemak", "ko-2-set", "ko-bada-roman",
         "ja-romaji", "ja-kana", "uni-unicode", "uni-symbols", "uni-emojis"]

R1 = ["Digit%s" % d for d in "1234567890"] + ["Minus", "Equal", "IntlYen"]
R2 = ["Key" + c for c in "QWERTYUIOP"] + ["BracketLeft", "BracketRight"]
R3 = ["Key" + c for c in "ASDFGHJKL"] + ["Semicolon", "Quote", "Backslash"]
R4 = ["Key" + c for c in "ZXCVBNM"] + ["Comma", "Period", "Slash", "IntlRo"]
KEYS = R1 + R2 + R3 + R4
# The JIS table (the standard, §7.2): plain, then ⇧. ⇧0 is empty.
JIS = {"Digit1": "1!", "Digit2": "2\"", "Digit3": "3#", "Digit4": "4$", "Digit5": "5%", "Digit6": "6&",
       "Digit7": "7'", "Digit8": "8(", "Digit9": "9)", "Digit0": "0", "Minus": "-=", "Equal": "^~",
       "IntlYen": "¥|", "BracketLeft": "@`", "BracketRight": "[{", "Semicolon": ";+", "Quote": ":*",
       "Backslash": "]}", "Comma": ",<", "Period": ".>", "Slash": "/?", "IntlRo": "\\_"}
ASCII_SYMBOLS = [chr(i) for i in range(0x21, 0x7F) if not chr(i).isalnum()]
# What each stock layout has no base or ⇧ key for (design §6.4). None: not checked (it types no ASCII).
CANT_TYPE = {"en-qwerty": "", "en-qwerty-us": "", "en-dvorak": "", "en-colemak": "", "ko-2-set": "",
             "ko-bada-roman": "{}", "ja-romaji": None, "ja-kana": None,
             "uni-unicode": "", "uni-symbols": "", "uni-emojis": ""}

failures = []
checked = 0


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def fail(what, why):
    failures.append((what, why))
    print("  FAIL", what, "-", why)


def ok(what):
    global checked
    checked += 1
    print("  ok  ", what)


def errors(validator, value):
    return ["%s: %s" % ("/".join(str(p) for p in e.absolute_path) or "(root)", e.message[:160])
            for e in sorted(validator.iter_errors(value), key=lambda e: list(e.absolute_path))]


def check(validator, value, what):
    errs = errors(validator, value)
    if errs:
        fail(what, "; ".join(errs[:3]))
    else:
        ok(what)
    return not errs


def jis_slot(code, shift):
    if code.startswith("Key"):
        return code[3] if shift else code[3].lower()
    s = JIS[code]
    return s[shift] if shift < len(s) else None


def typed(layout):
    """What the base and ⇧ layers of the 48 keys type, with the JIS table under the layout's own slots."""
    out = {}
    for code in KEYS:
        own = layout["keys"].get(code, {})
        for shift, layer in ((0, "base"), (1, "shift")):
            if layer in own:
                out[(code, layer)] = own[layer]["t"] if own[layer] else None
            else:
                out[(code, layer)] = jis_slot(code, shift)
    return out


print("schemas")
validators = {}
for path in sorted(SCHEMAS.glob("*.schema.json")):
    name = path.name.removesuffix(".schema.json")
    schema = load(path)
    try:
        Draft202012Validator.check_schema(schema)
    except Exception as e:  # noqa: BLE001
        fail(path.name, str(e)[:200])
        continue
    validators[name] = Draft202012Validator(schema)
    ok(path.name)

print("examples")
for name, file, many in [("event", "events.json", True), ("reply", "replies.json", True), ("settings", "settings.json", False),
                         ("layout", "layout.json", False), ("describe", "describe.json", False),
                         ("tide-translator", "tide-translator.json", False)]:
    value = load(EXAMPLES / file)
    for i, item in enumerate(value if many else [value]):
        label = "%s[%d] %s" % (file, i, item.get("type", "")) if many else file
        check(validators[name], item, label.strip())
for i, item in enumerate(load(EXAMPLES / "invalid.json")):
    what = "invalid.json[%d] %s: %s" % (i, item["schema"], item["why"])
    if errors(validators[item["schema"]], item["value"]):
        ok(what + " (refused)")
    else:
        fail(what, "was accepted")

# bada-tide §7: a translator's send templates must not begin like a known dangerous request.
TIDE_DENY_PFU = ["AA AA E%X" % i for i in range(8)] + ["AA AA 86", "AA AA 04", "AA AA 07", "AA AA 08", "AA AA D0"]
TIDE_DENY_VIA = ["05", "07", "0B", "0C", "12", "13"]


def tide_refused(translator):
    """The send template that bada-tide §7 refuses, or None."""
    for msg in ("hello", "state"):
        send = translator[msg]["send"].upper()
        if any(send.startswith(p) for p in TIDE_DENY_PFU):
            return msg
        if translator["match"]["report_size"] == 32 and send.split(" ")[0] in TIDE_DENY_VIA:
            return msg
    return None


print("tide")
tide_example = load(EXAMPLES / "tide-translator.json")
if tide_refused(tide_example):
    fail("tide-translator.json", "a send template is on the deny list")
else:
    ok("tide-translator.json passes the deny list")
for path in sorted((ROOT / "tide").glob("*.json")):
    translator = load(path)
    if check(validators["tide-translator"], translator, "tide/" + path.name):
        if tide_refused(translator):
            fail("tide/" + path.name, "a send template is on the deny list")
denied = json.loads(json.dumps(tide_example))
denied["state"]["send"] = "AA AA E0 00 00"
if tide_refused(denied) == "state":
    ok("a state template starting AA AA E0 is refused")
else:
    fail("deny list", "AA AA E0 was not refused")

print("layouts")
layouts = {}
for path in sorted(LAYOUTS.glob("*.json")):
    layout = load(path)
    if check(validators["layout"], layout, "engine/layouts/" + path.name):
        layouts[layout["id"]] = layout
        if layout["id"] != path.stem:
            fail(path.name, "its id is %s" % layout["id"])
missing = [i for i in STOCK if i not in layouts]
extra = [i for i in layouts if i not in STOCK]
if missing or extra:
    fail("the stock eleven", "missing %s, unexpected %s" % (missing, extra))
else:
    ok("the stock eleven are all there")

print("what each layout can't type on base and ⇧ (ASCII symbols)")
for id_ in STOCK:
    if id_ not in layouts:
        continue
    layout = layouts[id_]
    slots = typed(layout)
    chars = {t for t in slots.values() if t}
    cant = [c for c in ASCII_SYMBOLS if c not in chars]
    empty = ["%s%s" % ("⇧" if layer == "shift" else "", code) for (code, layer), t in slots.items() if t is None]
    other_layers = {s["t"] for k in layout["keys"].values() for layer in ("alt", "alt_shift") if (s := k.get(layer))}
    elsewhere = [c for c in cant if c in other_layers]
    line = "%-14s can't type: %s | empty slots: %s" % (id_, " ".join(cant) or "nothing", " ".join(empty) or "none")
    if elsewhere:
        line += " | on ⌥ or ⌥⇧: " + " ".join(elsewhere)
    expected = CANT_TYPE[id_]
    if expected is None:
        ok(line.replace(" ".join(cant), "%d of 32" % len(cant)) if len(cant) > 8 else line)
    elif sorted(cant) != sorted(expected):
        fail(id_, "can't type %s; the design says %s" % (" ".join(cant) or "nothing", " ".join(expected) or "nothing"))
    else:
        ok(line)

print("references")
actions = {"%s.%s" % (id_, a["id"]) for id_, lay in layouts.items() for a in lay.get("actions", [])}
buffers = set()
for lay in layouts.values():
    for a in lay.get("actions", []):
        buffers.add(a.get("data"))
    buffers.update(lay.get("words", []))
buffers.discard(None)
ok("word lists and lookups the layouts name: " + ", ".join(sorted(buffers)))
for id_, lay in layouts.items():
    declared = set(lay.get("options", {}))
    used = set()
    table = lay.get("table", {}).get("syllable", {})
    if table.get("shortcuts", {}).get("option"):
        used.add(table["shortcuts"]["option"])
    for src in lay.get("search", {}).get("sources", []):
        if src.get("tones"):
            used.add(src["tones"])
    if used - declared:
        fail(id_, "uses options it doesn't declare: %s" % sorted(used - declared))
    for o, decl in lay.get("options", {}).items():
        if decl["type"] == "order" and not set(decl["default"]) <= set(decl["of"]):
            fail(id_, "option %s: its default isn't among its values" % o)
ok("every option a layout uses is declared in it")


def check_settings(settings, what, known_layouts):
    engine = settings.get("engine", {})
    profiles = engine.get("henkan", {}).get("profiles", [])
    ids = [p["id"] for p in profiles]
    problems = []
    if len(ids) != len(set(ids)):
        problems.append("profile ids repeat")
    mu = engine.get("muhenkan", {}).get("layout")
    if mu and (mu not in known_layouts or known_layouts[mu]["kind"] != "muhenkan"):
        problems.append("muhenkan layout %s" % mu)
    for p in profiles:
        problems += ["unknown layout %s" % x for x in p["layouts"] if x not in known_layouts]
        for key, bound in p.get("fkeys", {}).get("keys", {}).items():
            if bound not in ("swallow", "pass") and bound not in actions:
                problems.append("%s is bound to %s" % (key, bound))
    keys = settings.get("keys", {})
    for b in keys.get("bindings", []):
        for a in b["do"] + ([b["when"]] if "when" in b else []):
            if a.startswith("profile:") and a[8:] not in ids:
                problems.append("unknown profile in %s" % a)
            if "." in a and a.split(".")[0] not in ("mode", "profile", "menu", "width") and a not in actions:
                problems.append("unknown action %s" % a)
        pair = b["on"].split("+")
        if len(pair) == 2 and pair[0] == pair[1]:
            problems.append("%s is one key twice" % b["on"])
    problems += ["unknown profile %s in cycle" % c for c in keys.get("cycle", []) if c not in ids]
    he = keys.get("henkan_enters", "last")
    if he != "last" and he not in ids:
        problems.append("henkan_enters %s" % he)
    if problems:
        fail(what, "; ".join(problems))
    else:
        ok(what + ": every layout, profile and action it names exists")


check_settings(load(EXAMPLES / "settings.json"), "settings.json", layouts)

print("cases")
for path in sorted(CASES.glob("*.json")):
    cases = load(path)
    if not check(validators["key-cases"], cases, "docs/cases/" + path.name):
        continue
    for name, table in cases["tables"].items():
        check(validators["settings"], {"format": "bada-settings/2", "keys": table}, "  table %s" % name)
    names = [c["name"] for c in cases["cases"]]
    if len(names) != len(set(names)):
        fail(path.name, "case names repeat")
    bad = [c["name"] for c in cases["cases"] if c["table"] not in cases["tables"] or c["profiles"] not in cases["profiles"]]
    if bad:
        fail(path.name, "cases name a table or profile list that isn't there: %s" % bad[:3])
    else:
        ok("  %d cases, each with its table and profile list" % len(cases["cases"]))

print()
if failures:
    print("%d checks passed, %d FAILED" % (checked, len(failures)))
    sys.exit(1)
print("%d checks passed, none failed" % checked)
