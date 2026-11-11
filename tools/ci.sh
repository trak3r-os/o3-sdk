#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/ci.sh <host|mk66> [mode...]: the CI of this repository, the same on a developer's machine and on the remote build machine
# (tools/remote_ci.sh; o3-sdk's docs/ci.md). The file is the same in o3-sdk and in an app repository that builds against it (o3-303):
# in an app, the framework is $O3SDK_DIR (default ../o3-sdk), passed to the configure as -DO3SDK_DIR.
#
#   host   configure build-ci-host, build, ctest --output-on-failure
#   mk66   configure build-ci-mk66 (-DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT), build, ctest (the images' static checks), then for every
#          image `arm-none-eabi-size` and the software doubles: __aeabi_d references left undefined in the archives (nm -u) and
#          __aeabi_d symbols linked into the images
#
# A run fails on a configure or build error, on any warning in the configure or build output (the count is printed; 0 is the only
# pass, the first-party targets are -Werror anyway), on a failing ctest or an empty test list, and in mk66 on any software double.
# Nothing is copied out of the build directory: CI produces no candidate image (those go through fw-gate-check and the maintainer).
#
# Environment:
#   ARM_GNU_ROOT       mk66: the Arm GNU Toolchain 15.3.Rel1 directory (the one that holds bin/); default: the one in .toolchains/
#                      (python3 -I tools/fetch_toolchain.py puts it there), else
#                      ../track3r-os/.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi
#   O3SDK_DIR          an app only: the o3sdk checkout to build against (default ../o3-sdk)
#   O3SDK_CI_TESTS     <regex>: only the ctests whose name matches (ctest -R). Letters, digits and _ . | ^ $ * + ? ( ) - only
#   O3SDK_CI_KEEP      1 (default): keep the build directory, so a run is incremental (it can hide a warning in a file that is up to
#                      date); 0: a fresh directory, which the release workflow, the gate and a batch's last run say explicitly
#   O3SDK_CI_SCOPE=changed  run only the tests of what changed since `git merge-base HEAD origin/main` plus the uncommitted and untracked
#                      files, mapped to labels (below); a full run when a CMake file, a shared header, an o3sdk change an app builds
#                      against or anything unmapped changed, or when there is no origin/main. O3SDK_CI_TESTS wins over it.
#   O3SDK_CI_SLOW=0    skip the tests labelled `slow` (ctest -LE slow); default 1
#   O3SDK_CI_JOBS      build and ctest parallelism; default min(8, max(2, threads - the 1-minute load)), so a busy machine gets fewer
#   O3SDK_CI_NICE=0    keep the normal priority (default: nice 10)
#   O3SDK_CI_CCACHE=0  do not compile through ccache (used when it is installed); CCACHE_BASEDIR is exported, the common parent of this
#                      checkout and the o3sdk it builds against, so every worktree of the same trees shares hits
#
# Scope mapping (O3SDK_CI_SCOPE=changed): a changed tests/*_test.cpp runs that ctest; app/, apps/, units/, extras/ and scenario files run
# every label but tools, docs, template and mk66; *.md and docs/ run the `docs` label; .claude/, .github/, LICENSE and the CI scripts
# run nothing. Everything else (CMake files, cmake/, kit/, sim/, platform/, third_party/, any include/ header, tools/*.py, ...) is a full run.
set -euo pipefail

die() { echo "ci.sh: $*" >&2; exit 1; }
need() { command -v "$1" > /dev/null 2>&1 || die "$1 not found"; }
usage() { echo "usage: tools/ci.sh <host|mk66> [mode...]   (see the comment at the top of this file)" >&2; exit 2; }

[ $# -ge 1 ] || usage
for m in "$@"; do
  case "$m" in host|mk66) ;; *) usage ;; esac
done
case "${O3SDK_CI_TESTS:-}" in
  *[!A-Za-z0-9_.\|^\$*+?\(\)-]*) echo "ci.sh: O3SDK_CI_TESTS may hold only letters, digits and _ . | ^ \$ * + ? ( ) -" >&2; exit 2 ;;
esac

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
# o3sdk itself, or an app that builds against it (add_subdirectory of $O3SDK_DIR).
if [ -f "$ROOT/cmake/O3sdkPlatform.cmake" ]; then
  KIND=sdk
  SDK=$ROOT
  SDK_ARGS=()
else
  KIND=app
  SDK=$(cd "${O3SDK_DIR:-$ROOT/../o3-sdk}" 2> /dev/null && pwd -P) || die "the o3sdk checkout '${O3SDK_DIR:-$ROOT/../o3-sdk}' does not exist (set O3SDK_DIR)"
  [ -f "$SDK/cmake/O3sdkPlatform.cmake" ] || die "$SDK is not an o3sdk checkout (no cmake/O3sdkPlatform.cmake; set O3SDK_DIR)"
  SDK_ARGS=("-DO3SDK_DIR=$SDK")
fi

# The cross toolchain: ARM_GNU_ROOT when given; else the one tools/fetch_toolchain.py unpacked into .toolchains/ (this checkout's, then
# o3sdk's, whichever host's archive is there); else track3r-os's, next to this checkout.
find_arm() {
  local d
  for d in "$ROOT"/.toolchains/arm-gnu-toolchain-15.3.rel1-*-arm-none-eabi "$SDK"/.toolchains/arm-gnu-toolchain-15.3.rel1-*-arm-none-eabi; do
    if [ -x "$d/bin/arm-none-eabi-gcc" ]; then echo "$d"; return; fi
  done
  echo "$ROOT/../track3r-os/.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi"
}
ARM_DIR=${ARM_GNU_ROOT:-$(find_arm)}
# Jobs: the machine is shared, so a busy one gets fewer: threads minus the 1-minute load, at least 2, at most 8 (16 only when one asks).
default_jobs() {
  local n load
  n=$(nproc 2> /dev/null || echo 4)
  load=$(cut -d' ' -f1 /proc/loadavg 2> /dev/null || echo 0)
  awk -v n="$n" -v l="${load:-0}" 'BEGIN { j = int(n - l); if (j < 2) j = 2; if (j > 8) j = 8; print j }'
}
JOBS=${O3SDK_CI_JOBS:-$(default_jobs)}
case "$JOBS" in ""|*[!0-9]*) die "O3SDK_CI_JOBS must be a number" ;; esac
if [ "${O3SDK_CI_NICE:-1}" = 1 ]; then renice -n 10 -p $$ > /dev/null 2>&1 || true; fi

# common_dir <a> <b>: the deepest directory that holds both (the first when they share only /).
common_dir() {
  local a=$1
  while [ -n "$a" ] && [ "$2" != "$a" ] && [ "${2#"$a"/}" = "$2" ]; do a=${a%/*}; done
  echo "${a:-$1}"
}

# ccache: every compile goes through it when it is installed, so a fresh build directory rebuilds from the cache. CCACHE_BASEDIR makes
# the paths under it relative: the common parent of this checkout and the o3sdk it builds against (for o3sdk alone, its parent), so another
# checkout or worktree of the same trees, and the app and o3sdk repositories side by side, hit the cache; the host build has -g, and the
# prefix map writes its debug paths relative to the same directory.
CCACHE_ARGS=()
HOST_ARGS=()
if [ "${O3SDK_CI_CCACHE:-1}" != 1 ]; then
  CCACHE_ARGS=(-DO3SDK_CCACHE=OFF)  # cmake/O3sdkPlatform.cmake would use it by itself
elif command -v ccache > /dev/null 2>&1; then
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  CCACHE_BASEDIR=$(common_dir "$ROOT" "$SDK")
  [ "$CCACHE_BASEDIR" != "$ROOT" ] || CCACHE_BASEDIR=$(dirname "$ROOT")
  export CCACHE_BASEDIR
  HOST_ARGS=("-DCMAKE_C_FLAGS=-fdebug-prefix-map=$CCACHE_BASEDIR=." "-DCMAKE_CXX_FLAGS=-fdebug-prefix-map=$CCACHE_BASEDIR=.")
fi

# scope_regex <dir>: the ctest -R regex (names, anchored) of the tests O3SDK_CI_SCOPE=changed selects, or "all" for a full run, or "none".
# Everything it says about the choice goes to stderr.
scope_regex() {
  local dir=$1 base f full=0 labels="" names="" docs=0 files n code=0
  base=$(git -C "$ROOT" merge-base HEAD origin/main 2> /dev/null) || { echo "== ci.sh: scope changed: no origin/main to take a merge-base with: full run" >&2; echo all; return; }
  files=$({ git -C "$ROOT" diff --name-only "$base" --; git -C "$ROOT" ls-files --others --exclude-standard; } | sort -u)
  if [ "$KIND" = app ] && [ -n "$(git -C "$SDK" diff HEAD --name-only 2> /dev/null)" ]; then
    echo "== ci.sh: scope changed: o3sdk has uncommitted changes the app builds against: full run" >&2; echo all; return
  fi
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    case "$f" in
      CMakeLists.txt|*/CMakeLists.txt|*.cmake|cmake/*|kit/*|sim/*|platform/*|third_party/*|*/include/*.h|*/include/*.hpp|tests/support/*|tests/main.cpp)
        full=1 ;;
      tests/*_test.cpp) n=${f##*/}; names="$names|${n%.cpp}(_[0-9]+)?" ;;
      app/*|apps/*|units/*|extras/*|tests/digests/*|tests/*) code=1 ;;
      .claude/*|.github/*|LICENSE*|.gitignore|tools/ci.sh|tools/remote_ci.sh|tools/o3sdkci.sh) ;;
      *.md|docs/*) docs=1 ;;
      *) full=1 ;;
    esac
    [ "$full" = 0 ] || { echo "== ci.sh: scope changed: $f is shared or unmapped: full run" >&2; echo all; return; }
  done <<< "$files"
  if [ "$code" = 1 ]; then
    labels=$(ctest --test-dir "$dir" --print-labels | sed -n 's/^  //p' | grep -Ev '^(tools|docs|template|mk66)$' | paste -sd'|')
    [ "$docs" = 0 ] || labels="$labels|docs"
  elif [ "$docs" = 1 ]; then
    labels=docs
  fi
  if [ -n "$labels" ]; then
    names="$names|$(ctest --test-dir "$dir" -N -L "^($labels)$" | sed -n 's/^ *Test *#[0-9]*: //p' | paste -sd'|')"
  fi
  names=${names#|}; names=${names%|}
  echo "== ci.sh: scope changed: $(printf '%s\n' "$files" | grep -c .) file(s) since $(git -C "$ROOT" rev-parse --short "$base"), labels: ${labels:-none}" >&2
  if [ -z "${names//|/}" ]; then echo none; else echo "^($names)\$"; fi
}

head_of() { git -C "$1" rev-parse --short=12 HEAD 2> /dev/null || echo "(not a git checkout)"; }

# run_mode <mode> <configure argument>...: configure, build, warnings, (mk66: images), ctest.
run_mode() {
  local mode=$1; shift
  local dir="$ROOT/build-ci-$mode"
  echo "== ci.sh $mode: $(basename "$ROOT") $(head_of "$ROOT")$([ "$KIND" = sdk ] || echo ", o3sdk $SDK at $(head_of "$SDK")"); $(cmake --version | sed -n 1p), $JOBS jobs${CCACHE_BASEDIR:+, ccache}"
  if [ "${O3SDK_CI_KEEP:-1}" != 1 ]; then rm -rf "$dir"; fi
  mkdir -p "$dir"

  echo "== ci.sh $mode: configure"
  cmake -S "$ROOT" -B "$dir" ${CCACHE_ARGS[@]+"${CCACHE_ARGS[@]}"} ${SDK_ARGS[@]+"${SDK_ARGS[@]}"} "$@" 2>&1 | tee "$dir/ci-configure.log" ||
    die "$mode: configure failed"
  echo "== ci.sh $mode: build"
  cmake --build "$dir" -j "$JOBS" 2>&1 | tee "$dir/ci-build.log" || die "$mode: build failed"

  # The first-party targets are -Werror; this also catches a warning of CMake, the linker or a target that opts out.
  local warnings
  warnings=$(cat "$dir/ci-configure.log" "$dir/ci-build.log" | grep -Eic 'warning[:(]|CMake (Deprecation )?Warning' || true)
  echo "== ci.sh $mode: warnings $warnings"
  if [ "$warnings" != 0 ]; then
    grep -nEi 'warning[:(]|CMake (Deprecation )?Warning' "$dir/ci-configure.log" "$dir/ci-build.log" >&2 || true
    die "$mode: $warnings warning(s) in the configure or build output (above)"
  fi

  if [ "$mode" = mk66 ]; then images "$dir"; fi

  local args=(--test-dir "$dir" --output-on-failure --no-tests=error -j "$JOBS") sel="" slow=()
  if [ "${O3SDK_CI_SLOW:-1}" = 0 ]; then slow=(-LE slow); fi
  if [ -n "${O3SDK_CI_TESTS:-}" ]; then
    sel=$O3SDK_CI_TESTS
  elif [ "${O3SDK_CI_SCOPE:-}" = changed ]; then
    sel=$(scope_regex "$dir")
    case "$sel" in
      all) sel="" ;;
      none) echo "== ci.sh $mode: scope changed: no test covers what changed; build only"; echo "== ci.sh $mode: OK"; return ;;
    esac
  fi
  if [ -n "$sel" ]; then args+=(-R "$sel"); fi
  args+=(${slow[@]+"${slow[@]}"})
  local total picked
  total=$(ctest --test-dir "$dir" -N | sed -n 's/^Total Tests: //p')
  picked=$(ctest "${args[@]}" -N | sed -n 's/^Total Tests: //p')
  echo "== ci.sh $mode: ctest${sel:+ -R '$sel'}${slow[0]+ -LE slow}: $picked of $total tests"
  ctest "${args[@]}" || die "$mode: ctest failed"
  echo "== ci.sh $mode: OK"
}

# images <build dir>: the size of every image and the software doubles (CLAUDE.md: no double; check with arm-none-eabi-nm).
images() {
  local dir=$1 size="$ARM_DIR/bin/arm-none-eabi-size" nm="$ARM_DIR/bin/arm-none-eabi-nm"
  local elf text data bss n linked=0 count=0 refs
  while IFS= read -r elf; do
    read -r text data bss _ < <("$size" "$elf" | sed -n 2p) || die "mk66: arm-none-eabi-size ${elf#"$dir"/} failed"
    n=$("$nm" "$elf" | grep -c ' __aeabi_d' || true)
    echo "== ci.sh mk66: image ${elf#"$dir"/}: text $text data $data bss $bss (flash text+data $((text + data))), __aeabi_d $n"
    linked=$((linked + n))
    count=$((count + 1))
  done < <(find "$dir" -name '*.elf' -not -path '*/CMakeFiles/*' | sort)
  [ "$count" != 0 ] || die "mk66: no image (*.elf) was built"
  refs=$(find "$dir" -name '*.a' -not -path '*/CMakeFiles/*' -print0 | xargs -0 -r "$nm" -A -u 2> /dev/null | grep '__aeabi_d' || true)
  n=$(printf '%s' "$refs" | grep -c . || true)
  echo "== ci.sh mk66: doubles: $n undefined __aeabi_d reference(s) in the archives, $linked __aeabi_d symbol(s) in the images"
  if [ "$n" != 0 ] || [ "$linked" != 0 ]; then
    [ -z "$refs" ] || printf '%s\n' "$refs" >&2
    die "mk66: software doubles (above; arm-none-eabi-nm <elf> | grep ' __aeabi_d' names the linked ones)"
  fi
}

case "${O3SDK_CI_SCOPE:-}" in ""|changed) ;; *) die "O3SDK_CI_SCOPE must be 'changed' (or unset)" ;; esac
case "${O3SDK_CI_SLOW:-1}" in 0|1) ;; *) die "O3SDK_CI_SLOW must be 0 or 1" ;; esac
need cmake; need ctest; need python3
for mode in "$@"; do
  case "$mode" in
    host)
      need c++
      echo "== ci.sh host: $(c++ --version | sed -n 1p)"
      run_mode host ${HOST_ARGS[@]+"${HOST_ARGS[@]}"}
      ;;
    mk66)
      [ -x "$ARM_DIR/bin/arm-none-eabi-gcc" ] || die "mk66: no arm-none-eabi-gcc in $ARM_DIR/bin (set ARM_GNU_ROOT to the Arm GNU Toolchain 15.3.Rel1 directory, or run python3 -I tools/fetch_toolchain.py)"
      echo "== ci.sh mk66: $("$ARM_DIR/bin/arm-none-eabi-gcc" --version | sed -n 1p)"
      run_mode mk66 -DO3SDK_PLATFORM=mk66 "-DARM_GNU_ROOT=$ARM_DIR"
      ;;
  esac
done
