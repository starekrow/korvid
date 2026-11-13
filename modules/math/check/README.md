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
The f32 functions are the f64 ones (or for `pow`, a shorter path in double) rounded to
float, which makes them correctly rounded except where the double lands within its own
error of a float rounding boundary: a handful of the 2^32 inputs.

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
  needs about 70 bits when it is near 1000. The f32 `pow` needs only one double.
- **The exact ones** (`floor`, `ceil`, `trunc`, `round`, `fmod`, `ldexp`, `frexp`, `sqrt`):
  by the bits, by the integer conversion, by long division (`fmod`), or the instruction
  (`sqrt`, and `floor` and `ceil` where the machine has one). None has a branch on the
  input that a random sign would mispredict.

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

| function | f32: inputs | f32: not correctly rounded | f32: worst | f64: worst (4M samples) |
|---|---|---|---|---|
| `sin` | all 2^32 | 2 | 1 ulp | 2 ulp |
| `cos` | all 2^32 | 4 | 1 ulp | 2 ulp |
| `tan` | all 2^32 | 0 | correctly rounded | 4 ulp |
| `asin` | all 2^32 | 0 | correctly rounded | 2 ulp |
| `acos` | all 2^32 | 2 | 1 ulp | 2 ulp |
| `atan` | all 2^32 | 2 | 1 ulp | 1 ulp |
| `atan2` | 200M pairs | 0 | correctly rounded | 2 ulp |
| `exp` | all 2^32 | 0 | correctly rounded | 1 ulp |
| `exp2` | all 2^32 | 2 | 1 ulp | 1 ulp |
| `log` | all 2^32 | 5 | 1 ulp | 2 ulp |
| `log2` | all 2^32 | 0 | correctly rounded | 3 ulp |
| `log10` | all 2^32 | 3 | 1 ulp | 3 ulp |
| `pow` | 200M pairs | 273 (0.00014%) | 1 ulp | 1 ulp |

For comparison, glibc 2.43's `sinf` and `cosf` differ from these (that is, are not
correctly rounded) on about 1% of inputs, by an ulp; its `tanf`, `atanf`, `asinf`,
`acosf`, `atan2f` and `log10f` agreed with kv's on all of 4M inputs each. The f64
functions are correctly rounded for 66% (`tan`) to over 99% (`asin`, `acos`) of inputs. `f64 pow`'s worst was 960 ulp before log2 and the product went into two doubles.

## Speed

`bench.sh` runs `bench.cc`: ns per call on one thread over 4M game-sized inputs, summed,
best of five, with the engine's flags (`-O2`, no fused multiply-adds, no errno), glibc
2.43, gcc 15.2, on an AMD Ryzen 7 7840U (a laptop: 8 cores, 16 threads).

| function | kv | glibc | kv / glibc |
|---|---|---|---|
| `sinf` | 8.4 ns | 6.1 | 1.4x |
| `cosf` | 8.0 | 6.1 | 1.3x |
| `tanf` | 10.2 | 4.3 | 2.4x |
| `atanf` | 8.9 | 5.0 | 1.8x |
| `atan2f` | 10.4 | 6.7 | 1.5x |
| `asinf` | 13.6 | 4.8 | 2.8x |
| `expf` | 7.6 | 2.5 | 3.0x |
| `logf` | 8.2 | 2.5 | 3.3x |
| `powf(x, 2.4)` | 22.2 | 3.4 | 6.6x |
| `sqrtf` | 0.6 | 0.6 | 1.0x |
| `floorf` | 1.6 | 1.1 | 1.5x |
| `fmodf` | 14.2 | 3.9 | 3.7x |
| `sin` (f64) | 8.4 | 13.4 | 0.6x |
| `exp` (f64) | 6.6 | 3.8 | 1.8x |
| `log` (f64) | 7.4 | 3.0 | 2.5x |
| `pow` (f64) | 50.8 | 7.2 | 7.1x |

Branches were most of the first cut's cost: a random sign, a quadrant or a halving test
mispredicts half the time, about 7 ns a miss on this machine. `sinf` was 2.7x glibc's time
and `floorf` 7.5x before every such choice became a select or a mask. What is left is the
double-precision series, longer than an f32 needs (`sin`'s goes to r^17), and, for `expf`
and `logf`, glibc's tables, which reduce the argument further than one step of ln 2 does.

## What is not done

- **Speed.** The f32 functions run the f64 code and round: full double accuracy is what
  makes them correctly rounded, and it costs. f32 kernels of their own (shorter
  polynomials, glibc-style tables for exp and log) would come close to glibc's speed, and
  stay deterministic, at the price of being correctly rounded for most inputs rather than
  nearly all. Whether that trade is right is open.
- **Other platforms, run.** The functions are written for cl, gcc and clang on x86-64 and
  ARM64, but only Linux has run them here. The tests' hashes are the proof: they must pass
  on Windows and macOS as they do on Linux.
- **The f64 functions are not correctly rounded**, only within an ulp or a few (see
  above). Only the vendored decoders use them.
- **Outside this module:** the GPU's math (shaders), printf and strtod (exact on all three
  C libraries the engine targets, but not checked here), SDL3's internals, and the order
  of a parallel sum.
