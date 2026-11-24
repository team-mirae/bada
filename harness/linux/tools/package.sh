#!/bin/sh
# Builds what a release carries for Linux: build/dist/bada-<version>-linux-<arch>.tar.gz. It holds
# the Fcitx5 addon as `cmake --install` lays it out under /usr, with INSTALL.txt. The IBus engine is
# left out: it has not been run.
#
# The addon is built against this machine's Fcitx5 and C++ library, so it loads where those are the
# same or newer. INSTALL.txt says what it was built with.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
version=$(cat "$root/VERSION")
name=bada-$version-linux-$(uname -m)
build=${BADA_BUILD:-$here/build/package}
stage=$build/stage/$name

rm -rf "$build/stage"
cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBADA_IBUS=OFF -DBADA_TESTS=OFF
cmake --build "$build" -j
DESTDIR="$stage" cmake --install "$build" --strip
cp "$root/LICENSE" "$stage/LICENSE.txt"

addon=$(cd "$stage" && find usr -name 'libbada-fcitx5.so')
. /etc/os-release
cat > "$stage/INSTALL.txt" <<EOF
bada $version for Linux ($(uname -m)): the Fcitx5 addon.

Built on $PRETTY_NAME with Fcitx5 $(fcitx5 --version) and $(ldd --version | head -1).
It loads where Fcitx5 and the C++ library are the same or newer. Elsewhere, build it from the
source: https://github.com/team-mirae/bada/tree/dev-latest/harness/linux

Install:

    sudo cp -a usr/. /usr/
    fcitx5 -r

The addon is $addon. A distribution that keeps Fcitx5's addons elsewhere (Debian and Ubuntu:
/usr/lib/x86_64-linux-gnu/fcitx5) needs it moved there.

Then add bada to a group with fcitx5-configtool (it is listed as Multilingual), and take three
keys out of Fcitx5's own hotkeys, in Global Options: Zenkaku_Hankaku out of Trigger Input Method,
and Hangul and Hangul_Hanja out of Activate and Deactivate.

A new install has no layouts chosen, so keys type as the system has them until bada has a
settings file, ~/.config/bada/settings.json. How to write one:
https://github.com/team-mirae/bada/blob/dev-latest/harness/linux/README.md#first-run

Remove:

    sudo rm /$addon /usr/share/fcitx5/addon/bada.conf /usr/share/fcitx5/inputmethod/bada.conf
    fcitx5 -r
EOF

mkdir -p "$here/build/dist"
tar -C "$build/stage" -czf "$here/build/dist/$name.tar.gz" "$name"
echo "$here/build/dist/$name.tar.gz"
