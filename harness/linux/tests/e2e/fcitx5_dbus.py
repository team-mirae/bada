"""End to end through the installed addon: a fresh `fcitx5` that loads bada from an install tree,
driven over its D-Bus frontend (org.fcitx.Fcitx.InputMethod1) as an app's input module drives it.
The published state is read as a script would read it: the file, and the D-Bus signal.

tools/test.sh runs it, inside its own session bus, home and runtime folder."""

import json
import os
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
SERVICE = "org.fcitx.Fcitx5"
IME = "bada"
CONFIG = os.path.join(os.environ["XDG_CONFIG_HOME"], "bada")

# token -> (keysym under a US layout, evdev code)
KEYS = {
    **{c: (ord(c), code) for c, code in zip("qwertyuiop", range(16, 26))},
    **{c: (ord(c), code) for c, code in zip("asdfghjkl", range(30, 39))},
    **{c: (ord(c), code) for c, code in zip("zxcvbnm", range(44, 51))},
    "Space": (0x20, 57), "Enter": (0xFF0D, 28), "Backspace": (0xFF08, 14), "Equal": (ord("="), 13),
    "muhenkan": (0xFF22, 94), "henkan": (0xFF23, 92), "katakana": (0xFF27, 93), "grave": (ord("`"), 41),
}
SHIFT, CTRL, ALT = 1, 4, 8
# CapabilityFlag (fcitx-utils/capabilityflags.h)
PREEDIT, PASSWORD, FORMATTED, SURROUNDING = 1 << 1, 1 << 3, 1 << 4, 1 << 6

failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {what}", flush=True)


def check_eq(got, want, what):
    check(got == want, f"{what}: got {got!r}, want {want!r}")


def pump(seconds=0.15):
    """Runs the main loop until nothing has happened for a while."""
    ctx = GLib.MainContext.default()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if ctx.iteration(False):
            end = time.monotonic() + seconds
        else:
            time.sleep(0.005)


def wait_for(predicate, seconds=10.0, what="condition"):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return True
        pump(0.05)
    check(False, f"timed out waiting for {what}")
    return False


def read_state():
    try:
        with open(os.path.join(os.environ["XDG_RUNTIME_DIR"], "bada", "state.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_settings(settings):
    os.makedirs(CONFIG, exist_ok=True)
    tmp = os.path.join(CONFIG, "settings.json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(settings, f, ensure_ascii=False)
    os.rename(tmp, os.path.join(CONFIG, "settings.json"))


wait_for(lambda: bus.name_has_owner(SERVICE), 20, "fcitx5 on the bus")
controller = dbus.Interface(bus.get_object(SERVICE, "/controller"), "org.fcitx.Fcitx.Controller1")
im = dbus.Interface(bus.get_object(SERVICE, "/org/freedesktop/portal/inputmethod"), "org.fcitx.Fcitx.InputMethod1")

ims = [str(entry[0]) for entry in controller.AvailableInputMethods()]
check(IME in ims, f"{IME} is listed ({ims})")
# Another input method that must be listed beside bada's (bada-linux's, where it is installed).
for other in filter(None, os.environ.get("BADA_E2E_BESIDE", "").split(",")):
    check(other in ims, f"{other} is listed beside bada ({ims})")

signals = []
bus.add_signal_receiver(lambda js: signals.append(json.loads(str(js))), signal_name="Changed",
                        dbus_interface="com.teammirae.Bada.State", path="/com/teammirae/Bada")


class Client:
    """An app's text field: what it holds, and what bada told it."""

    def __init__(self, caps=PREEDIT | FORMATTED | SURROUNDING, program="e2e"):
        path, _ = im.CreateInputContext([("program", program), ("display", "x11:")])
        self.ic = dbus.Interface(bus.get_object(SERVICE, path), "org.fcitx.Fcitx.InputContext1")
        self.text = ""
        self.preedit = ""
        self.commits = []
        iface = "org.fcitx.Fcitx.InputContext1"
        bus.add_signal_receiver(self.commit, signal_name="CommitString", dbus_interface=iface, path=path)
        bus.add_signal_receiver(lambda parts, cursor: setattr(self, "preedit", "".join(str(p[0]) for p in parts)),
                                signal_name="UpdateFormattedPreedit", dbus_interface=iface, path=path)
        bus.add_signal_receiver(self.delete, signal_name="DeleteSurroundingText", dbus_interface=iface, path=path)
        self.ic.SetCapability(dbus.UInt64(caps))

    def commit(self, s):
        self.commits.append(str(s))
        self.text += str(s)

    def delete(self, offset, size):
        self.text = self.text[: len(self.text) + int(offset)] + self.text[len(self.text) + int(offset) + int(size):]

    def focus(self):
        self.ic.FocusIn()
        pump()
        if str(controller.CurrentInputMethod()) != IME:
            controller.SetCurrentIM(IME)
            pump()

    def key(self, token, state=0, release=False):
        sym, code = KEYS[token]
        if state & SHIFT and len(token) == 1:
            sym = ord(token.upper())
        eaten = bool(self.ic.ProcessKeyEvent(dbus.UInt32(sym), dbus.UInt32(code + 8), dbus.UInt32(state), release,
                                             dbus.UInt32(0)))
        pump(0.05)
        if not eaten and not release and not (state & (CTRL | ALT)):
            # What the app does with a key bada didn't eat: its own US layout types it.
            if token == "Space":
                self.text += " "
            elif token == "Enter":
                self.text += "\n"
            elif token == "Backspace":
                self.text = self.text[:-1]
            elif sym < 0x7F:
                self.text += chr(sym)
        return eaten

    def type(self, tokens):
        for t in tokens.split():
            self.key(t)
            self.key(t, release=True)


a = Client()
a.focus()
check_eq(str(controller.CurrentInputMethod()), IME, "the input method in use")
state = read_state()
check(state.get("format") == "bada-compatible-state/2" and state.get("harness", {}).get("id") == "com.teammirae.bada.linux"
      and state["harness"].get("frontend") == "fcitx5" and state["harness"].get("app") == "e2e",
      f"the state file at focus in ({state})")
check(state.get("harness", {}).get("keys", {}).get("available") ==
      ["Backquote", "NonConvert", "Convert", "KanaMode", "Lang2", "Lang1", "CapsLock"],
      "harness.keys.available")
check(state.get("harness", {}).get("problems") == [], f"no problems with the settings ({state.get('harness', {}).get('problems')})")

# QWERTY in muhenkan: the app's layout types the same letters, so the keys are let through. The
# JIS ^ key isn't what a US layout has there: bada types it.
a.type("h e l l o")
check_eq(a.text, "hello", "QWERTY in muhenkan")
check_eq(a.commits, [], "letters the OS types the same are let through")
check(a.key("Equal"), "the JIS ^ key is eaten")
a.key("Equal", release=True)
check_eq(a.text, "hello^", "^ is typed by bada")

# 変換: henkan, the first profile. Korean 2-set: gks is 한; Space commits and goes to the app.
check(a.key("henkan"), "変換 is eaten")
a.key("henkan", release=True)
state = read_state()
check(state.get("mode") == "henkan" and state.get("profile") == "main", f"変換 enters henkan ({state.get('mode')}, {state.get('profile')})")
a.type("g k s")
check_eq(a.preedit, "한", "gks is 한")
state = read_state()
check(state.get("staged") == "한" and state.get("preedit", {}).get("text") == "한", "the state file while composing")
check(any(s.get("staged") == "한" for s in signals), "com.teammirae.Bada.State.Changed carried the state")
if os.environ.get("BADA_E2E_SHOW"):
    print(json.dumps(state, ensure_ascii=False), flush=True)
a.type("Space")
check_eq(a.text, "hello^한 ", "Space commits 한, then the app types the space")
check_eq(a.preedit, "", "nothing is marked after the commit")

# カタカナ: the profile ja (the table's own binding). Romaji: ka is か.
a.type("katakana k a")
check_eq(read_state().get("profile"), "ja", "カタカナ enters its profile")
check_eq(a.preedit, "か", "ka is か")
# 無変換: muhenkan. The select commits what is staged.
a.type("muhenkan")
check_eq(a.text, "hello^한 か", "無変換 commits what is staged")
check_eq(read_state().get("mode"), "muhenkan", "無変換 leaves henkan")
check(any(s.get("mode") == "muhenkan" for s in signals[-3:]), "a signal carried the mode")

# Focus out commits what is staged, once.
a.type("katakana s a")
before = len(a.commits)
a.ic.FocusOut()
pump()
check(len(a.commits) == before + 1 and a.text.endswith("かさ"), f"focus out commits once ({a.commits[before:]})")

# Another field follows the session's mode and profile.
b = Client(program="other")
b.focus()
b.type("k i muhenkan x")
check_eq(b.text, "きx", "a second field follows, and switches for everyone")

# 半角/全角 with nothing staged makes the selection full-width (ask, answered from surrounding text).
c = Client(program="selection")
c.focus()
c.text = "abc"
c.ic.SetSurroundingText("abc", dbus.UInt32(3), dbus.UInt32(0))
pump(0.05)
c.type("grave")
check_eq(c.text, "ａｂｃ", "半角/全角 makes the selection full-width")

# A password field: nothing composes, and nothing typed is published.
p = Client(caps=PREEDIT | FORMATTED | PASSWORD, program="pw")
p.focus()
p.type("henkan s e c r e t")
check_eq(p.preedit, "", "nothing composes in a password field")
check_eq(p.text, "secret", "a password field gets plain keys")
state = read_state()
check(state.get("harness", {}).get("secure") is True and "secret" not in json.dumps(state, ensure_ascii=False),
      f"the state hides a password field ({state})")
check(all("secret" not in json.dumps(s, ensure_ascii=False) for s in signals), "no signal carried the password")
p.ic.FocusOut()
pump()

# A settings file written while it runs is taken up: the folder is watched.
a.focus()
write_settings({"format": "bada-settings/2",
                "engine": {"muhenkan": {"layout": "en-qwerty"},
                           "henkan": {"profiles": [{"id": "only", "name": "かな", "layouts": ["ja-kana"]}]}}})
wait_for(lambda: read_state().get("profile") is None and read_state().get("mode") == "muhenkan", 5,
         "the reload (the selected profile is gone: muhenkan)")
a.text = ""
a.type("henkan t Enter")
check_eq(read_state().get("profile"), "only", "変換 enters the new first profile")
check_eq(a.text, "か\n", "JIS kana: the T key is か")
# A broken file changes nothing, and says so in the state.
with open(os.path.join(CONFIG, "settings.json"), "w", encoding="utf-8") as f:
    f.write("{ broken")
wait_for(lambda: read_state().get("harness", {}).get("problems"), 5, "the broken file's problem in the state")
problems = read_state().get("harness", {}).get("problems") or [{}]
check(problems[0].get("code") == "bad-json" and problems[0].get("file") == "settings.json", f"the problem ({problems})")
a.text = ""
a.type("t Enter")
check_eq(a.text, "か\n", "the settings from before stay in effect")

print(f"fcitx5 over D-Bus: {checks} checks, {failures} failed", flush=True)
sys.exit(1 if failures else 0)
