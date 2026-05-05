# WIP: WASM host-function cost measurement

Working notes capturing what was measured and how. Methods and
results only.

## 1. Scope of measurement

Six WASM host-function wrappers were measured:

- `getLedgerSqn` — calibration anchor (size-independent).
- `getCurrentLedgerObjField` — variable-length Blob field read.
- `cacheLedgerObj` — ledger-object cache load (in-memory portion only).
- `updateData` — host-side staging of input bytes (apply-time write
  not measured).
- `computeSha512HalfHash` — SHA-512 hash of input slice.
- `checkSignature` — ECDSA secp256k1 signature verification.

For each wrapper, per-call wall-clock cost was measured at a sweep of
input or output sizes: `{32, 128, 512, 1024, 4096}` bytes when the
wrapper's hard input limit permits 4 KB, otherwise `{32, 128, 512,
1024}` with an additional over-limit point to characterize the
rejection-path cost.

## 2. Measurement setup

Each wrapper was invoked directly from C++ — not through the WASM
engine — by constructing its parameter vector and a stub runtime by
hand and calling the wrapper function pointer. The host side used
the real `WasmHostFunctionsImpl` against an in-memory `OpenView`
populated by `rawInsert` of synthetic SLEs constructed in memory.
This isolates wrapper-level CPU cost from WASM dispatch overhead.

Additional investigative measurements on `getCurrentLedgerObjField`:

- Cross-type comparison: the wrapper was measured against a UInt32
  field and a Hash256 field at their fixed sizes.
- A hardened variant of the Blob sweep that adds per-call compiler
  barriers and per-iteration cycling of the output pointer through
  multiple distinct destinations.
- A differential measurement: the same inner loop with vs. without
  the wrapper call, subtracted to confirm that the measured value
  reflects wrapper cost rather than harness overhead.

## 3. Timing methodology

Each timing sample is the elapsed time of N consecutive wrapper calls
(N=1000), divided by N. Many such samples (~1000) are collected per
data point; the median is reported with IQR.

Inner-loop amortization is necessary because
`std::chrono::steady_clock` on macOS Apple Silicon has roughly 30–45 ns
granularity per `now()` call — single-call timing would be lost in
clock noise.

Three layers of defense against compiler optimization across the
tight inner loop:

- A `volatile`-qualified accumulator XOR'd with the wrapper's output
  and result code on every iteration.
- For the most measurement-critical wrapper
  (`getCurrentLedgerObjField`), additionally `asm volatile("" : :
  "r,m"(value) : "memory")` compiler barriers (the same primitive
  Google Benchmark's `DoNotOptimize` uses) plus per-iteration cycling
  of the output pointer through 16 distinct destinations in the
  host-side memory buffer.
- A differential measurement that runs the same inner loop without
  the wrapper call, then subtracts to isolate the wrapper's
  contribution from any harness overhead.

## 4. Observations on methodology

- **Three-way agreement** on the `getCurrentLedgerObjField` slope and
  intercept. The volatile-XOR sink, the hardened
  asm-barrier-plus-pointer-cycling variant, and the differential
  call-vs-no-call subtraction all give the same numbers within
  run-to-run noise. Compiler elision is not affecting the headline
  numbers for this wrapper.

- **Cold-start effect**: the first timed measurement after fixture
  setup is consistently 7–9 ns higher per call than subsequent ones.
  Whichever sweep size runs first eats this penalty. Confirmed by
  reversing the sweep order. The reported per-call cost averages
  millions of calls per size, so the cold-start contribution to the
  median is small in absolute terms — but the resulting numbers are
  tight-loop warm, not cold.

- **Production cold-path is not directly measured.** Production WASM
  executions invoke each host function a small number of times,
  potentially after long gaps where the wrapper's code paths have
  fallen out of L1/icache. Cold-path measurement would require
  per-call cache flushing or fresh-process timing; neither is in
  scope here.

- **In-memory NodeStore** means no NuDB disk I/O is exercised, and
  SLEs are never serialized (they are constructed in memory and
  inserted directly into the view). Three cost components are
  consequently unmeasured: SLE deserialization, disk-read latency
  for cold ledger entries, and ledger persistence at apply time.

- **Hardware and build**: Apple M4 Pro, macOS, release build. Debug-
  build slopes for any rippled C++ code path are 5–10× larger;
  slopes for opaque library code (libcrypto, libsecp256k1) are
  nearly identical across builds.
