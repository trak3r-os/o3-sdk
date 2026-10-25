#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# o3sdkci.sh <sdk|app> <slot> <ref> [<sdk-ref>] <host|mk66> [mode...]: the remote build machine's side of tools/remote_ci.sh
# (docs/ci.md). This file is the reference copy; the machine runs it installed as <root>/o3sdkci.sh, beside the bare repositories
# sdk.git (o3-sdk) and app.git (the app, o3-303). Reinstall it there after changing it.
#
#   sdk  checks <ref> of sdk.git out in <root>/sdk/<slot>/o3-sdk and runs its tools/ci.sh <mode>...
#   app  checks <ref> of app.git out in <root>/app/<slot>/app and <sdk-ref> of sdk.git beside it in <root>/app/<slot>/o3-sdk (the
#        sibling layout of a developer's checkouts) and runs the app's tools/ci.sh <mode>... with O3SDK_DIR set to that o3-sdk.
#
# One run per slot at a time (<root>/locks/<kind>-<slot>.lock). A slot keeps its build directories (O3SDK_CI_KEEP=1, unless the
# caller passes 0); the slots `dev` and `gate` start fresh. The run takes the first free of four turn locks <turns>.0 to <turns>.3
# (a fifth run waits for one) and runs with 4 jobs at nice 10. <root>/o3sdkci.conf, on the machine only, may set:
#   O3SDKCI_TURNS   the turn lock prefix (default <root>/.turn). Pointing it at another CI's locks on the same machine makes the two
#                   share the same four turns instead of oversubscribing it; existing locks are opened read-only, nothing is written
#   O3SDKCI_ARM     the Arm GNU Toolchain 15.3.Rel1 directory (default <root>/toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi)
#   O3SDKCI_CCACHE  the ccache directory (default <root>/ccache)
# Prints the log path (<root>/logs/), the summary lines of tools/ci.sh (warnings, images, doubles, ctest, OK or the failure) and
# `rc=<n> secs=<n>`; exits with the run's code (2: bad usage or a failed checkout).
set -u
R=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
O3SDKCI_TURNS=$R/.turn
O3SDKCI_ARM=$R/toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi
O3SDKCI_CCACHE=$R/ccache
if [ -f "$R/o3sdkci.conf" ]; then . "$R/o3sdkci.conf"; fi

usage() { echo "usage: o3sdkci.sh <sdk|app> <slot> <ref> [<sdk-ref>] <host|mk66> [mode...]" >&2; exit 2; }
[ $# -ge 4 ] || usage
kind=$1 slot=$2 ref=$3 sdkref=
shift 3
case "$kind" in
  sdk) ;;
  app) sdkref=$1; shift ;;
  *) usage ;;
esac
[ $# -ge 1 ] || usage
case "$slot" in ""|.*|*[!A-Za-z0-9._-]*) usage ;; esac
for r in "$ref" ${sdkref:+"$sdkref"}; do
  case "$r" in ""|*[!0-9a-f]*) usage ;; esac
done
for m in "$@"; do
  case "$m" in host|mk66) ;; *) usage ;; esac
done

# checkout <bare repository> <worktree> <ref>
checkout() {
  if [ ! -d "$2" ]; then
    git -C "$1" worktree prune   # a worktree whose folder was deleted by hand is still registered
    mkdir -p "$(dirname "$2")" && git -C "$1" worktree add -q --detach "$2" "$3" || return 2
  fi
  git -C "$2" checkout -q --detach "$3"
}

# turn <command>...: runs the command in the first free turn, waiting for one.
turn() {
  local n fd rc
  while :; do
    for n in 0 1 2 3; do
      if [ ! -e "$O3SDKCI_TURNS.$n" ]; then
        # Only this CI's own locks are created; another CI's are its own business.
        [ "$(dirname "$O3SDKCI_TURNS")" = "$R" ] && : >> "$O3SDKCI_TURNS.$n" || { echo "o3sdkci: no turn lock $O3SDKCI_TURNS.$n"; return 2; }
      fi
      exec {fd}< "$O3SDKCI_TURNS.$n" || return 2
      if flock -n "$fd"; then
        echo "== o3sdkci: turn $n, $(date '+%F %T')"
        nice -n 10 "$@"
        rc=$?
        exec {fd}<&-
        return $rc
      fi
      exec {fd}<&-
    done
    sleep 5
  done
}

run() {
  local wt
  echo "== o3sdkci: $kind slot $slot, ref $ref${sdkref:+, o3-sdk $sdkref}, modes $*, $(date '+%F %T')"
  exec {slotfd}>> "$R/locks/$kind-$slot.lock" || return 2
  flock "$slotfd" || return 2
  case "$kind" in
    sdk)
      wt=$R/sdk/$slot/o3-sdk
      checkout "$R/sdk.git" "$wt" "$ref" || return 2
      unset O3SDK_DIR
      ;;
    app)
      wt=$R/app/$slot/app
      checkout "$R/app.git" "$wt" "$ref" || return 2
      checkout "$R/sdk.git" "$R/app/$slot/o3-sdk" "$sdkref" || return 2
      export O3SDK_DIR=$R/app/$slot/o3-sdk
      ;;
  esac
  export ARM_GNU_ROOT=$O3SDKCI_ARM CCACHE_DIR=$O3SDKCI_CCACHE O3SDK_CI_JOBS=4
  case "$slot" in
    dev|gate) export O3SDK_CI_KEEP=0 ;;
    *) export O3SDK_CI_KEEP=${O3SDK_CI_KEEP:-1} ;;
  esac
  cd "$wt" || return 2
  turn tools/ci.sh "$@"
}

mkdir -p "$R/logs" "$R/locks" || exit 2
log=$R/logs/$kind-$slot-${ref:0:12}-$(echo "$*" | tr ' ' '+')-$(date +%Y%m%d-%H%M%S).log
start=$(date +%s)
run "$@" > "$log" 2>&1
rc=$?
echo "rc=$rc secs=$(($(date +%s) - start))" >> "$log"
echo "$log"
grep -E '^== ci\.sh [a-z0-9]+: (warnings|image|doubles|OK)|tests passed|tests failed|^ci\.sh: |^o3sdkci: ' "$log" | tail -20
tail -1 "$log"
exit $rc
