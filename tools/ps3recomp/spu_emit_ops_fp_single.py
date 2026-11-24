"""Group `fp_single`: single-precision arithmetic (`fa`, `fs`, `fm`, `fma`, `fms`, `fnms`), compares
(`fceq`, `fcmeq`, `fcgt`, `fcmgt`), estimates (`frest`, `frsqest`, `fi`) and conversions (`csflt`, `cuflt`,
`cflts`, `cfltu`).

The lane semantics live in `runtime/include/sshd/spu_fp.h`; a handler only picks the helper. SPU ISA 1.2,
9.1 (extended-range single precision, p.195-196) and the instruction pages for `fa` .. `fnms`:

- A word with exponent field 0 is zero whatever its sign and fraction (denormals are zero); an exponent
  field of 255 is an ordinary exponent, there are no Inf or NaN.
- A result is zero when it is zero or below the smallest normal number (always +0), saturates to
  `sign | 0x7fffffff` above the largest, and is otherwise truncated toward zero to 24 bits.
- `fma`, `fms` and `fnms` compute RA * RB + RC, RA * RB - RC and RC - RA * RB with an exact product; only
  the final value is rounded.
- The compares (p.231-234) always run in extended-range mode: a word with exponent field 0 counts as +0,
  two zeros are equal and never greater. `fceq`/`fcgt` order by sign and magnitude, `fcmeq`/`fcmgt` by
  magnitude alone; true is all ones in the word.
- The estimates (p.215-219) are a recorded deviation, because the ISA does not publish the hardware's base
  and step tables: `frest` and `frsqest` give the exact reciprocal (square root) truncated to 24 bits,
  `fi` returns RB. `frest` of a zero exponent is `sign | 0x7fffffff`, of `|x| >= 2^126` is +0;
  `frsqest` of a zero exponent is `0x7fffffff` and its sign is always 0.

- The conversions (p.220-223) use `scale = 155 - I8` (`csflt`, `cuflt`) or `173 - I8` (`cflts`, `cfltu`):
  the integer divided or the float multiplied by 2^scale. `csflt`/`cuflt` go through the same truncating
  pack as the arithmetic; `cflts` saturates to `0x7fffffff` / `0x80000000`, `cfltu` gives 0 for negative
  values and saturates to `0xffffffff`; otherwise the value is truncated toward zero. A scale outside 0..127
  (undefined in the ISA) is not emitted: the word counts as `<mnemonic>:scale` among the unhandled ones.

The status register is not touched: the single-precision flags are never set (recorded deviation).
`fscrrd` and `fscrwr` belong to the `chan` module.
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler

_RR = {"fa": "sshdSpuFa", "fs": "sshdSpuFs", "fm": "sshdSpuFm", "fceq": "sshdSpuFceq", "fcmeq": "sshdSpuFcmeq",
       "fcgt": "sshdSpuFcgt", "fcmgt": "sshdSpuFcmgt", "fi": "sshdSpuFi"}

# One source operand.
_R = {"frest": "sshdSpuFrest", "frsqest": "sshdSpuFrsqest"}

# The RRR forms name their fields RT4, RA, RB, RC.
_RRR = {"fma": "sshdSpuFma", "fms": "sshdSpuFms", "fnms": "sshdSpuFnms"}


def _make_rr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s, %s);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")))]
    handler(name)(emit)


def _make_r(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")))]
    handler(name)(emit)


def _make_rrr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s, %s, %s);" % (op.gpr(op.f("RT4")), fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")),
                                          op.gpr(op.f("RC")))]
    handler(name)(emit)


# Conversions: (helper, scale base). The scale is 155 - I8 (integer to float) or 173 - I8 (float to
# integer), ISA 1.2 p.220-223.
_CONVERT = {"csflt": ("sshdSpuCsflt", 155), "cuflt": ("sshdSpuCuflt", 155),
            "cflts": ("sshdSpuCflts", 173), "cfltu": ("sshdSpuCfltu", 173)}


def _make_convert(name: str, fn: str, base: int):
    def emit(op: SpuOp) -> list[str] | None:
        scale = base - op.f("I8")
        # The ISA leaves the result undefined outside 0..127; the census shows such a site as unhandled
        # instead of guessing a value.
        if not 0 <= scale <= 127:
            return op.unsupported("scale")
        return ["%s = %s(%s, %du);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")), scale)]
    handler(name)(emit)


for _name, _fn in _RR.items():
    _make_rr(_name, _fn)
for _name, _fn in _R.items():
    _make_r(_name, _fn)
for _name, _fn in _RRR.items():
    _make_rrr(_name, _fn)
for _name, (_fn, _base) in _CONVERT.items():
    _make_convert(_name, _fn, _base)
