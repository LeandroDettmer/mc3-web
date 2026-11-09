#!/usr/bin/env bash
# Build the browser module from a clean checkout of a commit and promote it to
# the owner's build (:3000), leaving the shared working tree and its
# uncommitted edits out of it.
#
#   git worktree add --detach ../mc3-web-main <commit>   (Windows git, once;
#   later: git -C ../mc3-web-main checkout --detach <commit>)
#   tools/web_release_build.sh                            (in WSL; GAME=remix)
#
# Runs in WSL with emsdk; git stays on the Windows side (a worktree made by
# Windows git records a Windows path WSL's git cannot read). The clean tree is
# ../mc3-web-main; it borrows this tree's lift (work-<game>/gen, venv) and
# disc through links.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
game="${GAME:-remix}"
wt="$(dirname "$repo")/mc3-web-main"
[ -d "$wt/tools" ] || { echo "no clean tree in $wt: git worktree add --detach ../mc3-web-main HEAD (Windows git)" >&2; exit 1; }
mkdir -p "$wt/work-$game"
for d in gen venv analysis; do ln -sfn "$repo/work-$game/$d" "$wt/work-$game/$d"; done
for g in "$repo"/game-"$game"*; do [ -e "$g" ] && ln -sfn "$g" "$wt/$(basename "$g")"; done
. /opt/emsdk/emsdk_env.sh >/dev/null 2>&1
# the checkout's mtimes are new; the lift itself is this tree's, already checked
(cd "$wt" && MC3_SKIP_LIFT_CHECK=1 GAME="$game" JOBS="${JOBS:-6}" bash tools/build_web.sh)
GAME="$game" bash "$repo/tools/web_promote.sh" "$wt/work-$game/build-web"
echo "release build of $wt done"
