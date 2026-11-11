# CI

Two scripts, the same files in o3-sdk and in an app repository that builds against it (o3-303): `tools/ci.sh` runs the checks,
`tools/remote_ci.sh` runs them on the remote build machine. There is no hosted CI. Nothing here flashes or copies an image out of
its build directory: candidates go through `fw-gate-check` and the maintainer.

## `tools/ci.sh <host|mk66> [mode...]`

| Mode | Build directory | What runs |
|---|---|---|
| `host` | `build-ci-host` | configure, build, `ctest --output-on-failure` |
| `mk66` | `build-ci-mk66` | configure with `-DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT`, build, ctest (the images' static checks), then `arm-none-eabi-size` of every image and the software doubles |

- A run fails on a configure or build error, on any warning in the configure or build output (it prints `warnings <n>`: 0 is the
  only pass), on a failing ctest or an empty test list, and in `mk66` on any software double: an `__aeabi_d*` reference left
  undefined in an archive (`nm -u`) or linked into an image.
- In an app, the framework is `$O3SDK_DIR` (default `../o3-sdk`), passed to the configure as `-DO3SDK_DIR`. The script tells the two
  apart by `cmake/O3sdkPlatform.cmake`.
- A run keeps its build directory (`O3SDK_CI_KEEP=1`, the default: incremental, but a warning in a file that is up to date is not
  printed again). `O3SDK_CI_KEEP=0` starts fresh; the release workflow, the gate and the last run of a batch say it explicitly.
- `O3SDK_CI_TESTS=<regex>` runs only the ctests whose name matches (`ctest -R`); letters, digits and `_ . | ^ $ * + ? ( ) -` only.
  A regex that matches nothing fails.
- `ARM_GNU_ROOT` is the Arm GNU Toolchain 15.3.Rel1 directory (default `../track3r-os/.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi`).
  `O3SDK_CI_JOBS` (default: threads minus the 1-minute load, at least 2, at most 8), `O3SDK_CI_NICE=0` (default: nice 10),
  `O3SDK_CI_CCACHE=0` (default: ccache when it is installed).

## Faster iteration: labels, scope, jobs

- Every ctest of `o3sdk_test_suite()` has an **area label**: `AREA <label>` or else the suite's name (`o3_303_tests`), and **`slow`** when
  its source name has "scenario" in it or `SLOW <test>...` names it. `o3sdk_test_labels(<test>... AREA <label> [SLOW])` labels a test
  registered by hand. `ctest -L <area>`, `ctest -N -L <area>` (the list) and `ctest -LE slow` work in any build directory.
- **`timing`** marks a test that measures real time (o3-303's `midi_check_selftest`: the MIDI clock's lock and tempo against a fake console, 298 s, and
  13 to 26 ms of lateness fails it when the machine is loaded). It is also `slow`, so `O3SDK_CI_SLOW=0` skips it, and `ctest -LE timing` leaves out
  any such test. **Timing tests run only on a quiet machine or on the remote**; a failure of one at a load of 16 or more is the load, until a rerun on a quiet
  machine says otherwise. A test that measures time gets `LABELS "<area>;slow;timing"` (`o3sdk_test_labels(<test> AREA <area> SLOW)` does not add `timing`).
- `O3SDK_CI_TESTS=<regex>` (`ctest -R`) is the narrowest run: the one test you touched. `O3SDK_CI_SLOW=0` leaves the `slow` label out.
- `O3SDK_CI_SCOPE=changed` maps the files changed since the **base** to tests (the table is at the top of `tools/ci.sh`): `tests/<x>_test.cpp`
  runs that ctest, app, unit and scenario files every label but tools, docs, template and mk66, markdown and `docs/` the `docs` label,
  `.claude/` and the CI scripts nothing. A CMake file, a shared header (`include/`), `kit/`, `sim/`, `platform/`, anything unmapped, or no
  base at all, is a full run. When the files map to no test, the run builds and stops ("no test covers what changed").
- **The base**, in order: `O3SDK_CI_BASE=<rev>`; else the **green ref** `refs/ci/green/<mode>`; else `git merge-base HEAD origin/main`; else a full
  run. Changed files are those that differ from the base in the working tree (so a change reverted since the green run counts, and a
  deleted file does), plus the untracked files that are not ignored. One line says which base was used. In an app, a recorded green base also
  holds the o3sdk tree (`refs/ci/green/<mode>-sdk`): a different or missing o3sdk tree is a full run, and with the other bases an uncommitted
  change in the o3sdk is.
- **The green ref.** After a full run that passes (`rc=0`, no `O3SDK_CI_TESTS`, no `O3SDK_CI_SCOPE` filter, `O3SDK_CI_SLOW` not 0), `ci.sh` records
  the exact tree it tested, taken before the build, as a snapshot commit and points `refs/ci/green/<mode>` at it, only if the tree is unchanged
  when the run ends (otherwise "no green ref"). A local run records itself; `remote_ci.sh` points the local ref at the snapshot it pushed, on a
  green run and on a cache hit (the remote runs with `O3SDK_CI_GREEN=0`). The refs live in the git common dir, so worktrees share them, and
  they keep the snapshot objects from `git gc`. `O3SDK_CI_GREEN=0` records nothing. The shared code is in `ci.sh`: `ci.sh --snapshot <dir> <sdk>`,
  `--scope <dir> <mode>` and `--record-green <mode> <commit> [sdk-tree]` are its helpers for `remote_ci.sh` (not for hands).
- `O3SDK_CI_TESTS` takes letters, digits and `_ . | ^ $ * + ? ( ) [ ] -` only.
- Habits. **One run per checkout at a time** (two at once take 2 to 3 times as long each). **Filtered runs while iterating**
  (`O3SDK_CI_TESTS`, `O3SDK_CI_SCOPE=changed`, `ctest -L`); **one full run per batch**, not per item, the last one with
  `O3SDK_CI_KEEP=0` when the batch ends in a release or a push. **Jobs are the machine's threads divided by the runs that go at once**
  (the default does it from the load; set `O3SDK_CI_JOBS` when two lanes are known to run). **Never invent a remote slot name**: a
  new slot builds cold; use the checkout's own (the default) or `dev` and `gate`.
- **`COST`**: a test of several minutes gets `set_tests_properties(<test> PROPERTIES COST <seconds>)` (an uncontended guess), so ctest starts the
  longest first even in a fresh build directory, where it has no timing history. A file with many test cases is cut into shares with
  `O3SDK_TEST_SHARD=i/n` (`tests/main.cpp`), and o3-303's `o3_303_shard_coverage` proves that the shares together hold each case once.
- ccache is on by default for every build (`O3SDK_CCACHE=OFF` to compile directly; `cmake/O3sdkPlatform.cmake`). `tools/ci.sh` exports
  `CCACHE_BASEDIR`, the common parent of the checkout and the o3sdk it builds against, so worktrees and the apps beside o3-sdk share hits.

## Remote CI: `tools/remote_ci.sh [--slot <name>] <host|mk66> [mode...]`

- The first choice for a full run while the machine is up. It sends the working tree as it is, committed or not: a snapshot commit
  on top of HEAD made with a temporary index (the branch, the index and the tree are untouched), pushed to `refs/ci/<sha>` of a bare
  repository on the machine (a new ref per commit, never forced). From an app it also sends the o3sdk tree it builds against
  (`$O3SDK_DIR`, default `../o3-sdk`) the same way, and the run builds exactly those two trees.
- It prints the warning, image and double lines, the ctest summary and `rc=<n> secs=<n>`; the full log comes back to
  `build-remote/` (untracked). Its exit code is the run's.
- **Exit 3** (`O3SDK_REMOTE_CI` not set) or **255** (ssh failed: the machine is off or unreachable): run `tools/ci.sh` locally,
  under `nice -n 10`. Exit 2 is a usage error.
- The machine is named by `O3SDK_REMOTE_CI=<ssh-host>:<root>`, set locally only, in the `env` block of
  `.claude/settings.local.json` (git-ignored; the key is in `.claude/settings.local.json.example`, empty). The host, the user and
  the path never go into git.
- `--slot` names the remote worktree (default: the checkout's folder name). A slot keeps its build directories, so its runs are
  incremental; `dev` and `gate` start fresh. One run per slot at a time: a second waits (a `flock` on
  `<git common dir>/remote-ci-slot-<slot>.lock`, shared by the worktrees, held for the whole run and not inherited by ssh). If the push of
  `refs/ci/<sha>` fails, `ls-remote` is checked and the run goes on when the ref already holds the sha. `O3SDK_CI_TESTS`, `O3SDK_CI_KEEP=0|1`
  and `O3SDK_CI_SLOW=0|1` are passed on, `O3SDK_REMOTE_JOBS` as `O3SDK_CI_JOBS`; `O3SDK_CI_SCOPE` is not passed (see the next item).
- **Green cache.** After `rc=0` of a full run (no `O3SDK_CI_TESTS`), `<git common dir>/ci-green.log` gets the line `<key> <mode> <date> <slot>`; the key
  is the sha1 of the snapshot tree (an app: its tree and the o3sdk tree it builds against), the mode and the environment sent to the machine (not the job
  count). A later run of the same trees skips a mode whose key is there ("already green for this tree") and exits 0 without ssh when all are skipped.
  A filtered run is never cached. `O3SDK_CI_FORCE=1` runs anyway: use it after changing the machine (toolchain, wrapper), which the key does not cover.
- **`O3SDK_CI_SCOPE=changed`** is worked out locally, with the same code as `ci.sh` (the base above, the test names of the configured
  `build-ci-host` or `build-host`, else a full run), once per mode, and the result is what is sent: `none` for every mode sends nothing and exits 0 with
  the reason; `all` (any mode) is a normal full run, cacheable and green-recorded; otherwise the union of the modes' regexes goes as
  `O3SDK_CI_TESTS` (a filtered run: not cached, no green ref; more than 30000 characters is a full run). A mode whose own scope is `none`
  in such a run goes in `O3SDK_CI_BUILD_ONLY`: it builds (warnings and mk66's image checks included) and runs no ctest, so another mode's
  regex never reaches a ctest with no test of its own (before, `host mk66` in one call ended rc 1 on "No tests were found").
- **Evidence for `verify_gate.py`.** When the machine ran the tests to an rc (the `tests passed|tests failed` line and `rc=` came back),
  `remote_ci.sh` touches `<git common dir>/ci-remote-last-run`; exit 3 and 255 do not. The Stop hook counts the newer of that file and the
  newest `build*/Testing/Temporary/LastTest.log` as "ctest ran after your last edit", so a remote run satisfies it; its nudge names
  `tools/remote_ci.sh host` with `O3SDK_CI_SCOPE=changed` or `O3SDK_CI_TESTS` as the check to prefer while RAM is short.
- **Jobs by load.** With `O3SDK_REMOTE_JOBS` unset the run asks the machine how many of its four turn locks are busy and sends
  `O3SDK_CI_JOBS=nproc/(busy+1)`, at least 4.

## The machine

- `<root>` holds `sdk.git` and `app.git` (bare), `o3sdkci.sh` (installed from `tools/o3sdkci.sh`, the reference copy: reinstall it
  after a change), `logs/`, `locks/`, `ccache/` and the worktrees: `sdk/<slot>/o3-sdk`, and for an app `app/<slot>/app` with the
  o3sdk tree beside it in `app/<slot>/o3-sdk`.
- Turns: a run takes the first free of four turn locks and runs with 4 jobs at nice 10; a fifth waits. On the current machine the
  turns are track3r-os's CI's own (`O3SDKCI_TURNS` in `<root>/o3sdkci.conf`), so both projects share the same 16 threads instead
  of oversubscribing them; those locks are opened read-only. The toolchain is the one already on the machine (`O3SDKCI_ARM`).
- ccache is this CI's own, `<root>/ccache` (5 GB): the other CI's cache is full, evicts constantly and is not ours to write.
- A new machine: `git init --bare` the two repositories, copy `tools/o3sdkci.sh` to `<root>/`, unpack the toolchain into
  `<root>/toolchains/` (or name it in `o3sdkci.conf`), and set `O3SDK_REMOTE_CI` in each checkout's `.claude/settings.local.json`.

## The release gate: `tools/gate.py <app>`

CI checks the tree; the gate checks one **image** before the maintainer sees it (skill `fw-gate-check`, `docs/NEW-APP.md` section 10). The same file
is in o3-sdk and in an app repository. `python3 -I tools/gate.py <app> --host-build <host build dir>` builds the image twice from scratch into
`build-gate-a` and `build-gate-b`, checks that neither build warns, overflows a region or leaves an undefined reference, that the two HEX files are
byte-identical, that the app's image ctests (`ctest -L mk66`: `_image`, `_flashconfig`, `_ramfunc`, `_usb`) pass, that no software double is in the
image or the archives, and that the host ctest of the same code passes; it writes `build-gate-a/CANDIDATE.md` (identity of both trees, every check,
the link's region usage, sizes and SHA-256 of `.hex`, `.ptf`, `.bin`, `.elf`, the label `version` will print on the unit, what the host cannot prove).
Verdicts: `CANDIDATE`, `BENCH BUILD` (a tree has changed tracked files or no commit, so the image says `-dirty` or `unknown` and cannot be traced:
the maintainer commits first) or `FAIL` (exit 1). `--skip-host` leaves the host ctest out and says so in the manifest. It takes about 40 s for a
small app; it never flashes, writes to a card or touches a unit, and writes only in its two build directories.
