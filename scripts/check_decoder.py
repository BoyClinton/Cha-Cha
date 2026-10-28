"""Cross-check the recompiler's Espresso decoder against Ghidra's disassembly.

Inputs (both one line per distinct instruction word):
  ours.tsv    from `decoder_coverage <repo> ours.tsv`      word, canonical text, first address
  ghidra.tsv  from Ghidra `ExportMnemonics.java ghidra.tsv` word, mnemonic, operands
For every word in both files: Ghidra's (possibly simplified) mnemonic must map to our canonical op, and every
register Ghidra prints (rN / fN) must appear among our operands.

Usage: python -I scripts/check_decoder.py ours.tsv ghidra.tsv
"""
import re
import sys
from collections import Counter

# Simplified mnemonic -> canonical op (record '.', link 'l', absolute 'a', '+'/'-' hints stripped first).
ALIASES = {
    "li": "addi", "lis": "addis", "la": "addi", "subi": "addi", "subis": "addis", "subic": "addic",
    "subic.": "addic.", "mr": "or", "nop": "ori", "not": "nor", "sub": "subf", "subc": "subfc",
    "cmpw": "cmp", "cmpwi": "cmpi", "cmplw": "cmpl", "cmplwi": "cmpli",
    "slwi": "rlwinm", "srwi": "rlwinm", "clrlwi": "rlwinm", "clrrwi": "rlwinm", "rotlwi": "rlwinm",
    "rotrwi": "rlwinm", "extlwi": "rlwinm", "extrwi": "rlwinm", "clrlslwi": "rlwinm", "inslwi": "rlwimi",
    "insrwi": "rlwimi", "rotlw": "rlwnm",
    "mtcr": "mtcrf", "crclr": "crxor", "crset": "creqv", "crmove": "cror", "crnot": "crnor",
    "trap": "tw", "mftbu": "mftb",
}
SPR_MOVES = re.compile(r"^m[tf](lr|ctr|xer|spr|sprg\d|dec|srr[01]|dar|dsisr|sdr1|ear|gqr\d|hid\d|l2cr|pvr|.*)$")
TRAPS = re.compile(r"^tw(eq|ne|lt|le|gt|ge|lgt|llt|lle|lge|ng|nl)i?$")
BRANCH_CC = "(eq|ne|lt|le|gt|ge|so|ns|un|nu|nl|ng|dnz|dz|dnzt|dnzf|dzt|dzf|t|f)"


def canonical(mn, ops):
    m = mn.rstrip("+-")
    if m in ("b", "bl", "ba", "bla"):
        return "b"
    if m in ("blr", "blrl", "bctr", "bctrl"):
        return "bclr" if "lr" in m else "bcctr"
    mm = re.fullmatch(r"b%s?(lr|ctr)?l?a?" % BRANCH_CC, m)
    if mm and m not in ("bl", "b"):
        return {"lr": "bclr", "ctr": "bcctr"}.get(mm.group(2), "bc")
    if m.startswith("bc") and m.rstrip("la") in ("bc", "bclr", "bcctr"):
        return m.rstrip("la") if m.rstrip("la") != "bc" or not m.startswith("bcctr") else "bcctr"
    if TRAPS.match(m):
        return "twi" if m.endswith("i") else "tw"
    if m in ("mftb",):
        return "mftb"
    if SPR_MOVES.match(m) and m not in ("mfcr", "mtcrf", "mfmsr", "mtmsr", "mfsr", "mtsr", "mfsrin", "mtsrin",
                                        "mffs", "mtfsf", "mtfsfi", "mtfsb0", "mtfsb1"):
        return "mtspr" if m.startswith("mt") else "mfspr"
    rec = m.endswith(".") and m not in ("andi.", "andis.", "addic.", "stwcx.", "subic.")
    base = m[:-1] if rec else m
    base = ALIASES.get(base, base)
    return base


def ours_op(text):
    mn = text.split(" ", 1)[0]
    if mn.startswith(".long"):
        return None
    for suffix in ("la", "l", "a"):
        for b in ("bcctr", "bclr", "bc", "b"):
            if mn == b + suffix:
                return b
    if mn not in ("andi.", "andis.", "addic.", "stwcx."):
        mn = mn.rstrip(".")
    if mn.endswith("o") and mn[:-1] in ("add", "addc", "adde", "addme", "addze", "divw", "divwu", "mullw", "neg",
                                        "subf", "subfc", "subfe", "subfme", "subfze"):
        mn = mn[:-1]
    return mn


def regs(text):
    return re.findall(r"\b[rf]\d+\b", text)


def main():
    ours = {}
    for line in open(sys.argv[1], encoding="utf-8"):
        w, text, _ = line.rstrip("\n").split("\t")
        ours[w] = text
    ghidra = {}
    for line in open(sys.argv[2], encoding="utf-8"):
        w, mn, ops = line.rstrip("\n").split("\t")
        ghidra[w] = (mn, ops)
    both = ours.keys() & ghidra.keys()
    op_bad, reg_bad = Counter(), Counter()
    samples = []
    for w in sorted(both):
        mn, ops = ghidra[w]
        want, got = canonical(mn, ops), ours_op(ours[w])
        if want != got:
            op_bad[(mn, ours[w].split(" ")[0])] += 1
            if len(samples) < 25:
                samples.append("%s ghidra=%s %s ours=%s" % (w, mn, ops, ours[w]))
            continue
        ours_regs = regs(ours[w].split(" ", 1)[1] if " " in ours[w] else "")
        missing = [r for r in regs(ops) if r not in ours_regs]
        if missing:
            reg_bad[mn] += 1
            if len(samples) < 25:
                samples.append("%s regs ghidra=%s %s ours=%s" % (w, mn, ops, ours[w]))
    print("words: ours=%d ghidra=%d compared=%d; op mismatches=%d, register mismatches=%d"
          % (len(ours), len(ghidra), len(both), sum(op_bad.values()), sum(reg_bad.values())))
    for (g, o), n in op_bad.most_common(20):
        print("  op  ghidra %-10s ours %-10s x%d" % (g, o, n))
    for g, n in reg_bad.most_common(10):
        print("  reg %-10s x%d" % (g, n))
    for s in samples:
        print("   ", s)
    only = sorted(ghidra.keys() - ours.keys())
    print("only in Ghidra (outside function bodies): %d, mnemonics %s" % (len(only), Counter(ghidra[w][0] for w in only).most_common(8)))
    return 0 if not op_bad and not reg_bad else 1


if __name__ == "__main__":
    sys.exit(main())
