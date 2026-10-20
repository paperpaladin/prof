#!/usr/bin/env python3
"""Diff prof's .eh_frame reader against readelf, row by row.

    python3 t/cfi_cmp.py ./prof FILE...

`readelf --debug-dump=frames-interp` prints, for every FDE in a file, the
interpreted CFI table: one row per instruction address at which the unwind
rules change, with a column for the CFA and one per callee-saved register.
That is exactly what `prof --cfi-dump` prints for the addresses it is given.
So this script reads readelf's table, picks addresses out of it, asks prof for
those same addresses, and compares every column readelf printed. A register
readelf does NOT print a column for has no rule in that FDE, so prof must say
"u" for it; that is checked too, since a parser inventing rules for registers
nobody mentioned would otherwise go unnoticed.

Three outcomes are counted separately, because merging them hides the
interesting one:

  agree    identical rules
  wrong    prof produced different rules -- a wrong answer, the worst case
  refused  prof produced "-" where readelf has a row -- an honest refusal,
           which still means no stack for that address

Addresses in the gaps BETWEEN FDEs are probed too, and there prof must
refuse: an address with no FDE must not be given the preceding function's
rules, which is precisely what an unchecked binary search would do.

Nothing here reads prof's output to decide what is correct. readelf is the
authority; prof is the thing on trial.
"""

import re, subprocess, sys

ENTRY = re.compile(r"^[0-9a-f]+ [0-9a-f]+ [0-9a-f]+ (CIE|FDE)\b")
PCRANGE = re.compile(r"pc=([0-9a-f]+)\.\.([0-9a-f]+)")
CIEREF = re.compile(r"cie=([0-9a-f]+)")
ROW = re.compile(r"^([0-9a-f]{8,16}) ")

# prof --cfi-dump prints the CFA and then one rule per register in DWARF
# numbering (System V psABI figure 3.36), which is this order.
DWREGS = ["rax", "rdx", "rcx", "rbx", "rsi", "rdi", "rbp", "rsp", "r8", "r9",
          "r10", "r11", "r12", "r13", "r14", "r15", "ra"]


def split_row(tokens, cols):
    """readelf writes a register rule as either 'c-8' or 'r3 (rbx)'."""
    out, i = {}, 0
    for c in cols:
        if i >= len(tokens):
            break
        v = tokens[i]
        i += 1
        if i < len(tokens) and tokens[i].startswith("("):
            i += 1  # the register name readelf appends in parentheses
        out[c] = v
    return out


def parse(path):
    """-> (fdes, cies). fdes: list of (start, end, cie_off, rows).
    rows: list of (loc, {col: rule}). cies: {offset: rows}."""
    # Not check=True: readelf exits 1 on a file whose build-id points at a
    # separate debug object with NOBITS sections (every Debian libc), after
    # having printed a perfectly good table for the file itself. So the gate
    # is "did it print FDEs", not its exit status -- and a truly unreadable
    # file raises rather than quietly comparing nothing.
    txt = subprocess.run(
        ["readelf", "--debug-dump=frames-interp", path],
        capture_output=True, text=True).stdout
    if "FDE" not in txt:
        raise SystemExit("readelf printed no FDE for %s" % path)
    fdes, cies = [], {}
    cur, cols, kind, off = None, [], None, None
    for line in txt.splitlines():
        m = ENTRY.match(line)
        if m:
            kind = m.group(1)
            off = int(line.split()[0], 16)
            cols = []
            if kind == "FDE":
                pc = PCRANGE.search(line)
                ref = CIEREF.search(line)
                cur = (int(pc.group(1), 16), int(pc.group(2), 16),
                       int(ref.group(1), 16), [])
                fdes.append(cur)
            else:
                cur = (0, 0, off, [])
                cies[off] = cur[3]
            continue
        if line.startswith("   LOC"):
            cols = line.split()[1:]          # CFA, then one per register
            continue
        if "ZERO terminator" in line:
            cur, cols = None, []   # the zero word that ends .eh_frame
            continue
        m = ROW.match(line)
        if m and cur is not None and cols:
            tok = line.split()
            cur[3].append((int(tok[0], 16), split_row(tok[1:], cols)))
    return fdes, cies


def want(row):
    """The full expected rule set: what readelf printed, and "u" for every
    register it did not print a column for."""
    out = {"CFA": row.get("CFA", "u")}
    for r in DWREGS:
        out[r] = row.get(r, "u")
    return out


def got(fields):
    """Parse one prof --cfi-dump line's fields after the address."""
    if fields[0] == "-":
        return None
    out = {"CFA": fields[0]}
    for i, r in enumerate(DWREGS):
        out[r] = fields[1 + i]
    return out


def main():
    prof, files = sys.argv[1], sys.argv[2:]
    agree = wrong = refused = gapok = gapbad = 0
    fdes_with_rows = fdes_total = 0
    bad = []

    for path in files:
        fdes, cies = parse(path)
        probes = []          # (addr, cfa, rbp, ra) expected
        for start, end, cie, rows in fdes:
            fdes_total += 1
            if not rows:
                # readelf prints no table for an FDE whose program is all
                # nops; the rules are then the CIE's initial row.
                rows = cies.get(cie, [])
                if not rows:
                    continue
                rows = [(start, rows[-1][1])]
            fdes_with_rows += 1
            for i, (loc, r) in enumerate(rows):
                nxt = rows[i + 1][0] if i + 1 < len(rows) else end
                for a in (loc, loc + 1, nxt - 1):
                    if loc <= a < nxt and a < end:
                        probes.append((a, want(r)))
        # the gaps: alignment padding between functions, and anything after
        # the last FDE. No FDE covers these, so there are no rules.
        covered = sorted((s, e) for s, e, _, _ in fdes)
        gaps = []
        for i in range(len(covered) - 1):
            if covered[i][1] < covered[i + 1][0]:
                gaps.append(covered[i][1])
        if covered:
            gaps.append(covered[-1][1])

        seen = {}
        addrs = [p[0] for p in probes] + gaps
        out = subprocess.run([prof, "--cfi-dump", path], check=True, text=True,
                             capture_output=True,
                             input="".join("%x\n" % a for a in addrs)).stdout
        for line in out.splitlines():
            f = line.split()
            seen[int(f[0], 16)] = got(f[1:])

        for a, exp in probes:
            g = seen.get(a, 0)
            if g == exp:
                agree += 1
            elif g is None:
                refused += 1
                if len(bad) < 12:
                    bad.append("refused %s %#x want %s" % (path, a, exp))
            else:
                wrong += 1
                if len(bad) < 12:
                    diff = "missing" if g == 0 else ", ".join(
                        "%s got %s want %s" % (k, g[k], exp[k])
                        for k in exp if g.get(k) != exp[k])
                    bad.append("wrong   %s %#x %s" % (path, a, diff))
        for a in gaps:
            if seen.get(a, 0) is None:
                gapok += 1
            else:
                gapbad += 1
                if len(bad) < 12:
                    bad.append("gap     %s %#x got %s want no rules" % (path, a, seen.get(a, 0)))

    for b in bad:
        print("  " + b)
    print("cfi vs readelf: %d rows agree, %d wrong, %d refused; "
          "%d/%d uncovered addresses refused correctly; %d/%d FDEs compared"
          % (agree, wrong, refused, gapok, gapok + gapbad, fdes_with_rows, fdes_total))
    if agree == 0:
        print("cfi vs readelf: NOTHING WAS COMPARED")
        return 1
    return 1 if (wrong or refused or gapbad) else 0


if __name__ == "__main__":
    sys.exit(main())
