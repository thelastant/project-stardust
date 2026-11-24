"""Group `int`: arithmetic, logical, compare, select, multiply, byte, shift and rotate instructions of
the integer lanes, plus the conditional halts (`heq`, `hgt`, `hlgt` and their immediate forms).

The lane semantics live in `runtime/include/sshd/spu_ops.h`; a handler only picks the helper and
turns the immediate into a splatted register, so every immediate form shares the code of its register
form. SPU ISA 1.2, chapters "Constant-formation", "Integer and logical", "Compare, branch, and halt"
and "Shift and rotate".
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler, ls_lit, u32

_MASK32 = 0xFFFFFFFF

# Register forms: RT, RA, RB.
_RR = {
    "a": "sshdSpuA", "ah": "sshdSpuAh", "sf": "sshdSpuSf", "sfh": "sshdSpuSfh",
    "cg": "sshdSpuCg", "bg": "sshdSpuBg",
    "mpy": "sshdSpuMpy", "mpyu": "sshdSpuMpyu", "mpyh": "sshdSpuMpyh", "mpys": "sshdSpuMpys",
    "mpyhh": "sshdSpuMpyhh", "mpyhhu": "sshdSpuMpyhhu",
    "avgb": "sshdSpuAvgb", "absdb": "sshdSpuAbsdb", "sumb": "sshdSpuSumb",
    "and": "sshdSpuAnd", "andc": "sshdSpuAndc", "or": "sshdSpuOr", "orc": "sshdSpuOrc", "xor": "sshdSpuXor",
    "nand": "sshdSpuNand", "nor": "sshdSpuNor", "eqv": "sshdSpuEqv",
    "ceqb": "sshdSpuCeqb", "ceqh": "sshdSpuCeqh", "ceq": "sshdSpuCeq",
    "cgtb": "sshdSpuCgtb", "cgth": "sshdSpuCgth", "cgt": "sshdSpuCgt",
    "clgtb": "sshdSpuClgtb", "clgth": "sshdSpuClgth", "clgt": "sshdSpuClgt",
    "shl": "sshdSpuShl", "shlh": "sshdSpuShlh", "rot": "sshdSpuRot", "roth": "sshdSpuRoth",
    "rotm": "sshdSpuRotm", "rothm": "sshdSpuRothm", "rotma": "sshdSpuRotma", "rotmah": "sshdSpuRotmah",
}

# Forms whose result also depends on the old RT (carry-in, accumulator).
_RR_ACC = {
    "addx": "sshdSpuAddx", "cgx": "sshdSpuCgx", "sfx": "sshdSpuSfx", "bgx": "sshdSpuBgx",
    "mpyhha": "sshdSpuMpyhha", "mpyhhau": "sshdSpuMpyhhau",
}

_RRR = {"selb": "sshdSpuSelb", "mpya": "sshdSpuMpya"}

_UNARY = {
    "clz": "sshdSpuClz", "cntb": "sshdSpuCntb", "xsbh": "sshdSpuXsbh", "xshw": "sshdSpuXshw", "xswd": "sshdSpuXswd",
}

# Immediate forms: mnemonic -> (register-form helper, element width of the splat). RI10 forms
# sign-extend I10 to the element width; the byte forms use its low 8 bits; the shifts take the
# sign-extended I7 as the count, of which the helper keeps the low bits. `sfi` computes imm - RA, which
# is `sf` with the immediate in the RB position.
_IMM = {
    "ai": ("sshdSpuA", "W"), "ahi": ("sshdSpuAh", "H"), "sfi": ("sshdSpuSf", "W"), "sfhi": ("sshdSpuSfh", "H"),
    "andi": ("sshdSpuAnd", "W"), "andhi": ("sshdSpuAnd", "H"), "andbi": ("sshdSpuAnd", "B"),
    "ori": ("sshdSpuOr", "W"), "orhi": ("sshdSpuOr", "H"), "orbi": ("sshdSpuOr", "B"),
    "xori": ("sshdSpuXor", "W"), "xorhi": ("sshdSpuXor", "H"), "xorbi": ("sshdSpuXor", "B"),
    "ceqi": ("sshdSpuCeq", "W"), "ceqhi": ("sshdSpuCeqh", "H"), "ceqbi": ("sshdSpuCeqb", "B"),
    "cgti": ("sshdSpuCgt", "W"), "cgthi": ("sshdSpuCgth", "H"), "cgtbi": ("sshdSpuCgtb", "B"),
    # The unsigned compares use the sign-extended immediate, then compare as unsigned.
    "clgti": ("sshdSpuClgt", "W"), "clgthi": ("sshdSpuClgth", "H"), "clgtbi": ("sshdSpuClgtb", "B"),
    "mpyi": ("sshdSpuMpy", "W"),
    # Zero-extended from the sign-extended 16-bit immediate.
    "mpyui": ("sshdSpuMpyu", "U"),
    "shlhi": ("sshdSpuShlh", "H"), "shli": ("sshdSpuShl", "W"), "rothi": ("sshdSpuRoth", "H"),
    "roti": ("sshdSpuRot", "W"), "rothmi": ("sshdSpuRothm", "H"), "rotmi": ("sshdSpuRotm", "W"),
    "rotmahi": ("sshdSpuRotmah", "H"), "rotmai": ("sshdSpuRotma", "W"),
}

_SPLAT = {
    "W": ("sshdSpuSplatW", _MASK32, "0x%08xu"),
    "H": ("sshdSpuSplatH", 0xFFFF, "0x%04xu"),
    "B": ("sshdSpuSplatB", 0xFF, "0x%02xu"),
    "U": ("sshdSpuSplatW", 0xFFFF, "0x%08xu"),
}


def _set(op: SpuOp, expr: str, reg: str = "RT") -> list[str]:
    return ["%s = %s;" % (op.gpr(op.f(reg)), expr)]


def _splat(kind: str, value: int) -> str:
    fn, mask, fmt = _SPLAT[kind]
    return "%s(%s)" % (fn, fmt % (value & mask))


def _make_rr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s)" % (fn, op.gpr(op.f("RA")), op.gpr(op.f("RB"))))
    handler(name)(emit)


def _make_rr_acc(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s, %s)" % (fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")), op.gpr(op.f("RT"))))
    handler(name)(emit)


def _make_rrr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s, %s)" % (fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")), op.gpr(op.f("RC"))), "RT4")
    handler(name)(emit)


def _make_unary(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s)" % (fn, op.gpr(op.f("RA"))))
    handler(name)(emit)


def _make_imm(name: str, fn: str, kind: str):
    imm_field = "I7" if name.startswith(("shl", "rot")) else "I10"

    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s)" % (fn, op.gpr(op.f("RA")), _splat(kind, op.f(imm_field))))
    handler(name)(emit)


for _name, _fn in _RR.items():
    _make_rr(_name, _fn)
for _name, _fn in _RR_ACC.items():
    _make_rr_acc(_name, _fn)
for _name, _fn in _RRR.items():
    _make_rrr(_name, _fn)
for _name, _fn in _UNARY.items():
    _make_unary(_name, _fn)
for _name, (_fn, _kind) in _IMM.items():
    _make_imm(_name, _fn, _kind)


# --- Immediate loads --------------------------------------------------------------------------

@handler("il")
def emit_il(op: SpuOp) -> list[str]:
    return _set(op, _splat("W", op.f("I16")))


@handler("ilh")
def emit_ilh(op: SpuOp) -> list[str]:
    return _set(op, _splat("H", op.f("U16")))


@handler("ilhu")
def emit_ilhu(op: SpuOp) -> list[str]:
    return _set(op, _splat("W", op.f("U16") << 16))


@handler("ila")
def emit_ila(op: SpuOp) -> list[str]:
    return _set(op, _splat("W", op.f("I18")))


@handler("iohl")
def emit_iohl(op: SpuOp) -> list[str]:
    rt = op.gpr(op.f("RT"))
    return _set(op, "sshdSpuOr(%s, %s)" % (rt, _splat("W", op.f("U16"))))


# --- Conditional halts ------------------------------------------------------------------------

@handler("heq", "hgt", "hlgt", "heqi", "hgti", "hlgti")
def emit_halt(op: SpuOp) -> list[str]:
    # Compares the preferred words; the immediate forms use the sign-extended I10.
    a = op.pref(op.f("RA"))
    if op.name.endswith("i"):
        b = u32(op.f("I10"))
    else:
        b = op.pref(op.f("RB"))
    if op.name.startswith("heq"):
        cond = "%s == %s" % (a, b)
    elif op.name.startswith("hgt"):
        cond = "(int32_t)%s > (int32_t)%s" % (a, b)
    else:
        cond = "%s > %s" % (a, b)
    return ["if (%s) sshdSpuHalt(ctx, ls, %s);" % (cond, ls_lit(op.addr))]
