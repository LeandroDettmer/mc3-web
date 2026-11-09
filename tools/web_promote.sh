#!/usr/bin/env bash
# Promote an agent's browser build to the one the owner plays on :3000.
#
#   tools/web_promote.sh <build folder> [GAME=remix]
#
# Copies the module (mcdub3<game>.js/.wasm/.js.symbols) into
# work-<game>/build-web, each file through a temporary name and a rename, so a
# page that loads during the copy gets the old file or the new one, never half.
set -euo pipefail
src="${1:?usage: tools/web_promote.sh <build folder>}"
game="${GAME:-remix}"
app="mcdub3${game}"; [ "$game" = dub ] && app="mcdub3"
dst="$(cd "$(dirname "$0")/.." && pwd)/work-${game}/build-web"
mkdir -p "$dst"
for f in "$app.wasm" "$app.js.symbols" "$app.js"; do   # the .js last: it names the .wasm
    [ -f "$src/$f" ] || continue
    cp "$src/$f" "$dst/.$f.tmp"
    mv -f "$dst/.$f.tmp" "$dst/$f"
done
echo "promoted $src -> $dst ($(stat -c %s "$dst/$app.wasm") bytes wasm)"
