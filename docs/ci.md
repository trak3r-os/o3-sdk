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
- Every run starts from a fresh build directory. `O3SDK_CI_KEEP=1` keeps it (incremental, but a warning in a file that is up to date
  is not printed again).
- `O3SDK_CI_TESTS=<regex>` runs only the ctests whose name matches (`ctest -R`); letters, digits and `_ . | ^ $ * + ? ( ) -` only.
  A regex that matches nothing fails.
- `ARM_GNU_ROOT` is the Arm GNU Toolchain 15.3.Rel1 directory (default `../track3r-os/.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi`).
  `O3SDK_CI_JOBS` (default 4), `O3SDK_CI_NICE=0` (default: nice 10), `O3SDK_CI_CCACHE=0` (default: ccache when it is installed).

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
  incremental; `dev` and `gate` start fresh. One run per slot at a time: a second waits. `O3SDK_CI_TESTS` and `O3SDK_CI_KEEP=0|1`
  are passed on.

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
