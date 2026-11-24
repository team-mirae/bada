#!/bin/sh
# Sends the harness (and, with --all, the engine and what it builds from) from this machine to a
# Windows machine's copy of the repository, over SSH. The source of truth is here: edit locally,
# send, build there. Build folders and cargo's target folder are left out.
#   BADA_WINDOWS_HOST=user@host tools/send.sh            harness/windows
#   BADA_WINDOWS_HOST=user@host tools/send.sh --all      also engine, crates, docs, tools,
#                                                        Cargo.toml, Cargo.lock, VERSION
# BADA_WINDOWS_SSH is the ssh command (default: ssh). BADA_WINDOWS_DIR is the copy's folder there
# (default: %USERPROFILE%\Desktop\bada).
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
host=${BADA_WINDOWS_HOST:?set BADA_WINDOWS_HOST to user@host}
ssh=${BADA_WINDOWS_SSH:-ssh}
dir=${BADA_WINDOWS_DIR:-%USERPROFILE%\\Desktop\\bada}
paths="harness/windows"
if [ "${1:-}" = "--all" ]; then
    paths="$paths engine crates docs tools Cargo.toml Cargo.lock VERSION"
fi
# shellcheck disable=SC2086
COPYFILE_DISABLE=1 tar -C "$root" --no-xattrs --exclude 'harness/windows/build' --exclude 'target' \
    --exclude '.DS_Store' -cf - $paths |
    $ssh "$host" "cmd /c \"tar -xf - -C $dir\""
echo "sent: $paths"
