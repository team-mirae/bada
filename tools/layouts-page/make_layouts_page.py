"""Draws bada's stock typing layouts on a JIS keyboard as one static HTML page.
Sources: bada ime's layout files (QWERTY, 2-set, bada roman, romaji, kana); standard Dvorak, Colemak and
US ANSI symbols, written out below."""
import html, json, sys

LAYOUTS = sys.argv[1]          # bada-ime/docs/layouts
OUT = sys.argv[2]

R1 = ["Digit%s" % d for d in "1234567890"] + ["Minus", "Equal", "IntlYen"]
R2 = ["Key" + c for c in "QWERTYUIOP"] + ["BracketLeft", "BracketRight"]
R3 = ["Key" + c for c in "ASDFGHJKL"] + ["Semicolon", "Quote", "Backslash"]
R4 = ["Key" + c for c in "ZXCVBNM"] + ["Comma", "Period", "Slash", "IntlRo"]
KEYS = R1 + R2 + R3 + R4


def load(name):
    d = json.load(open("%s/%s.json" % (LAYOUTS, name), encoding="utf-8"))
    out = {}
    for code, v in d["keys"].items():
        for s, slot in ((0, "base"), (1, "shift")):
            e = (v.get(slot) or [{"t": "", "r": "x"}])[0]
            out[(code, s)] = (e.get("t", ""), e.get("r", "x"))
    return out


JIS = load("00-qwerty")
assert len(JIS) == 96 and JIS[("IntlRo", 0)][0] == "\\" and JIS[("IntlRo", 1)][0] == "_"

SLOT_NAME = {"Minus": "-", "Equal": "^", "IntlYen": "¥", "BracketLeft": "@", "BracketRight": "[",
             "Semicolon": ";", "Quote": ":", "Backslash": "]", "Comma": ",", "Period": ".", "Slash": "/", "IntlRo": "ろ"}


def slot_name(code, s):
    n = SLOT_NAME.get(code) or code.replace("Digit", "").replace("Key", "")
    return ("⇧" if s else "") + n


def whole_keys(mapping):
    """JIS Korean's Dvorak / Colemak: each key types the JIS key named."""
    over = {}
    for code, src in mapping.items():
        for s in (0, 1):
            over[(code, s)] = JIS[(src, s)]
    return over


DVORAK = {"KeyQ": "Slash", "KeyW": "Comma", "KeyE": "Period", "KeyR": "KeyP", "KeyT": "KeyY", "KeyY": "KeyF",
          "KeyU": "KeyG", "KeyI": "KeyC", "KeyO": "KeyR", "KeyP": "KeyL", "KeyS": "KeyO", "KeyD": "KeyE",
          "KeyF": "KeyU", "KeyG": "KeyI", "KeyH": "KeyD", "KeyJ": "KeyH", "KeyK": "KeyT", "KeyL": "KeyN",
          "Semicolon": "KeyS", "KeyZ": "Semicolon", "KeyX": "KeyQ", "KeyC": "KeyJ", "KeyV": "KeyK",
          "KeyB": "KeyX", "KeyN": "KeyB", "Comma": "KeyW", "Period": "KeyV", "Slash": "KeyZ"}
COLEMAK = {"KeyE": "KeyF", "KeyR": "KeyP", "KeyT": "KeyG", "KeyY": "KeyJ", "KeyU": "KeyL", "KeyI": "KeyU",
           "KeyO": "KeyY", "KeyP": "Semicolon", "KeyS": "KeyR", "KeyD": "KeyS", "KeyF": "KeyT", "KeyG": "KeyD",
           "KeyJ": "KeyN", "KeyK": "KeyE", "KeyL": "KeyI", "Semicolon": "KeyO", "KeyN": "KeyK"}


def pairs(over, table):
    for code, (a, b) in table.items():
        over[(code, 0)] = (a, "x")
        over[(code, 1)] = (b, "x")
    return over


dvorak_jis = whole_keys(DVORAK)
dvorak_own = pairs({k: v for k, v in whole_keys(DVORAK).items() if JIS[(DVORAK[k[0]], 0)][1] == "l"},
                   {"KeyQ": "'\"", "KeyW": ",<", "KeyE": ".>", "KeyZ": ";:", "Minus": "[{", "Equal": "]}",
                    "BracketLeft": "/?", "BracketRight": "=+", "Quote": "-_"})
colemak_jis = whole_keys(COLEMAK)
colemak_own = pairs({k: v for k, v in whole_keys(COLEMAK).items() if k[0] != "KeyP"}, {"KeyP": ";:"})

kts = {k: v for k, v in load("10-kts").items() if v != JIS[k]}
kts[("IntlYen", 0)] = ("₩", "x")              # the Korean layouts type ₩ where ¥ is (owner, 2026-11-24)
bada_roman = {k: v for k, v in load("15-bada-roman").items() if v != JIS[k]}
bada_roman[("Semicolon", 0)] = ("sep", "x")      # ; separates syllables (the file's "break")
print("bada roman differs from JIS on:", {slot_name(*k): v[0] for k, v in bada_roman.items()})
# US ANSI symbols by key position; the ` ~ key, which JIS lacks as a typing key, goes on ろ.
qwerty_us = pairs({}, {"Digit2": "2@", "Digit6": "6^", "Digit7": "7&", "Digit8": "8*", "Digit9": "9(", "Digit0": "0)",
                       "Minus": "-_", "Equal": "=+", "BracketLeft": "[{", "BracketRight": "]}", "Backslash": "\\|",
                       "Semicolon": ";:", "Quote": "'\"", "IntlRo": "`~"})
kana = {k: v for k, v in load("20-kana").items() if v != JIS[k]}
romaji = {("Minus", 0): ("ー", "k"), ("Comma", 0): ("、", "k"), ("Period", 0): ("。", "k"),
          ("Slash", 0): ("・", "k"), ("BracketRight", 0): ("「", "k"), ("Backslash", 0): ("」", "k")}

ASCII = [chr(i) for i in range(0x21, 0x7F) if not chr(i).isalpha()]
# JIS kana's ⌥⇧ layer (bada ime 0.3.1, docs/layouts/20-kana.md): a number or symbol key's shifted JIS
# character, full-width.
KANA_ALT = {c: chr(ord(JIS[(c, 1)][0]) + 0xFEE0) for c in KEYS if JIS[(c, 0)][1] == "x"}
assert KANA_ALT["Digit1"] == "！" and KANA_ALT["Digit0"] == "０" and KANA_ALT["IntlRo"] == "＿" and len(KANA_ALT) == 22


def board(over, proposals=None):
    slots = {}
    for k in ((c, s) for c in KEYS for s in (0, 1)):
        t, r = over.get(k, JIS[k])
        slots[k] = {"t": t, "r": r, "st": "jis" if t == JIS[k][0] else "own"}
    own_text = {v["t"] for v in slots.values() if v["st"] == "own"}
    typable = {v["t"] for v in slots.values() if v["t"]}
    for (code, s), v in slots.items():
        if s == 1 and v["t"] == slots[(code, 0)]["t"]:
            v["st"] = "free"
        elif v["st"] == "jis" and v["r"] == "x" and v["t"] in own_text:
            v["st"] = "dup"
    missing = [c for c in ASCII if c not in typable]
    for k, ch in (proposals or {}).items():
        assert slots[k]["st"] in ("free", "dup"), (k, slots[k])
        slots[k] = {"t": ch, "r": "x", "st": "prop", "was": JIS[k][0]}
    if proposals:                               # every character that lost its key has one again
        assert sorted(missing) == sorted(proposals.values()), (missing, proposals)
    free = sum(1 for v in slots.values() if v["st"] == "free")
    return slots, missing, free


E = html.escape
ROLE = {"i": "ri", "v": "rv", "f": "rf", "c": "ri", "k": "rk", "l": "rl", "x": "rx", "a": "ri", "n": "rv"}
TIP = {"free": "Free: it repeats the unshifted key", "dup": "Also typed by one of the layout's own keys",
       "own": "The layout's own", "jis": "JIS legend"}


def key_html(code, slots, alt=None):
    p, s = slots[(code, 0)], slots[(code, 1)]
    cls = "key"
    if p["st"] in ("own", "prop"):
        cls += " own " + ROLE.get(p["r"], "rx")

    def legend(v, which):
        if v["st"] == "free":
            return '<span class="%s free" title="%s">○</span>' % (which, E(TIP["free"]))
        tip = TIP.get(v["st"], "")
        if v["st"] == "prop":
            tip = "Moved here for JIS. The JIS legend on this slot: %s" % (v["was"] or "nothing")
        return '<span class="%s %s" title="%s">%s</span>' % (which, v["st"], E(tip), E(v["t"]))
    al = '<span class="al" title="⌥⇧ + key (Alt+Shift off the Mac)">%s</span>' % E(alt[code]) if alt and code in alt else ""
    return '<div class="%s" style="--w:1">%s%s%s</div>' % (cls, legend(s, "sh"), legend(p, "pl"), al)


def mod(label, w, cls="mod"):
    return '<div class="key %s" style="--w:%s"><span class="ml">%s</span></div>' % (cls, w, E(label))


def keyboard_html(slots, alt=None):
    rows = [
        [mod("半/全", 1, "mod lang")] + [key_html(c, slots, alt) for c in R1] + [mod("⌫", 1)],
        [mod("tab", 1.5)] + [key_html(c, slots, alt) for c in R2] + [mod("↵", 1.5)],
        [mod("ctrl", 1.75)] + [key_html(c, slots, alt) for c in R3] + [mod("", 1.25)],
        [mod("shift", 2.25)] + [key_html(c, slots, alt) for c in R4] + [mod("shift", 1.75)],
        [mod("", 1.5), mod("", 1), mod("", 1.25), mod("無変換 · 英数", 1.5, "mod lang"), mod("", 4.25),
         mod("変換 · かな", 1.5, "mod lang"), mod("カタカナ", 1.25, "mod lang"), mod("", 1), mod("", 1.75)],
    ]
    return '<div class="kb-scroll"><div class="kb" role="img" aria-label="JIS keyboard">%s</div></div>' % "".join(
        '<div class="row">%s</div>' % "".join(r) for r in rows)


def chips(chars, cls):
    return "".join('<span class="chip %s">%s</span>' % (cls, E(c)) for c in chars)


BOARDS = [
    dict(id="en-qwerty", title="English - QWERTY", tag="", over={},
         text="The JIS legends as printed. Every other layout starts from this board and changes only the keys it names."),
    dict(id="en-qwerty-us", title="English - QWERTY (US)", tag="US ANSI symbols", over=qwerty_us,
         text="The symbols a US keyboard has, key for key by position, so they no longer match the JIS legends: ⇧2 is @, the @ key is [, the : key is '. The US ` ~ key has no JIS typing key, so it goes on ろ. ¥ stays ¥, and 半角/全角 stays full-width and half-width."),
    dict(id="en-dvorak", title="English - Dvorak", tag="its own symbol places", over=dvorak_own,
         props={("Backslash", 0): "@", ("Backslash", 1): "`", ("Digit0", 1): "~", ("Digit2", 1): "^", ("Digit7", 1): "*"},
         text="Dvorak's letters and its own pairs: ' \" on Q, ; : on Z, the brackets on the number row, / ? = + and - _ on the right. Five characters had no JIS key left and moved: @ and ` to the ] key, ~ to ⇧0, ^ to ⇧2 and * to ⇧7. So ⇧2 and ⇧7 no longer type the \" and ' printed on them; both are on Q."),
    dict(id="en-colemak", title="English - Colemak", tag="its own ; : on P", over=colemak_own,
         props={("Digit0", 1): "+"},
         text="Colemak's letters, with its own pair on P. The JIS ; key types o, so + had no key left and moved to ⇧0. : is on two keys."),
    dict(id="ko-2-set", title="Korean - 2-set", tag="KS X 5002", over=kts,
         text="Jamo on the letter keys, with ⇧ for ㅃ ㅉ ㄸ ㄲ ㅆ ㅒ ㅖ. ¥ types ₩, as in every Korean layout; no other symbol key is touched."),
    dict(id="ko-bada-roman", title="Korean - bada roman", tag="bada ime's layout 15", over=bada_roman,
         alt={c: JIS[(c, 1)][0] for c in KEYS if JIS[(c, 0)][1] == "x"}, missing=[],
         note="{ and } are on ⌥⇧ + [ and ], with the other shifted JIS characters (blue, top right). ⌥; is the middle dot ·.",
         text="Korean typed as it is romanized (hanguk 한국) on the QWERTY letters, with an English dictionary and Japanese on'yomi shortcuts. Its own symbol keys: ¥ is ₩, ⇧0 is +, ⇧[ and ⇧] are 「 and 」, and ; separates syllables, with ⇧; typing ;."),
    dict(id="ja-romaji", title="Japanese - Romaji", tag="", over=romaji, missing=[],
         note="Digits and the other symbols are typed full-width, and the half-width forms are rows of the menu.",
         text="The letters spell kana. Six keys type Japanese punctuation."),
    dict(id="ja-kana", title="Japanese - Kana", tag="JIS X 6002", over=kana, alt=KANA_ALT,
         summary="1–9, the unshifted symbols, and anything half-width",
         note="As on any JIS kana keyboard, and as bada ime 0.3.1 has it. In a profile with an English layout these come from that layout.",
         text="Kana on all 48 keys. ⇧ gives the small kana, を and 、 。 ・ 「 」; on the other keys it does nothing. In blue at the top right: ⌥⇧ + key (Alt+Shift off the Mac) types the key's shifted JIS character, full-width."),
]

sections, summary = [], []
for b in BOARDS:
    slots, missing, free = board(b["over"], b.get("props"))
    moved = b.get("props") or {}
    lines = []
    if b.get("summary"):
        verdict = '<span class="pill bad">by design</span>'
        lines.append('<p class="line"><b>Can\'t type</b> <span class="chip miss wide">%s</span></p>' % E(b["summary"]))
    elif moved:
        verdict = '<span class="pill mv">%d moved</span>' % len(moved)
        where = ", ".join('<span class="chip prop">%s</span> on %s' % (E(ch), E(slot_name(*k))) for k, ch in moved.items())
        lines.append('<p class="line"><b>Moved for JIS</b> %s</p>' % where)
    else:
        verdict = '<span class="pill ok">nothing moved</span>'
    if b.get("note"):
        lines.append('<p class="line note">%s</p>' % E(b["note"]))
    tag = '<span class="tag">%s</span>' % E(b["tag"]) if b["tag"] else ""
    sections.append('''<section id="%s"><header><h2>%s</h2>%s%s<code>%s</code></header>
<p class="lede">%s</p>%s%s</section>''' % (b["id"], E(b["title"]), tag, verdict, E(b["id"]), E(b["text"]),
                                            keyboard_html(slots, b.get("alt")), "".join(lines)))
    summary.append('<tr><td><a href="#%s">%s</a>%s</td><td>%s</td><td>%s</td><td class="num">%d</td></tr>' % (
        b["id"], E(b["title"]), ' <span class="sub">%s</span>' % E(b["tag"]) if b["tag"] else "",
        ('<span class="lost">%s</span>' % E(" ".join(moved.values()))) if moved else '<span class="none">nothing</span>',
        E(b["summary"]) if b.get("summary") else '<span class="none">nothing</span>', free))

PAGE = open(sys.argv[3], encoding="utf-8").read()
open(OUT, "w", encoding="utf-8").write(PAGE.replace("{{SUMMARY}}", "\n".join(summary)).replace("{{SECTIONS}}", "\n".join(sections)))
print("wrote", OUT, "boards:", len(BOARDS))
for b in BOARDS:
    s, m, f = board(b["over"], b.get("props"))
    print(" ", b["id"], "moved:", "".join((b.get("props") or {}).values()) or "-", "free:", f)
