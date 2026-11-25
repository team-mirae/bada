#!/bin/sh
# Runs tests/e2e/fcitx5_dbus.py: a fresh fcitx5 that loads the installed addon, driven over D-Bus.
# It has its own session bus, home and runtime folder, so a Fcitx5 that is running is left alone.
#
#   tools/e2e-dbus.sh <prefix>          the addon installed under <prefix> (a staging folder's usr)
#   tools/e2e-dbus.sh                   the addon installed in the system
#   BADA_E2E_BESIDE=bada-linux:bada-ara tools/e2e-dbus.sh
#       also enables another addon (bada-linux) and checks that its input method (bada-ara) is
#       listed beside bada's: the two install side by side
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
prefix=${1:-}
if ! python3 -c 'import dbus, gi' 2> /dev/null; then
  echo "(python-dbus or python-gobject is missing: skipped)"
  exit 0
fi
work=$(mktemp -d /tmp/bada-e2e-XXXXXX)
trap 'rm -rf "$work"' EXIT
export HOME=$work/home XDG_RUNTIME_DIR=$work/run
export XDG_CONFIG_HOME=$HOME/.config
mkdir -p "$XDG_CONFIG_HOME/fcitx5" "$XDG_CONFIG_HOME/bada" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
unset XDG_DATA_HOME WAYLAND_DISPLAY DISPLAY
# No data, whatever is installed: what the test types doesn't depend on it.
export BADA_DATA_DIR=$work/no-data
if [ -n "$prefix" ]; then
  # Fcitx5 finds the addon under the prefix first, and its own addons where they are installed.
  libdir=$(cd "$prefix"/lib*/fcitx5 && pwd)
  export XDG_DATA_DIRS=$prefix/share:/usr/local/share:/usr/share
  export FCITX_ADDON_DIRS=$libdir:$(pkg-config --variable=libdir Fcitx5Core)/fcitx5
fi
beside=${BADA_E2E_BESIDE:-}
addons=dbus,dbusfrontend,keyboard,bada${beside:+,${beside%%:*}}
export BADA_E2E_BESIDE=${beside#*:}

cat > "$XDG_CONFIG_HOME/fcitx5/profile" <<'END'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=bada

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=bada
Layout=

[GroupOrder]
0=Default
END
# Fcitx5's defaults take 半角/全角 and an Apple keyboard's 英数 and かな as its own hotkeys, and keep
# input methods out of password fields. The README has users change the first; the second is
# turned on here so that bada's own handling of a password field is tested.
cat > "$XDG_CONFIG_HOME/fcitx5/config" <<'END'
[Hotkey/TriggerKeys]
0=Control+space

[Hotkey/AltTriggerKeys]

[Hotkey/ActivateKeys]

[Hotkey/DeactivateKeys]

[Behavior]
AllowInputMethodForPassword=True
END
cat > "$XDG_CONFIG_HOME/bada/settings.json" <<'END'
{"format": "bada-settings/2",
 "engine": {"muhenkan": {"layout": "en-qwerty"},
            "henkan": {"profiles": [{"id": "main", "name": "한국어", "layouts": ["ko-2-set"]},
                                    {"id": "ja", "name": "日本語", "layouts": ["ja-romaji"]}]}},
 "keys": {"bindings": [{"on": "NonConvert", "do": ["mode:muhenkan"]},
                       {"on": "Convert", "do": ["profile.first"]},
                       {"on": "Convert", "do": ["menu.next", "profile.first"], "when": "henkan"},
                       {"on": "KanaMode", "do": ["profile:ja", "menu.next"]},
                       {"on": "Backquote", "do": ["width.full"]}]}}
END
export BADA_E2E_ADDONS=$addons BADA_E2E_TEST=$here/tests/e2e/fcitx5_dbus.py
dbus-run-session -- sh -c '
  fcitx5 --disable=all --enable="$BADA_E2E_ADDONS" > "$HOME/fcitx5.log" 2>&1 &
  pid=$!
  python3 "$BADA_E2E_TEST"; status=$?
  kill $pid 2> /dev/null; wait $pid 2> /dev/null
  if [ $status -ne 0 ]; then echo "--- fcitx5 log"; tail -40 "$HOME/fcitx5.log"; fi
  exit $status'
