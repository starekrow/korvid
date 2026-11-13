# Checking kv's math

The scalar functions (`include/korvid/math/scalar.h`) promise the same bits on every
platform, and close to the right bits. The tests (`tests/scalar_test.cc`) hold the first
promise: a hash of every bit each function returns over a few hundred thousand inputs,
taken on Linux, which every platform's build must match. This directory holds the second,
and the speed. Neither tool is part of the build; both are Linux-only and need nothing
installed.

    modules/math/check/run.sh [FUNCTION]   # accuracy against MPFR, about 12 minutes on 16 threads
    modules/math/check/bench.sh            # ns per call against glibc, a few seconds
    modules/math/check/gen_constants.py    # rewrites src/constants.h (it must come out the same)

## How the functions work

Each transcendental is computed in double with `+ - * /` alone, from constants
`gen_constants.py` computes at 450 digits, and rounded once at the end. IEEE 754 makes each
of those operations correctly rounded, so any machine that does them one at a time gets
the same result: x86-64's SSE2 and ARM64 alike, and cl, gcc and clang alike, as long as no
multiply is fused into an add. The toolchains say so (`-ffp-contract=off`, `/fp:precise`).
The `_hp` f32 functions are the f64 ones rounded to float, which makes them correctly
rounded except where the double lands within its own error of a float rounding boundary:
a handful of the 2^32 inputs. The default f32 functions (`src/fast.cc`) compute less: what
an f32's 24 bits need, near 2^-44 of the true value, which still leaves them correctly
rounded on all but a few dozen inputs, at glibc's speed or near it.

- **sin, cos, tan**: x = n*pi/2 + r, |r| <= pi/4, by Cody-Waite (pi/2 in three parts) below
  2^20 and by Payne-Hanek (2/pi's bits, 1280 of them) above, so huge arguments are right
  too. Then the Taylor series of sin and cos in r, and the quadrant picks the sign and
  which, by a select rather than a branch.
- **atan, atan2, asin, acos**: atan(t), t in [0, 1], is atan(k/8) from a table plus the
  series of atan((t - c)/(1 + tc)), under 1/16. Larger t is pi/2 less atan(1/t); asin and
  acos are atan2 of x and sqrt((1 - x)(1 + x)). C's table of zeros and infinities (Annex
  F) for atan2.
- **exp, exp2**: k ln2 + r, |r| <= ln2/2, ln 2 in two parts; the series of e^r; 2^k by
  `ldexp`, rounded once into the subnormals.
- **log, log2, log10**: 2^e m, m in [sqrt(1/2), sqrt(2)), and log m = 2 atanh(s),
  s = (m-1)/(m+1). `log2` adds e exactly, so a power of two is its exponent.
- **pow**: C's table first. An integer exponent of 64 or less is repeated squaring in two
  doubles, exact when the result is (stb_vorbis sizes its codebooks with pow and floor).
  Otherwise 2^(y log2 x), with log2 x and the product carried in two doubles: y log2 x
  needs about 70 bits when it is near 1000. `pow_hp` (f32) is this one, rounded.
- **The exact ones** (`floor`, `ceil`, `trunc`, `round`, `fmod`, `ldexp`, `frexp`, `sqrt`):
  by the bits, by the integer conversion, by long division (`fmod`), or the instruction
  (`sqrt`, and `floor` and `ceil` where the machine has one). None has a branch on the
  input that a random sign would mispredict.

The default f32 functions take the same routes, shortened:

- **sin, cos, tan**: two parts of pi/2 (enough below 2^20; above, the `_hp` path), and
  polynomials fitted for the range (Chebyshev interpolation, solved exactly by
  `gen_constants.py`): 5 coefficients for sin where Taylor needs 8.
- **atan, atan2**: the nearest of five sectors k pi/8, from four comparisons, and
  `(y cos - x sin)/(x cos + y sin)` for what is left: one division, no select, and a
  6-coefficient polynomial for |u| <= tan(pi/16).
- **asin, acos**: one 10-coefficient polynomial on [0, 1/2], reached past 1/2 through
  `sqrt((1 - x)/2)`. No division.
- **exp, exp2**: 64ths of a power of two, 2^(j/64) from a table, and a degree-4
  polynomial, as glibc's own `expf` works.
- **log, log2, log10**: 64 cells of [0.7, 1.4), 1/c to 29 bits so that `z/c - 1` is
  exact, and 1/c = 1 in the cell that holds 1, so a result near zero keeps every bit.
- **pow**: the two above in one double.

Polynomials are evaluated two coefficients a step, `(c0 + c1 z) + z^2 (...)`: the chain
is half Horner's, which in a loop of calls is most of the time.

Every NaN they return is the canonical quiet NaN: x86's default NaN has the sign bit set
and ARM's does not, so a NaN made by arithmetic differs between them.

## Accuracy

`run.sh` runs `check.cc`. For each one-argument f32 function it takes every one of the 2^32
inputs and compares kv's result with the correctly rounded one. The reference is glibc's
double function rounded to float, except where that double lies within 8 of its ulps of a
float rounding boundary, or kv disagrees; there MPFR, at 160 bits, decides. The
two-argument f32 functions (200M pairs) and the f64 functions (4M inputs each) are sampled,
half from random bit patterns (huge, tiny, subnormal, NaN) and half from a game's
magnitudes, all against MPFR.

| function | f32 inputs | default: not correctly rounded | `_hp`: not correctly rounded | f64: worst (4M samples) |
|---|---|---|---|---|
| `sin` | all 2^32 | 14 | 2 | 2 ulp |
| `cos` | all 2^32 | 12 | 4 | 2 ulp |
| `tan` | all 2^32 | 20 | 0 | 4 ulp |
| `asin` | all 2^32 | 10 | 0 | 2 ulp |
| `acos` | all 2^32 | 2 | 2 | 2 ulp |
| `atan` | all 2^32 | 2 | 2 | 1 ulp |
| `atan2` | 200M pairs | 0 | 0 | 2 ulp |
| `exp` | all 2^32 | 20 | 0 | 1 ulp |
| `exp2` | all 2^32 | 13 | 2 | 1 ulp |
| `log` | all 2^32 | 3 | 5 | 2 ulp |
| `log2` | all 2^32 | 0 | 0 | 3 ulp |
| `log10` | all 2^32 | 4 | 3 | 3 ulp |
| `pow` | 200M pairs | 304 (0.00015%) | 0 | 1 ulp |

Every f32 result, default or `_hp`, is within 1 ulp of the true value; every miss is by
exactly one.

For comparison, glibc 2.43's `sinf` and `cosf` differ from these (that is, are not
correctly rounded) on about 1% of inputs, by an ulp; its `tanf`, `atanf`, `asinf`,
`acosf`, `atan2f` and `log10f` agreed with kv's on all of 4M inputs each. The f64
functions are correctly rounded for 66% (`tan`) to over 99% (`asin`, `acos`) of inputs. `f64 pow`'s worst was 960 ulp before log2 and the product went into two doubles.

## Speed

`bench.sh` runs `bench.cc`: ns per call on one thread over 4M game-sized inputs, summed,
best of five, with the engine's flags (`-O2`, no fused multiply-adds, no errno), glibc
2.43, gcc 15.2, on an AMD Ryzen 7 7840U (a laptop: 8 cores, 16 threads).

| function | default | `_hp` | glibc | default / glibc |
|---|---|---|---|---|
| `sinf` | 4.6 ns | 8.9 | 6.4 | 0.72x |
| `cosf` | 4.7 | 8.6 | 6.5 | 0.72x |
| `tanf` | 6.7 | 11.3 | 4.6 | 1.45x |
| `atanf` | 6.6 | 9.3 | 5.2 | 1.27x |
| `atan2f` | 9.0 | 10.8 | 7.0 | 1.28x |
| `asinf` | 6.8 | 14.1 | 5.0 | 1.37x |
| `acosf` | 10.4 | 15.1 | 4.9 | 2.13x |
| `expf` | 3.0 | 7.8 | 2.6 | 1.17x |
| `exp2f` | 3.0 | 7.2 | 2.6 | 1.16x |
| `logf` | 3.8 | 8.4 | 2.6 | 1.46x |
| `log2f` | 3.9 | 8.2 | 2.6 | 1.49x |
| `log10f` | 3.9 | 8.3 | 3.0 | 1.32x |
| `powf(x, 2.4)` | 8.0 | 57.2 | 3.5 | 2.28x |

| function | kv | glibc | kv / glibc |
|---|---|---|---|
| `sqrtf` | 0.6 ns | 0.6 | 1.0x |
| `floorf` | 1.7 | 1.1 | 1.5x |
| `fmodf` | 14.6 | 4.0 | 3.6x |
| `sin` (f64) | 8.7 | 13.9 | 0.62x |
| `exp` (f64) | 7.1 | 3.9 | 1.8x |
| `log` (f64) | 7.7 | 3.1 | 2.5x |
| `pow` (f64) | 55.1 | 7.5 | 7.4x |

Runs differ by up to 10% from one to the next. What made the difference, in the order it was found: branches (a random sign, a quadrant or
a halving test mispredicts half the time, about 7 ns a miss here; `sinf` was 2.7x glibc's
time and `floorf` 7.5x before every such choice became a select or a mask); for the
defaults, polynomials fitted to the range instead of Taylor's, and tables for exp and log;
a polynomial written out rather than looped (the compiler kept the loop); and two
coefficients a step, which halves the chain a loop of calls waits on. Two things that
looked cheaper were not: squaring s rather than selecting w (asin and acos then wait for
the square root before the polynomial), and pi times a comparison in place of a select
(`cvtsi2sd` carries a false dependency on its register).

## What is not done

- **Speed, the rest of the way.** `logf`, `tanf`, `atanf`, `asinf` and `atan2f` are
  1.3-1.4x glibc's time, `acosf` 2x and `powf` 2.2x. Some of glibc's lead is accuracy
  given up (its `logf` misses the correctly rounded result on about 1 input in 2,500, its
  `sinf` on 1 in 100), but not all: its `tanf`, `atanf`, `asinf` and `acosf` matched the
  correctly rounded result on all of 4M inputs and are faster still. Larger tables (log's
  cells, atan's sectors) are the obvious next step.
- **Other platforms, run.** The functions are written for cl, gcc and clang on x86-64 and
  ARM64, but only Linux has run them here. The tests' hashes are the proof: they must pass
  on Windows and macOS as they do on Linux.
- **The f64 functions are not correctly rounded**, only within an ulp or a few (see
  above). Only the vendored decoders use them.
- **Outside this module:** the GPU's math (shaders), printf and strtod (exact on all three
  C libraries the engine targets, but not checked here), SDL3's internals, and the order
  of a parallel sum.
