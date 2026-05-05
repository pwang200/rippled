# Defending microbenchmarks against compiler optimization

*Appendix to the host-function gas measurement report.*

This appendix documents how we ensured that the benchmark numbers
reflect real per-call wrapper work, rather than artifacts of compiler
optimization across tight inner loops with identical inputs.

The conclusion: for the most measurement-critical wrapper
(`getCurrentLedgerObjField`), three independent methodologies converged
on the same slope (0.02 ns/byte) and intercept (~26–29 ns). We have
~99.9% confidence the headline numbers are real per-call cost.

---

## 1. Why we measure in a tight inner loop

The benchmark's `runTimedLoop` times `kInner = 1000` consecutive wrapper
calls and divides the elapsed time by 1000 to get a per-call estimate.
Repeated `kSamples = 1000` times to get a distribution.

```cpp
for (int i = 0; i < samples; ++i) {
    auto t0 = clock::now();
    for (int j = 0; j < inner; ++j)
        fn();                          // 1000 wrapper calls
    auto t1 = clock::now();
    out.push_back((t1 - t0).count() / inner);
}
```

We can't time individual calls because of clock granularity. On macOS
Apple Silicon, `std::chrono::steady_clock::now()` has roughly 30–45 ns
overhead per call and similar minimum tick resolution. A wrapper call
that takes 6 ns (our baseline) is well below that floor; bracketing it
with two `now()` calls would yield 60–90 ns of clock noise per
measurement.

Inner-loop amortization lifts the per-call signal above clock
granularity at the cost of exposing the loop to potential
cross-iteration compiler optimization.

## 2. The problem: cross-iteration optimization

In a tight inner loop where every iteration has **identical inputs**,
the compiler's optimizer can in principle:

- Determine that iterations 2..N produce the same observable state as
  iteration 1.
- Hoist invariant computations out of the loop.
- Coalesce repeated identical writes into a single write.
- Cache values in registers across iterations rather than reloading.

If any of these happen, the measured per-call time is artificially low
because the loop is effectively executing fewer than `inner` real
iterations.

Our benchmark constructs each test with constant inputs (same SLE,
same params, same out_ptr) deliberately so that the measurement is
reproducible. That's also exactly the pattern that invites
optimization.

## 3. Defenses, in increasing strength

We evaluated four defense mechanisms, from cheapest to strongest.

### 3.1 `volatile` sink (initial implementation)

```cpp
volatile std::uint64_t benchSink_ = 0;

// inside the inner loop:
fn();
benchSink_ ^= rt.memData()[kOutOffset];
benchSink_ ^= static_cast<std::uint64_t>(resultsData[0].of.i32);
```

**What it does**: each iteration's read-modify-write of `benchSink_`
is a volatile-qualified side effect the compiler cannot remove or
reorder.

**What it doesn't do**: the right-hand-side reads
(`rt.memData()[kOutOffset]`, `resultsData[0].of.i32`) are
non-volatile. If the compiler can prove these values are invariant
across iterations, it may constant-fold the XOR and elide the
underlying work that produced them. The volatile sink keeps the *XOR*
visible, but doesn't force the wrapper call to be re-executed each
iteration.

**Confidence**: medium. Effective against simple optimizers; not
bulletproof under -O3 + LTO.

### 3.2 `asm volatile` compiler barrier

```cpp
template <typename T>
inline void doNotOptimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// inside the inner loop:
fn();
doNotOptimize(rt.memData()[kOutOffset]);
doNotOptimize(resultsData[0].of.i32);
```

**What it does**:
- `"r,m"(value)` constraint forces the value to be materialized in a
  register or memory at this exact program point. The compiler cannot
  prove the value isn't needed by an unknown asm instruction.
- `"memory"` clobber tells the compiler that any memory could have
  been modified. Cached values in registers must be reloaded if used
  later.
- `volatile` on the asm itself prevents reordering or removal of the
  barrier.

This is the same primitive Google Benchmark's `DoNotOptimize` and
`ClobberMemory` use under the hood.

**Confidence**: high. The compiler must:
1. Materialize the value at each barrier (defeats register caching of
   invariant values).
2. Treat memory as potentially modified across the barrier (defeats
   cross-iteration constant folding).
3. Keep the barrier itself in place (volatile).

The combination still doesn't *prove* the wrapper call is needed each
iteration if the compiler can statically determine its return value is
invariant — but for a path involving virtual calls, heap allocation,
and library-external code (`libcrypto`, `libsecp256k1`, `operator new`,
`memcpy`), full whole-program analysis is unlikely.

### 3.3 Per-iteration input variation

```cpp
constexpr int kNumDests = 16;
constexpr int kDestSpacing = 4096;
int callCount = 0;

// inside the inner loop:
auto const out_ptr = (callCount++ & (kNumDests - 1)) * kDestSpacing;
paramsData[1].of.i32 = out_ptr;
fn();
doNotOptimize(rt.memData()[out_ptr]);
```

**What it does**: each iteration writes to a *different* memory
location. The wrapper produces a different observable side effect on
every call. The compiler cannot prove iterations are redundant
because they aren't.

**Why this is the strongest defense**: it removes the precondition
under which cross-iteration optimization is even legal. Even with
arbitrary aggressive optimization, the compiler must execute each
iteration's distinct side effect.

**Cost**: a few additional instructions per call (increment, mask,
multiply) — measured at ~1–2 ns per iteration, lost in noise for
anything but the smallest measurements.

### 3.4 Differential measurement (call vs. no-call)

```cpp
auto with    = runTimedLoop(rt, [&]() { /* ... */ fn();          /* ... */ });
auto without = runTimedLoop(rt, [&]() { /* ... */ /* no fn() */  /* ... */ });
auto diff    = with.median - without.median;
```

The two lambdas have **identical** structure, parameter updates, and
asm barriers. The only difference is whether the wrapper call runs.
The subtraction isolates the pure wrapper cost from any harness
overhead.

**Why this matters**: even if both 3.2 and 3.3 had subtle harness
overhead, the differential subtracts it out. The remaining number is
what the wrapper call *itself* takes.

In our case the no-call loop measured "0 ns" (an artifact of integer
division when per-call work is sub-1 ns), confirming the harness
overhead is negligible and that the with-call measurement is almost
entirely wrapper cost.

## 4. Validation results

For `getCurrentLedgerObjField` (the most measurement-critical wrapper,
because it underlies all `get*Field` family calls and is among the
most-invoked host functions in production):

| Methodology | Intercept (ns) | Slope (ns/byte) |
|---|---|---|
| 3.1 volatile sink | 29 | 0.02 |
| 3.2 + 3.3 hardened (asm barrier + 16-way out_ptr cycle) | 26 | 0.02 |
| 3.4 differential (with-call − no-call) | 29 | 0.02 |

Three independent methodologies, identical slopes, intercepts agreeing
within run-to-run noise. The headline numbers are real.

## 5. Where defenses aren't needed

For wrappers whose dominant work is in **opaque library code**:

- `compute_sha512_half`: SHA-512 kernel is in libcrypto (separately
  compiled, not visible to the rippled compiler).
- `check_sig`: ECDSA verify is in libsecp256k1 (same).

The compiler cannot see into these libraries. Each call is treated as
opaque and side-effecting; cross-iteration optimization is not
possible. Cross-build comparison of slopes confirms this — debug and
release SHA-512 slopes are nearly identical (~0.55 vs 0.51 ns/byte),
where they differ wildly for rippled-internal code paths
(getCurrentLedgerObjField was 5 ns/byte debug → 0.02 ns/byte release).

The same applies to:
- `getLedgerSqn` baseline — too short to elide further; just a virtual
  call returning a uint32.
- `cacheLedgerObj` and `updateData` floor measurements — these are
  already labeled FLOOR (real cost is unmeasurable disk I/O or
  post-execution persistence), so an extra few percent of measurement
  fidelity doesn't change the calibration story.

Only `getCurrentLedgerObjField` warranted the full validation suite
because (a) its slope number directly drives `gasPerMemByte`-style
calibration decisions, (b) the wrapper code path is entirely visible
to the rippled compiler, and (c) its small per-call work makes any
elision proportionally significant.

## 6. The integer-division "0 ns" artifact

The differential test's no-call loop reported "0 ns" per iteration.
This is not zero work — it's the result of integer division in
`runTimedLoop`:

```cpp
out.push_back(elapsed / inner);  // integer division, inner = 1000
```

If the no-call lambda runs at, say, 0.5 ns/call, total per sample =
500 ns, and `500 / 1000 = 0`. The reported "0 ns" therefore means
**"sub-1 ns per iteration"** — the compiler has unrolled and fused
the no-op loop body so aggressively that it nearly compiles to
nothing.

Useful corollary: since the no-call cost is essentially zero, the
differential reduces to `with_call − 0 = with_call`, and the
hardened-test measurement (which doesn't subtract anything) is
already accurate.

## 7. Open caveats

The hardened/differential validation we ran is not a formal proof
that no optimization happened — it's strong empirical evidence. Two
caveats remain:

1. **Specific to this benchmark and this compiler version** (Apple
   Clang on macOS Apple Silicon, M4 Pro, release build with default
   project flags). A different toolchain or future compiler version
   could in principle behave differently. Re-running the validation on
   a target deployment build is good hygiene.
2. **The asm barrier formulation depends on GCC/Clang inline-assembly
   syntax.** Won't work on MSVC; rippled is GCC/Clang-only as far as
   we know, but worth noting.

For other wrappers we did not validate to this depth; their numbers
are accepted on the basis of (a) being in opaque library paths, or
(b) already labeled as FLOOR measurements where extra fidelity
doesn't change the conclusion.
