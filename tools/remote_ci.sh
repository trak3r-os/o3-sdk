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
# O3SDK_CI_TESTS=<regex> (ctest -R; letters, digits and _ . | ^ $ * + ? ( ) - only), O3SDK_CI_KEEP=0|1 and O3SDK_CI_SLOW=0|1 are passed on
# (tools/ci.sh); O3SDK_REMOTE_JOBS=<n> becomes O3SDK_CI_JOBS on the machine (a name of its own, so a local O3SDK_CI_JOBS never reaches it).
# O3SDK_CI_SCOPE=changed is worked out here, by `tools/ci.sh --scope` (the same code as a local run: the base is O3SDK_CI_BASE, else the green
#          ref refs/ci/green/<mode>, else the merge-base with origin/main; the test names come from a configured local build directory,
#          build-ci-host or build-host): `none` sends nothing and exits 0; `all` is a normal full run, cacheable; otherwise the regex goes as
#          O3SDK_CI_TESTS (a filtered run, never cached). Several modes: any `all` makes it all, else the union of the regexes, and a
#          mode whose own scope is `none` builds only (O3SDK_CI_BUILD_ONLY), so the regex of another mode never meets its ctest.
# One run per slot at a time: a run holds flock on <git common dir>/remote-ci-slot-<slot>.lock from before the snapshot to its exit, so two
#          runs of one slot (same checkout, or worktrees given the same --slot) go one after the other, never checking the slot's worktree out
#          under a running build; a waiting run prints one line. Runs of different slots go at once, and each slot builds cold the first time:
#          never invent a slot name. Two runs of one commit that push the same refs/ci/<sha> together are fine: a push that fails because the
#          ref already holds that sha counts as done.
# Green cache: after rc=0 of a full run (no O3SDK_CI_TESTS) the line `<key> <mode> <date> <slot>` goes to <git common dir>/ci-green.log, key
#          = sha1 of the snapshot tree(s), the mode and the env sent to the machine. A later run of the same tree skips a mode whose key is
#          there and exits 0 without ssh when all are skipped. O3SDK_CI_FORCE=1 runs anyway (after a remote toolchain change, say).
# Green ref: a full run that is green (rc=0, no O3SDK_CI_TESTS or scope regex, no O3SDK_CI_SLOW=0), or a mode the cache skips, points the local
#          ref refs/ci/green/<mode> at the snapshot commit pushed (an app also stores the o3sdk tree in refs/ci/green/<mode>-sdk), the base of
#          O3SDK_CI_SCOPE=changed (tools/ci.sh). The machine's own ci.sh records nothing (O3SDK_CI_GREEN=0).
# Evidence: whenever the machine ran its tests to an rc, green or red (a ctest summary and `rc=` in the answer), <git common dir>/ci-remote-last-run
#          is touched, as a local ctest run writes its LastTest.log: .claude/hooks/verify_gate.py counts the newer of the two. Exit 3, 255,
#          a failed push, a run that died before ctest and the green cache's skip touch nothing.
# Jobs: without O3SDK_REMOTE_JOBS the run asks the machine how many of its four turns are busy and takes nproc/(busy+1), at least 4.
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
  *[!][A-Za-z0-9_.\|^\$*+?\(\)-]*) echo "remote_ci: O3SDK_CI_TESTS may hold only letters, digits and _ . | ^ \$ * + ? ( ) [ ] -" >&2; exit 2 ;;
  *) envs="${envs}O3SDK_CI_TESTS='$O3SDK_CI_TESTS' " ;;
esac
case "${O3SDK_CI_SCOPE:-}" in ""|changed) ;; *) echo "remote_ci: O3SDK_CI_SCOPE must be 'changed' (or unset)" >&2; exit 2 ;; esac
case "${O3SDK_CI_KEEP:-}" in
  "") ;;
  0|1) envs="${envs}O3SDK_CI_KEEP=$O3SDK_CI_KEEP " ;;
  *) echo "remote_ci: O3SDK_CI_KEEP must be 0 or 1" >&2; exit 2 ;;
esac
case "${O3SDK_CI_SLOW:-}" in
  "") ;;
  0|1) envs="${envs}O3SDK_CI_SLOW=$O3SDK_CI_SLOW " ;;
  *) echo "remote_ci: O3SDK_CI_SLOW must be 0 or 1" >&2; exit 2 ;;
esac
case "${O3SDK_REMOTE_JOBS:-}" in
  "") ;;
  *[!0-9]*) echo "remote_ci: O3SDK_REMOTE_JOBS must be a number" >&2; exit 2 ;;
  *) envs="${envs}O3SDK_CI_JOBS=$O3SDK_REMOTE_JOBS " ;;
esac

top=$(git rev-parse --show-toplevel) || exit 2
cd "$top" || exit 2
[ -n "$slot" ] || slot=$(basename "$top")
slot=$(printf '%s' "$slot" | tr -c 'A-Za-z0-9._-' '-')
case "$slot" in .*) slot="-${slot#.}" ;; esac

# One run per slot at a time. The lock sits in the git common dir, shared by all worktrees; fd 9 holds it until the script exits; every
# ssh and git push below closes it (9>&-), so a lingering ssh master (ControlPersist) never keeps the lock after this script is gone.
command -v flock > /dev/null 2>&1 || { echo "remote_ci: flock not found" >&2; exit 2; }
lockfile=$(git rev-parse --path-format=absolute --git-common-dir)/remote-ci-slot-$slot.lock || exit 2
exec 9> "$lockfile" || exit 2
if ! flock -n 9; then
  echo "remote_ci: waiting for slot $slot"
  flock 9 || exit 2
fi

ssh_opts=(-o BatchMode=yes -o ConnectTimeout=10)

# snapshot <dir> <name>: prints the commit that holds <dir>'s working tree: HEAD when the tree is clean, else a snapshot commit on top of
# it, made in a temporary index (tools/ci.sh --snapshot: the code a local run's green ref uses too).
snapshot() { "$top/tools/ci.sh" --snapshot "$1" "$2" 9>&-; }

# push <dir> <sha> <bare repository name>: the commit to refs/ci/<sha> of <root>/<name>; a new ref per commit, never forced. A second run
# of the same commit may have pushed the ref between our snapshot and our push: then the ref holds <sha> and the push counts as done.
push() {
  local have
  if ! git -C "$1" -c core.sshCommand="ssh ${ssh_opts[*]}" push -q "ssh://$host$root/$3" "$2:refs/ci/$2" 9>&-; then
    have=$(git -C "$1" -c core.sshCommand="ssh ${ssh_opts[*]}" ls-remote "ssh://$host$root/$3" "refs/ci/$2" 9>&- 2> /dev/null | cut -f1)
    [ "$have" = "$2" ]
  fi
}

if [ -f "$top/cmake/O3sdkPlatform.cmake" ]; then
  sha=$(snapshot "$top" o3-sdk) || exit 2
  tree=$(git -C "$top" rev-parse "$sha^{tree}") || exit 2
else
  sdk=${O3SDK_DIR:-$top/../o3-sdk}
  [ -f "$sdk/cmake/O3sdkPlatform.cmake" ] || { echo "remote_ci: '$sdk' is not an o3sdk checkout (set O3SDK_DIR)" >&2; exit 2; }
  sdk=$(git -C "$sdk" rev-parse --show-toplevel) || exit 2
  sha=$(snapshot "$top" "$(basename "$top")") || exit 2
  sdk_sha=$(snapshot "$sdk" o3-sdk) || exit 2
  sdk_tree=$(git -C "$sdk" rev-parse "$sdk_sha^{tree}") || exit 2
  tree="$(git -C "$top" rev-parse "$sha^{tree}") $sdk_tree" || exit 2
fi

# O3SDK_CI_SCOPE=changed: the scope is worked out here (the machine's worktree has no history to diff against).
filtered=0
[ -z "${O3SDK_CI_TESTS:-}" ] || filtered=1
if [ "${O3SDK_CI_SCOPE:-}" = changed ] && [ $filtered = 0 ]; then
  scopedir=$top/build-host
  for d in build-ci-host build-host; do
    if [ -f "$top/$d/CTestTestfile.cmake" ]; then scopedir=$top/$d; break; fi
  done
  scoped=
  anym=0
  nonem=
  for m in "$@"; do
    r=$("$top/tools/ci.sh" --scope "$scopedir" "$m" 9>&-) || { echo "remote_ci: tools/ci.sh --scope failed" >&2; exit 2; }
    case "$r" in
      all) anym=1 ;;
      none) nonem="$nonem $m" ;;
      *) r=${r#^(}; r=${r%)\$}; scoped="$scoped|$r" ;;
    esac
  done
  scoped=${scoped#|}
  if [ $anym = 1 ]; then
    echo "remote_ci: scope changed: a full run"
  elif [ -z "$scoped" ]; then
    echo "remote_ci: scope changed: no test covers what changed since the base; nothing sent (exit 0)"
    exit 0
  elif [ ${#scoped} -gt 30000 ]; then
    echo "remote_ci: scope changed: the regex is too long to send: a full run"
  else
    envs="${envs}O3SDK_CI_TESTS='^($scoped)\$' "
    nonem=${nonem# }
    if [ -n "$nonem" ]; then
      envs="${envs}O3SDK_CI_BUILD_ONLY='$nonem' "
      echo "remote_ci: scope changed: $nonem: no test covers what changed; build only"
    fi
    filtered=1
    echo "remote_ci: scope changed: O3SDK_CI_TESTS sent, ${#scoped} characters (a filtered run is not cached)"
  fi
fi

# Green cache: a mode this exact tree (the app's and, in an app, the o3sdk tree it builds against) already passed in full is skipped; when
# every mode is skipped nothing is sent and the exit is 0. A filtered run (O3SDK_CI_TESTS) is never cached or looked up. The key is the
# sha1 of the tree(s), the mode and the environment sent to the machine (not the job count); it does not cover the machine's own state
# (toolchain, wrapper): run with O3SDK_CI_FORCE=1 after changing that.
green_log=$(git rev-parse --path-format=absolute --git-common-dir)/ci-green.log
keyenv=$(printf '%s' "$envs" | sed 's/O3SDK_CI_JOBS=[0-9]* //g')
cacheable=0
[ $filtered = 0 ] && cacheable=1
common=$(git rev-parse --path-format=absolute --git-common-dir)
# record_green <mode>: the local green ref of a full, green mode (not with O3SDK_CI_SLOW=0, a run that left tests out).
record_green() {
  [ $cacheable = 1 ] && [ "${O3SDK_CI_SLOW:-1}" != 0 ] || return 0
  if [ -f "$top/cmake/O3sdkPlatform.cmake" ]; then
    "$top/tools/ci.sh" --record-green "$1" "$sha" 9>&-
  else
    "$top/tools/ci.sh" --record-green "$1" "$sha" "$sdk_tree" 9>&-
  fi || echo "remote_ci: could not record the green ref of $1" >&2
}
modekey() { printf '%s' "$tree $1 $keyenv" | sha1sum | cut -d' ' -f1; }
if [ $cacheable = 1 ] && [ "${O3SDK_CI_FORCE:-}" != 1 ]; then
  todo=()
  for m in "$@"; do
    hit=$(grep -s "^$(modekey "$m") " "$green_log" | tail -1)
    if [ -n "$hit" ]; then
      read -r _ hmode hdate hslot <<< "$hit"
      echo "remote_ci: $hmode already green for this tree ($hdate, slot $hslot); skipped (O3SDK_CI_FORCE=1 runs it anyway)"
      record_green "$m"
    else
      todo+=("$m")
    fi
  done
  [ ${#todo[@]} -gt 0 ] || exit 0
  set -- "${todo[@]}"
fi

# Jobs by load: without O3SDK_REMOTE_JOBS, count the machine's busy turns (the wrapper holds $root/.turn.<n>, four of them) and share its
# threads: nproc/(busy+1), at least 4. A floor against stacking a full -j on a loaded machine. Read-only.
case "$envs" in
  *O3SDK_CI_JOBS=*) ;;
  *)
    auto=$(ssh "${ssh_opts[@]}" "$host" "busy=0; for n in 0 1 2 3; do flock -n $root/.turn.\$n true 2>/dev/null || busy=\$((busy+1)); done; echo \$(nproc) \$busy" 9>&- 2> /dev/null)
    read -r nproc busy <<< "$auto"
    case "${nproc:-x}${busy:-x}" in
      *[!0-9]*) ;;
      *)
        jobs=$(( nproc / (busy + 1) )); [ $jobs -ge 4 ] || jobs=4
        envs="${envs}O3SDK_CI_JOBS=$jobs "
        echo "remote_ci: jobs=$jobs (auto: $busy of 4 turns busy, $nproc threads)" ;;
    esac ;;
esac

if [ -f "$top/cmake/O3sdkPlatform.cmake" ]; then
  push "$top" "$sha" sdk.git || exit 255
  cmd="$root/o3sdkci.sh sdk $slot $sha $*"
else
  push "$sdk" "$sdk_sha" sdk.git || exit 255
  push "$top" "$sha" app.git || exit 255
  cmd="$root/o3sdkci.sh app $slot $sha $sdk_sha $*"
fi

echo "remote_ci: slot $slot, modes $*; waiting for a CI turn if the machine's four are taken"
out=$(ssh "${ssh_opts[@]}" "$host" "O3SDK_CI_GREEN=0 $envs$cmd" 9>&-)
rc=$?
printf '%s\n' "$out"
printed_rc=$(printf '%s\n' "$out" | sed -n 's/.*rc=\([0-9]*\) secs=.*/\1/p' | tail -1)
if [ -n "$printed_rc" ] && printf '%s\n' "$out" | grep -Eq 'tests passed|tests failed'; then
  touch "$common/ci-remote-last-run"  # evidence for verify_gate.py: the tests ran to a verdict
fi
if [ $rc -eq 0 ] && [ "$printed_rc" = 0 ] && [ $cacheable = 1 ]; then
  for m in "$@"; do echo "$(modekey "$m") $m $(date +%F) $slot" >> "$green_log"; record_green "$m"; done
fi
log=$(printf '%s\n' "$out" | head -1)
case "$log" in
  "$root"/logs/*.log)
    mkdir -p build-remote
    ssh "${ssh_opts[@]}" "$host" "cat $log" 9>&- > "build-remote/$(basename "$log")" &&
      echo "remote_ci: full log in build-remote/$(basename "$log")" ;;
esac
exit $rc
