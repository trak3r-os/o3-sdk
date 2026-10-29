#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the MK66 linker script from mem_map.h, the single source of the memory map (stdlib only).

    gen_ld.py generate --header mem_map.h --template mk66.ld.in --out mk66.ld
    gen_ld.py check    --header mem_map.h --template mk66.ld.in [--ld mk66.ld]
    gen_ld.py --self-test

`generate` fills every @kName@ in the template with the value of `inline constexpr std::uint32_t kName = ...;` in the
header. The expressions are evaluated with a whitelist (integer literals with u/U/l/L suffixes and ' separators, earlier
constants, + - * / % << >> & | ^ and parentheses), in 32-bit unsigned arithmetic as C++ does: a value that would wrap is an
error, so is a placeholder with no constant, a leftover placeholder, or a statement in the header that matches the pattern
but cannot be evaluated. `check` generates in memory (or reads --ld) and verifies the result against the header
independently: the MEMORY regions, no overlaps, the SRAM_L hot-code budget, and the SDRAM layout (`.sdram_bss` NOLOAD at
the base of the SDRAM, the arena's symbols behind it). ctest runs it as `mk66_ld`; the image build runs `generate`.
"""
import argparse
import ast
import re
import sys
import tempfile
from pathlib import Path

U32 = 1 << 32

CONSTANT = re.compile(r"inline\s+constexpr\s+std::uint32_t\s+(k\w+)\s*=\s*([^;]+);")
NUMBER = re.compile(r"\b(0[xX][0-9A-Fa-f']+|[0-9][0-9']*)[uUlL]*\b")
PLACEHOLDER = re.compile(r"@(k\w+)@")


class GenError(Exception):
    pass


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def evaluate(name, expr, known):
    """Evaluates one C++ constant expression with the whitelist above."""
    source = NUMBER.sub(lambda m: str(int(m.group(1).replace("'", ""), 0)), expr.strip())
    try:
        tree = ast.parse(source, mode="eval")
    except SyntaxError as exc:
        raise GenError("%s: cannot parse '%s' (%s)" % (name, expr.strip(), exc.msg))

    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int) and not isinstance(node.value, bool):
            return node.value
        if isinstance(node, ast.Name):
            if node.id not in known:
                raise GenError("%s: '%s' is not an earlier constant" % (name, node.id))
            return known[node.id]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.UAdd):
            return walk(node.operand)
        if isinstance(node, ast.BinOp):
            a, b = walk(node.left), walk(node.right)
            op = type(node.op)
            if op is ast.Add:
                return a + b
            if op is ast.Sub:
                return a - b
            if op is ast.Mult:
                return a * b
            if op in (ast.Div, ast.FloorDiv):
                if b == 0:
                    raise GenError("%s: division by zero" % name)
                return a // b
            if op is ast.Mod:
                if b == 0:
                    raise GenError("%s: modulo by zero" % name)
                return a % b
            if op is ast.LShift:
                return a << b
            if op is ast.RShift:
                return a >> b
            if op is ast.BitAnd:
                return a & b
            if op is ast.BitOr:
                return a | b
            if op is ast.BitXor:
                return a ^ b
        raise GenError("%s: '%s' uses something outside the allowed expression grammar" % (name, expr.strip()))

    value = walk(tree)
    if not 0 <= value < U32:
        raise GenError("%s = %d does not fit std::uint32_t (C++ would wrap it)" % (name, value))
    return value


def parse_header(text):
    """Returns {name: value} for every uint32 constant in the header, in order."""
    constants = {}
    for m in CONSTANT.finditer(strip_comments(text)):
        name = m.group(1)
        if name in constants:
            raise GenError("%s is defined twice" % name)
        constants[name] = evaluate(name, m.group(2), constants)
    if not constants:
        raise GenError("no 'inline constexpr std::uint32_t kName = ...;' constants found: wrong file?")
    return constants


def render(template, constants):
    missing = sorted({n for n in PLACEHOLDER.findall(template) if n not in constants})
    if missing:
        raise GenError("template placeholders without a constant in the header: %s" % ", ".join(missing))
    out = PLACEHOLDER.sub(lambda m: "0x%08X" % constants[m.group(1)], template)
    if "@k" in out:
        raise GenError("a placeholder was left unreplaced")
    return out


def memory_regions(script):
    """The MEMORY block of a linker script: {region: (origin, length)}."""
    regions = {}
    for m in re.finditer(r"^\s*(\w+)\s*\([rwx]+\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*LENGTH\s*=\s*(0x[0-9A-Fa-f]+)", script, re.M):
        regions[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return regions


def verify(constants, script):
    """Independent checks of a generated script against the header. Returns a list of problems."""
    problems = []
    regions = memory_regions(script)
    expect = {
        "FLASH": (constants["kFlashBase"], constants["kFlashSize"]),
        "SRAM_L": (constants["kSramLBase"], constants["kSramLSize"]),
        "SRAM_U": (constants["kSramUBase"], constants["kSramUSize"]),
        "SDRAM": (constants["kSdramBase"], constants["kSdramSize"]),
    }
    for name, want in expect.items():
        got = regions.get(name)
        if got is None:
            problems.append("MEMORY has no region %s" % name)
        elif got != want:
            problems.append("MEMORY %s is origin 0x%08X length 0x%X, mem_map.h says origin 0x%08X length 0x%X" % ((name,) + got + want))
    extra = sorted(set(regions) - set(expect))
    if extra:
        problems.append("MEMORY has regions the header does not describe: %s" % ", ".join(extra))

    # No overlap between regions, and each inside the chip's address range it belongs to.
    spans = sorted((o, o + l, n) for n, (o, l) in regions.items())
    for (a0, a1, an), (b0, b1, bn) in zip(spans, spans[1:]):
        if b0 < a1:
            problems.append("regions %s and %s overlap" % (an, bn))
    if expect["FLASH"][0] + expect["FLASH"][1] > 0x00100000:
        problems.append("FLASH extends past 0x000FFFFF (the K66's 1 MiB program flash)")
    if not (0x1FFF0000 <= constants["kSramLBase"] and constants["kSramUBase"] + constants["kSramUSize"] <= 0x20030000):
        problems.append("SRAM regions outside 0x1FFF0000..0x2002FFFF")
    if not (0x70000000 <= constants["kSdramBase"] and constants["kSdramBase"] + constants["kSdramSize"] <= 0x71000000):
        problems.append("SDRAM outside the controller's 16 MiB block at 0x70000000")
    if constants["kRamfuncLBudget"] > constants["kSramLSize"] or constants["kRamfuncLBudget"] % 4:
        problems.append("SRAM_L: the hot-code budget kRamfuncLBudget does not fit SRAM_L in whole words")
    if constants["kSramLFreeBase"] != constants["kSramLBase"] + constants["kRamfuncLBudget"]:
        problems.append("SRAM_L: kSramLFreeBase is not where the hot-code budget ends")
    if constants["kSdramArenaAlign"] % 16 or constants["kSdramBase"] % constants["kSdramArenaAlign"]:
        problems.append("SDRAM: kSdramArenaAlign is not a whole number of cache lines dividing the SDRAM base")

    # The SDRAM layout: `.sdram_bss` NOLOAD in the SDRAM region, the arena's symbols, the overflow assert.
    if not re.search(r"^\s*\.sdram_bss\s+\(NOLOAD\)", script, re.M):
        problems.append("the script has no NOLOAD .sdram_bss output section (O3SDK_BIG objects would be loaded or orphaned)")
    if not re.search(r"\}\s*>\s*SDRAM\b", script):
        problems.append("nothing is placed in the SDRAM region")
    for symbol in ("__sdram_bss_start", "__sdram_bss_end", "__sdram_arena_start", "__sdram_arena_end"):
        if symbol not in script:
            problems.append("the script does not define %s" % symbol)

    # O3SDK_BIG (kit/core placement.h) puts its objects in the input section `.bss.sdram_bss`, so that GCC refuses a non-zero
    # initialiser on them. The `.sdram_bss` output section must collect exactly that name, and (like `.noinit` and `.dma_rings`) be
    # listed before the `.bss` output section, whose `.bss.*` pattern would otherwise take the input section into SRAM_U.
    body = re.search(r"^\s*\.sdram_bss\s+\(NOLOAD\)[^{]*\{(.*?)^\s*\}", script, re.M | re.S)
    if body and not re.search(r"\*\(\s*\.bss\.sdram_bss[\s)]", body.group(1)):
        problems.append("the .sdram_bss output section does not collect the input section .bss.sdram_bss (what O3SDK_BIG uses)")
    bss = re.search(r"^\s*\.bss\s*:", script, re.M)
    for name in (".noinit", ".dma_rings", ".sdram_bss"):
        first = re.search(r"^\s*%s\s*(\(NOLOAD\))?\s*:" % re.escape(name), script, re.M)
        if first and bss and first.start() > bss.start():
            problems.append("the %s output section is listed after .bss: the .bss.* pattern of .bss would take its input section" % name)

    for needle in ("ORIGIN(SRAM_U) + LENGTH(SRAM_U)", "ASSERT("):
        if needle not in script:
            problems.append("the script lost '%s'" % needle)
    if "@k" in script:
        problems.append("a placeholder is left in the script")
    return problems


def cmd_generate(args):
    constants = parse_header(Path(args.header).read_text(encoding="utf-8"))
    script = render(Path(args.template).read_text(encoding="utf-8"), constants)
    problems = verify(constants, script)
    if problems:
        raise GenError("generated script fails its checks:\n  " + "\n  ".join(problems))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(script, encoding="utf-8")
    print("gen_ld: wrote %s (%d constants from %s)" % (out, len(constants), args.header))
    return 0


def cmd_check(args):
    constants = parse_header(Path(args.header).read_text(encoding="utf-8"))
    script = render(Path(args.template).read_text(encoding="utf-8"), constants)
    if args.ld:
        on_disk = Path(args.ld).read_text(encoding="utf-8")
        if on_disk != script:
            raise GenError("%s is not what the header and template generate: rebuild it" % args.ld)
        script = on_disk
    problems = verify(constants, script)
    if problems:
        raise GenError("\n  ".join(["mk66 linker script check failed:"] + problems))
    print("gen_ld: check ok (%d constants, %d regions%s)" % (len(constants), len(memory_regions(script)), ", file matches" if args.ld else ""))
    return 0


def move_behind_bss(script, name):
    """Self-test helper: the script with the output section `name` listed behind `.bss` (the order verify() refuses)."""
    section = r"^  %s\b[^\n]*\n  \{.*?^  \}[^\n]*\n"
    block = re.search(section % re.escape(name), script, re.M | re.S)
    cut = script[:block.start()] + script[block.end():]
    bss = re.search(section % re.escape(".bss"), cut, re.M | re.S)
    return cut[:bss.end()] + block.group(0) + cut[bss.end():]


def self_test():
    failures = []

    def expect(label, condition):
        if not condition:
            failures.append(label)

    def raises(label, fn):
        try:
            fn()
        except GenError:
            return
        failures.append(label + " (no error)")

    # Expression evaluation: C++ literals and operators, in uint32 arithmetic.
    expect("hex and suffix", evaluate("x", "0x1FFF0000u", {}) == 0x1FFF0000)
    expect("digit separators", evaluate("x", "1'000'000u", {}) == 1000000)
    expect("names and shifts", evaluate("x", "(a + 4u) << 2u", {"a": 1}) == 20)
    expect("integer division", evaluate("x", "7u / 2u", {}) == 3)
    expect("precedence", evaluate("x", "2u + 3u * 4u", {}) == 14)
    expect("kKiB style", evaluate("x", "192u * kKiB", {"kKiB": 1024}) == 196608)
    raises("wrap is an error", lambda: evaluate("x", "0u - 1u", {}))
    raises("too big is an error", lambda: evaluate("x", "65536u * 65536u", {}))
    raises("unknown name", lambda: evaluate("x", "nope + 1u", {}))
    raises("call is not allowed", lambda: evaluate("x", "f(1u)", {}))
    raises("string is not allowed", lambda: evaluate("x", "'a'", {}))
    raises("float is not allowed", lambda: evaluate("x", "1.5", {}))
    raises("attribute access is not allowed", lambda: evaluate("x", "a.b", {"a": 1}))
    raises("division by zero", lambda: evaluate("x", "1u / 0u", {}))

    # A header in the real shape, and the real template filled from it.
    root = Path(__file__).resolve().parent.parent
    header = root / "include/o3sdk/platform/mk66/mem_map.h"
    template = root / "mk66.ld.in"
    constants = parse_header(header.read_text(encoding="utf-8"))
    pins = {  # independent copies of the facts: the chip (K66 RM memory map), ADR 0013 D7 (SRAM_U) and the kit's map (docs/M2-PLAN.md, "Memory")
        "kFlashSize": 1 << 20, "kSramLBase": 0x1FFF0000, "kSramLSize": 64 << 10, "kSramUBase": 0x20000000,
        "kSramUSize": 192 << 10, "kSdramBase": 0x70000000, "kSdramSize": 16 << 20, "kSramUMinFree": 32 << 10,
        "kRamfuncLBudget": 32 << 10, "kSramLFreeBase": 0x1FFF8000, "kSramLDataBudget": 20 << 10, "kDmaRingAlign": 1024, "kSdramArenaAlign": 32,
        "kFlashConfigOffset": 0x400, "kStackTop": 0x20030000, "kStackSize": 24 << 10,
    }
    for name, value in pins.items():
        expect("%s is 0x%X" % (name, value), constants.get(name) == value)
    script = render(template.read_text(encoding="utf-8"), constants)
    expect("real script verifies", verify(constants, script) == [])
    expect("real script has the SRAM_L region", memory_regions(script).get("SRAM_L") == (0x1FFF0000, 64 << 10))
    expect("real script has the SDRAM region", memory_regions(script).get("SDRAM") == (0x70000000, 16 << 20))

    # Drift is caught: break the generated script and the header one thing at a time.
    expect("wrong origin is caught", any("FLASH" in p for p in verify(constants, script.replace("ORIGIN = 0x00000000", "ORIGIN = 0x00001000"))))
    expect("wrong length is caught", any("SRAM_U" in p for p in verify(constants, re.sub(r"(SRAM_U\s+\(rw\)\s*:\s*ORIGIN = 0x20000000, LENGTH = )0x[0-9A-F]+", r"\g<1>0x00020000", script))))
    expect("extra region is caught", any("describe" in p for p in verify(constants, script.replace("SDRAM  (rw)", "OTHER (rw) : ORIGIN = 0x71000000, LENGTH = 0x10\n  SDRAM  (rw)"))))
    expect("overlap is caught", any("overlap" in p for p in verify(constants, script.replace("ORIGIN = 0x20000000", "ORIGIN = 0x1FFF8000"))))
    expect("a loaded .sdram_bss is caught", any("NOLOAD .sdram_bss" in p for p in verify(constants, script.replace(".sdram_bss (NOLOAD)", ".sdram_bss"))))
    expect("an O3SDK_BIG section the script does not collect is caught", any(".bss.sdram_bss" in p for p in verify(constants, script.replace("*(.bss.sdram_bss .bss.sdram_bss.*)", "*(.sdram_bss .sdram_bss.*)"))))
    expect("an .sdram_bss listed after .bss is caught", any("listed after .bss" in p for p in verify(constants, move_behind_bss(script, ".sdram_bss"))))
    expect("a .dma_rings listed after .bss is caught", any("listed after .bss" in p for p in verify(constants, move_behind_bss(script, ".dma_rings"))))
    expect("a missing arena symbol is caught", any("__sdram_arena_end" in p for p in verify(constants, script.replace("__sdram_arena_end", "__sdram_top"))))
    broken = dict(constants)
    broken["kRamfuncLBudget"] = (64 << 10) + 4
    expect("a hot-code budget past SRAM_L is caught", any("kRamfuncLBudget" in p for p in verify(broken, script)))
    broken = dict(constants)
    broken["kSramLFreeBase"] += 4
    expect("a free base off the budget is caught", any("kSramLFreeBase" in p for p in verify(broken, script)))
    broken = dict(constants)
    broken["kSdramSize"] = 32 << 20
    expect("an SDRAM past 16 MiB is caught", any("SDRAM" in p for p in verify(broken, render(template.read_text(encoding="utf-8"), broken))))
    broken = dict(constants)
    broken["kFlashSize"] = 2 << 20
    expect("flash past 1 MiB is caught", any("0x000FFFFF" in p for p in verify(broken, render(template.read_text(encoding="utf-8"), broken))))

    # Template errors.
    raises("unknown placeholder", lambda: render("@kNoSuchThing@", constants))
    raises("duplicate constant", lambda: parse_header("inline constexpr std::uint32_t kA = 1u;\ninline constexpr std::uint32_t kA = 2u;"))
    raises("unevaluable constant", lambda: parse_header("inline constexpr std::uint32_t kA = sizeof(int);"))
    raises("no constants", lambda: parse_header("int x;"))
    expect("comments are ignored", parse_header("// inline constexpr std::uint32_t kHidden = 1u;\n/* inline constexpr std::uint32_t kAlso = 2u; */\ninline constexpr std::uint32_t kShown = 3u;") == {"kShown": 3})

    # check against a file on disk: matching passes, a stale one fails.
    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / "good.ld"
        good.write_text(script, encoding="utf-8")
        ns = argparse.Namespace(header=str(header), template=str(template), ld=str(good))
        expect("check passes on a fresh file", cmd_check(ns) == 0)
        stale = Path(tmp) / "stale.ld"
        stale.write_text(script.replace("0x1FFF0000", "0x1FFF4000"), encoding="utf-8")
        ns.ld = str(stale)
        raises("check fails on a stale file", lambda: cmd_check(ns))

    for failure in failures:
        print("self-test FAILED: " + failure)
    print("gen_ld self-test: %s" % ("ok" if not failures else "%d failure(s)" % len(failures)))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    for name in ("generate", "check"):
        p = sub.add_parser(name)
        p.add_argument("--header", required=True)
        p.add_argument("--template", required=True)
        if name == "generate":
            p.add_argument("--out", required=True)
        else:
            p.add_argument("--ld", help="a generated script to compare with what the header and template generate")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command is None:
        parser.error("a command or --self-test is required")
    try:
        return {"generate": cmd_generate, "check": cmd_check}[args.command](args)
    except GenError as exc:
        print("gen_ld: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
