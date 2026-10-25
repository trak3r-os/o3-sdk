#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/remote_ci.sh [--slot <name>] <host|mk66> [mode...]: runs tools/ci.sh <mode>... for the working tree on the remote build
# machine, when one is configured (o3-sdk's docs/ci.md). Every agent may use it. The file is the same in o3-sdk and in an app
# repository that builds against it (o3-303).
#
# O3SDK_REMOTE_CI=<ssh-host>:<root> names the machine. It is set locally only (the `env` block of .claude/settings.local.json), never
# in git. <root> holds the bare repositories sdk.git (o3-sdk) and app.git (the app) and the wrapper o3sdkci.sh (installed from
# o3-sdk's tools/o3sdkci.sh), which checks the refs out in the slot's worktrees, runs the modes in one of the machine's CI turns and
# prints the log path, the summary and `rc=<n> secs=<n>`.
#
# The working tree is sent as it is, committed or not: tracked changes and untracked files that git does not ignore go into a
# snapshot commit on top of HEAD (a temporary index; the branch, the index and the tree stay as they are). The snapshot is pushed to
# refs/ci/<sha>, a new ref per commit, so nothing is ever forced. In an app, the o3sdk tree it builds against ($O3SDK_DIR, default
# ../o3-sdk) is sent the same way to sdk.git, and the run builds the app against exactly that tree.
# --slot   the remote worktree (default: this checkout's folder name). A slot keeps its build directories, so its runs are
#          incremental; the slots `dev` and `gate` start fresh.
# O3SDK_CI_TESTS=<regex> (ctest -R; letters, digits and _ . | ^ $ * + ? ( ) - only) and O3SDK_CI_KEEP=0|1 are passed on (tools/ci.sh).
# The full log is copied to build-remote/<log name> (untracked). Exit: the run's (tools/ci.sh); 2 bad usage; 3 no remote configured
# (run tools/ci.sh locally); 255 ssh failed (the machine is off or unreachable: run tools/ci.sh locally).
set -u

usage() { echo "usage: tools/remote_ci.sh [--slot <name>] <host|mk66> [mode...]" >&2; exit 2; }

slot=
while [ $# -gt 0 ]; do
  case "$1" in
    --slot) [ $# -ge 2 ] || usage; slot=$2; shift 2 ;;
    -h|--help) usage ;;
    -*) usage ;;
    *) break ;;
  esac
done
[ $# -ge 1 ] || usage
for m in "$@"; do
  case "$m" in host|mk66) ;; *) echo "remote_ci: unknown mode '$m'" >&2; exit 2 ;; esac
done

if [ -z "${O3SDK_REMOTE_CI:-}" ]; then
  echo "remote_ci: O3SDK_REMOTE_CI is not set; run tools/ci.sh locally (o3-sdk docs/ci.md)" >&2
  exit 3
fi
host=${O3SDK_REMOTE_CI%%:*}
root=${O3SDK_REMOTE_CI#*:}
if [ "$host" = "$O3SDK_REMOTE_CI" ] || [ -z "$host" ] || [ -z "$root" ]; then
  echo "remote_ci: O3SDK_REMOTE_CI must be <ssh-host>:<root>" >&2
  exit 2
fi
case "$root" in /*) ;; *) echo "remote_ci: the root in O3SDK_REMOTE_CI must be an absolute path" >&2; exit 2 ;; esac
case "$root" in *[!A-Za-z0-9._/-]*) echo "remote_ci: the root in O3SDK_REMOTE_CI may hold only letters, digits and . _ / -" >&2; exit 2 ;; esac

envs=
case "${O3SDK_CI_TESTS:-}" in
  "") ;;
  *[!A-Za-z0-9_.\|^\$*+?\(\)-]*) echo "remote_ci: O3SDK_CI_TESTS may hold only letters, digits and _ . | ^ \$ * + ? ( ) -" >&2; exit 2 ;;
  *) envs="${envs}O3SDK_CI_TESTS='$O3SDK_CI_TESTS' " ;;
esac
case "${O3SDK_CI_KEEP:-}" in
  "") ;;
  0|1) envs="${envs}O3SDK_CI_KEEP=$O3SDK_CI_KEEP " ;;
  *) echo "remote_ci: O3SDK_CI_KEEP must be 0 or 1" >&2; exit 2 ;;
esac

top=$(git rev-parse --show-toplevel) || exit 2
cd "$top" || exit 2
[ -n "$slot" ] || slot=$(basename "$top")
slot=$(printf '%s' "$slot" | tr -c 'A-Za-z0-9._-' '-')
case "$slot" in .*) slot="-${slot#.}" ;; esac

ssh_opts=(-o BatchMode=yes -o ConnectTimeout=10)
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

# snapshot <dir> <name>: prints the commit that holds <dir>'s working tree: HEAD when the tree is clean, else a snapshot commit on
# top of it, made in a temporary index (HEAD's index plus every tracked change and untracked file that git does not ignore).
snapshot() (
  cd "$1" || exit 2
  idx=$tmp/index.$2
  cp "$(git rev-parse --git-path index)" "$idx" || exit 2
  GIT_INDEX_FILE=$idx git add -u -- . || exit 2
  git ls-files -z --others --exclude-standard | GIT_INDEX_FILE=$idx xargs -0 -r git add -- || exit 2
  tree=$(GIT_INDEX_FILE=$idx git write-tree) || exit 2
  head=$(git rev-parse HEAD) || exit 2
  if [ "$tree" = "$(git rev-parse "HEAD^{tree}")" ]; then
    echo "remote_ci: $2 HEAD $(git rev-parse --short HEAD), clean tree" >&2
    echo "$head"
  else
    sha=$(git commit-tree "$tree" -p "$head" -m "remote_ci snapshot of $(git rev-parse --short HEAD)") || exit 2
    echo "remote_ci: $2 snapshot $(git rev-parse --short "$sha") of HEAD $(git rev-parse --short HEAD) plus the working tree" >&2
    echo "$sha"
  fi
)

# push <dir> <sha> <bare repository name>: the commit to refs/ci/<sha> of <root>/<name>; a new ref per commit, never forced.
push() {
  git -C "$1" -c core.sshCommand="ssh ${ssh_opts[*]}" push -q "ssh://$host$root/$3" "$2:refs/ci/$2"
}

if [ -f "$top/cmake/O3sdkPlatform.cmake" ]; then
  sha=$(snapshot "$top" o3-sdk) || exit 2
  push "$top" "$sha" sdk.git || exit 255
  cmd="$root/o3sdkci.sh sdk $slot $sha $*"
else
  sdk=${O3SDK_DIR:-$top/../o3-sdk}
  [ -f "$sdk/cmake/O3sdkPlatform.cmake" ] || { echo "remote_ci: '$sdk' is not an o3sdk checkout (set O3SDK_DIR)" >&2; exit 2; }
  sdk=$(git -C "$sdk" rev-parse --show-toplevel) || exit 2
  sha=$(snapshot "$top" "$(basename "$top")") || exit 2
  sdk_sha=$(snapshot "$sdk" o3-sdk) || exit 2
  push "$sdk" "$sdk_sha" sdk.git || exit 255
  push "$top" "$sha" app.git || exit 255
  cmd="$root/o3sdkci.sh app $slot $sha $sdk_sha $*"
fi

echo "remote_ci: slot $slot, modes $*; waiting for a CI turn if the machine's four are taken"
out=$(ssh "${ssh_opts[@]}" "$host" "$envs$cmd")
rc=$?
printf '%s\n' "$out"
log=$(printf '%s\n' "$out" | head -1)
case "$log" in
  "$root"/logs/*.log)
    mkdir -p build-remote
    ssh "${ssh_opts[@]}" "$host" "cat $log" > "build-remote/$(basename "$log")" &&
      echo "remote_ci: full log in build-remote/$(basename "$log")" ;;
esac
exit $rc
