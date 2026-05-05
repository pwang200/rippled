# Host-function gas cost: measurement and analysis

## Preamble

This report covers gas pricing for WASM **host function wrappers** in
rippled — the C++ shims that sit between WASM-imported function calls and
the rippled host implementations. Concretely, the wrappers in
`src/libxrpl/tx/wasm/HostFuncWrapper.cpp` (e.g. `getLedgerSqn_wrap`,
`getCurrentLedgerObjField_wrap`, `cacheLedgerObj_wrap`,
`computeSha512HalfHash_wrap`, `checkSignature_wrap`, `updateData_wrap`).

**Problem this report tries to answer.** Today each host function has a
fixed gas cost charged per call, regardless of how much data the call
processes. The question is whether that flat schedule should be replaced
(or supplemented) by a size-dependent component, and if so, at what
slopes. The report measures per-call cost across input/output sizes for
the size-sensitive wrappers, documents the methodology and its limits,
and arrives at a deferred conclusion (see §4 and §5) rather than a
calibration recommendation, because the most important production cost
components are not measurable in the available test environment.

By "cost" we mean real wall-clock work the network does on a single
wrapper invocation. We use `getLedgerSqn` (gas = 60) at its measured
release-build cost as the calibration anchor, so 1 gas unit ≈ 0.10 ns.

Out of scope:
- WASM engine / VM-instruction gas (the per-instruction gas charged by
  the wasm engine itself).
- Cold disk I/O on production NuDB (test environment uses an in-memory
  NodeStore — see §3.2 Limitations).
- Real ledger persistence cost at apply time (happens after WASM
  execution; the wrapper only stages — see §1 Group C).

---

## 1. Sources of cost

A host-function wrapper call can do several distinct kinds of work, each
with different scaling and observability characteristics. We group them
by **who pays when**, because that division drives both how the cost is
measurable and where in the gas model it should live.

### Group A — CPU-bound, paid during the host-function call

These are real costs the network pays inside the wrapper invocation
itself. They are CPU-bound, in-memory, and (with appropriate setup)
measurable in unit tests.

1. **Memory read (wasm-linear-memory slice access).**
   The wrapper's `getDataSlice` (and the family `getDataAccountID`,
   `getDataUInt256`, etc.) constructs a non-owning `Slice` over a region
   of wasm linear memory. No memcpy happens; the Slice is just a (ptr,
   len) view. Cost is essentially the call overhead and bounds check —
   a few nanoseconds, independent of the slice's byte length.

2. **Memory allocation.**
   Several impl paths allocate a host-side `Bytes` (`std::vector<uint8_t>`)
   or other heap-backed object per call:
   - `getAnyFieldData` in the `get*Field` family allocates a `Bytes` for
     the returned field bytes.
   - `WasmHostFunctionsImpl::updateData` allocates a `Bytes` to stage the
     input (`data_ = Bytes(data.begin(), data.end())`).
   - Trace/float wrappers materialize `STAmount` / `STNumber` objects
     from input slices (allocations within those types' constructors).
   - `checkSignature` allocates a `PublicKey` / signature buffer.

   For small allocations (≤ a few KB) on a modern allocator's fast path,
   per-allocation cost is roughly fixed (~5–10 ns), independent of size.

3. **Memory copy.**
   Byte-proportional memcpy at L1 cache speed (~50–100 GB/s on M4 Pro).
   Three different memcpy events appear in the wrappers:
   - SLE field bytes → host-side `Bytes` (in `getAnyFieldData`).
   - Slice → host-side `Bytes` (in `updateData`).
   - Host-side `Bytes` → wasm linear memory (in `setData`, the final
     boundary copy).
   Cost scales linearly with byte count. At small sizes the per-call
   memcpy overhead (alignment dispatch, etc.) dominates; at sizes ≥ a
   few hundred bytes the linear term dominates.

4. **SHA-512 hashing.**
   Used by `compute_sha512_half` directly and internally by
   `check_sig` (which hashes the message before curve ops). The hashing
   kernel is in libcrypto, so it runs at the same speed in debug and
   release builds (~0.5–0.6 ns/byte). Linearly scales with input bytes.

5. **Signature verification.**
   `check_sig` invokes `verify(PublicKey, message, signature)`, which
   runs ECDSA secp256k1 verification — two scalar multiplications on
   the curve. The curve operations are constant-time and dominate
   (~15 µs in release per verify). The per-byte component from the
   internal SHA-512 hash is small relative to the curve op.

6. **SLE deserialization.**
   When `Ledger::read(keylet)` (or `OpenView::read`) is called for a
   keylet whose SLE is not yet cached, it pulls the serialized bytes
   from the SHAMap and constructs an `SLE` via
   `std::make_shared<SLE>(SerialIter{slice}, key)`. The constructor
   walks every field and builds the in-memory STObject. Cost scales
   roughly with the SLE's serialized size. After the first read, the
   resulting `shared_ptr<SLE const>` is cached, so subsequent reads of
   the same keylet skip deserialization.

   For our wrappers, this cost only appears on the **first**
   `cacheLedgerObj` call for a given keylet during a transaction;
   subsequent reads (including all `get*Field` calls on the cached
   slot) skip deserialization.

   In production this cost almost always occurs *paired with* item 7
   (disk read) — the SHAMap traversal pulls serialized bytes from
   NuDB (paying disk-read latency), and the SLE constructor then
   deserializes those bytes immediately. The two are sequential and
   hard to separate empirically; on a cold NuDB cache the disk-read
   portion typically dominates the combined cost. Together they are
   the single largest unmeasurable item in this report.

### Group B — I/O-bound, paid during the host-function call

7. **Disk read for cold ledger object.**
   When `cacheLedgerObj` is called for a keylet whose serialized bytes
   aren't already in NuDB's cache or the OS page cache, the SHAMap
   traversal triggers a disk read. Production NuDB on SSD typically has
   ~10–100 µs random-read latency; on slow validators or high-load
   conditions this can climb. This cost can be 10–1000× the in-memory
   floor we measure.

   **Critically: this cost is NOT measurable in our test environment.**
   Tests use the in-memory NodeStore, so disk reads never happen. The
   floor we report is the steady-state cache-hit cost only.

### Group C — Deferred, paid after WASM execution

8. **Ledger persistence (data field write at apply time).**
   `updateData` does not persist data to the ledger. It stages the
   input into `WasmHostFunctionsImpl::data_`, a host-side member. At
   apply time — *after* WASM execution finishes — the transaction
   processor reads `data_` and modifies the relevant ledger object,
   which then flows through the SHAMap update + NuDB write path.

   The "real cost" of `updateData` from the network's perspective
   includes the apply-time work: SHAMap node updates, hash recomputation
   up the tree, and an eventual NuDB write. None of that runs inside
   the wrapper. **Our measurement captures only the staging step
   (alloc + memcpy of the input).**

---

## 2. The tests, and which costs they cover

The benchmark suite (`src/test/app/HostFuncBenchmark_test.cpp`,
`BEAST_DEFINE_TESTSUITE_MANUAL(HostFuncBenchmark, app, xrpl)`) contains
six **main** testcases, listed below. Each invokes one wrapper
repeatedly inside a tight inner-loop timing pattern with statistical
sampling. The suite also contains additional investigative testcases
(field-type variations of the get-field test, plus hardened and
differential validation runs); those help interpret and validate the
main results but do not introduce new cost-source coverage of their
own.

Cross-iteration compiler optimization is a real concern with this
measurement pattern and is addressed and validated separately. See
the **Appendix** (`volatile.md`) for the methodology and validation
results.

The harness exercises the **real `WasmHostFunctionsImpl`** against an
in-memory `OpenView` populated by `rawInsert` of synthetic SLEs that
are constructed directly in memory (never serialized in the first
place). The WASM engine itself is not invoked. This setup has two
direct consequences for coverage:

- **Cost source 6 (SLE deserialization) does not run in any test.**
  Because each SLE is constructed in memory via
  `std::make_shared<SLE>(keylet)` and `rawInsert`-ed into the view,
  there is no serialized form for `OpenView::read` to deserialize.
- **Cost source 7 (disk read) does not run either**, because the
  NodeStore configured by jtx is the in-memory backend, not NuDB.

For each main testcase, this section lists the wrapper it exercises,
which cost sources from §1 it touches, and a coverage label:

- **Complete** — the dominant in-wrapper cost is captured.
- **Floor** — the in-memory portion is captured but real cost
  includes unmeasured I/O or post-execution work.
- **Anchor** — the test exists to set the gas-vs-time calibration,
  not to measure a specific cost source.

### 2.1 `testGetLedgerSqnBaseline`

- **Wrapper**: `getLedgerSqn_wrap`.
- **What it does**: invokes the wrapper in a tight loop with no
  variable input; the impl returns a single `uint32_t` from
  `ctx_.view().seq()`.
- **Cost sources touched**: the wrapper's fixed call overhead
  (entry plumbing + virtual dispatch + a 4-byte write back to wasm
  memory). No items from §1 dominate; the cost is essentially the
  irreducible per-call plumbing.
- **Coverage**: anchor. The measurement defines the gas-vs-time
  conversion (`1 gas ≈ T ns`) used throughout the rest of the report.

### 2.2 `testGetCurrentLedgerObjFieldSweep` (sfData / Blob)

- **Wrapper**: `getCurrentLedgerObjField_wrap`.
- **What it does**: builds a synthetic escrow SLE with `sfData` of
  size N, sweeps N over {32, 128, 512, 1024, 4096}.
- **Cost sources touched**: Group A items **2 (memory allocation)**
  and **3 (memory copy)** on the output path —
  `getAnyFieldData` allocates a `Bytes` and copies field content into
  it; the wrapper then memcpys those bytes into wasm linear memory
  via `setData`.
- **Coverage**: complete for items 2 and 3 on the Blob field path.
  Provides slope and intercept for the per-byte output cost.

### 2.3 `testCacheLedgerObjSweep`

- **Wrapper**: `cacheLedgerObj_wrap`.
- **What it does**: builds synthetic escrow SLEs with `sfData` of size
  {32, 128, 512, 1024, 4096}, calls `cacheLedgerObj` repeatedly with
  `cacheIdx=1` so each call overwrites cache slot 0.
- **Cost sources touched**: Group A item **1 (memory read)** for the
  keylet bytes plus the cache-slot bookkeeping inside the impl.
  Item **6 (SLE deserialization)** does not run for the reason given
  at the top of this section; Group B item **7 (disk read)** does not
  run because tests use the in-memory NodeStore.
- **Coverage**: **floor**. We measure the steady-state cache-resident
  path only — neither deserialization nor disk I/O is exercised.
  Real production cost on a cold NuDB cache can be 10–1000× this
  number.

### 2.4 `testUpdateDataSweep`

- **Wrapper**: `updateData_wrap`.
- **What it does**: sweeps input size {32, 128, 512, 1024, 4096}
  (max = `maxWasmDataLength`). Each call invokes
  `WasmHostFunctionsImpl::updateData`, which reassigns
  `data_ = Bytes(slice.begin(), slice.end())`.
- **Cost sources touched**: Group A items **1 (memory read)**,
  **2 (memory allocation)**, **3 (memory copy)** — the staging path.
  Group C item **8 (ledger persistence)** is **not** triggered; the
  actual write happens at apply time, after WASM execution.
- **Coverage**: **floor**. Captures only the staging step; real
  apply-time persistence cost is unmeasured.

### 2.5 `testComputeSha512HashSweep`

- **Wrapper**: `computeSha512HalfHash_wrap`.
- **What it does**: sweeps input size {32, 128, 512, 1024,
  4096-overlimit}; each call hashes the input slice (read in place
  from wasm memory) and writes the 32-byte output back.
- **Cost sources touched**: Group A items **1 (memory read)** —
  trivially, because the SHA-512 kernel reads the slice in place;
  **3 (memory copy)** for the 32-byte output write; and most
  importantly **4 (SHA-512 hashing)**, which dominates the per-byte
  slope. The hashing kernel is in libcrypto, so debug and release
  builds give nearly identical slopes.
- **Coverage**: complete.

### 2.6 `testCheckSignatureSweep`

- **Wrapper**: `checkSignature_wrap`.
- **What it does**: pre-generates a secp256k1 keypair, then for each
  message size {32, 128, 512, 1024} measures both a "good" sig (valid
  for the message) and a "bad" sig (valid for a *different* message
  of the same size — well-formed, full verify path, returns 0). Plus
  an over-limit row for the wrapper's rejection cost.
- **Cost sources touched**: Group A items **1 (memory read)** for
  three input slices; **2 (memory allocation)** for `PublicKey` and
  signature buffer construction; **5 (signature verification)** —
  curve ops dominate; and **4 (SHA-512 hashing)** of the message
  internally. Cost is largely flat in N because the ECDSA curve op
  dominates.
- **Coverage**: complete.

### 2.7 Coverage map (cost source → tests)

| §1 cost source | Group | Measured by | Status |
|---|---|---|---|
| 1. Memory read | A | Implicitly in every test (slice reads, keylet reads) | Complete |
| 2. Memory allocation | A | 2.2 (Bytes for field), 2.4 (Bytes for staged data), 2.6 (key/sig buffers) | Complete |
| 3. Memory copy | A | 2.2 (output), 2.4 (input), 2.5 (output) | Complete |
| 4. SHA-512 hashing | A | 2.5 directly; 2.6 indirectly via internal message hash | Complete |
| 5. Sig verification | A | 2.6 | Complete |
| 6. SLE deserialization | A | (none — synthetic SLEs are built in memory and never serialized) | **Not measured** |
| 7. Disk read for cold ledger object | B | (none — in-memory NodeStore) | **Not measured** |
| 8. Ledger persistence at apply | C | (none — happens after WASM execution) | **Not measured** |

The three "not measured" rows are exactly where §3.2 will flag the
limits of what these numbers tell us.

---

## 3. Measurement results

All numbers in this section are from a release build of rippled,
running the benchmark suite on Apple M4 Pro / macOS, using the
methodology of §2 and validated as described in the appendix.

The baseline measurement (§2.1, `getLedgerSqn`) gave **6 ns per
call**. This is the irreducible per-call wrapper plumbing for a
trivial host function — it has no size dependence — and serves as
the floor any wrapper has to pay before it does any actual work.

### 3.1 Per-test measurements (raw)

Rows are the six main tests from §2. Cells are median ns per call at
the given size. "over limit" means the size exceeds the wrapper's
hard input limit (1 KB for SHA-512 and signature verification); the
wrapper rejects without doing the underlying work. **FLOOR** rows
report only the in-memory portion; real production cost includes
unmeasured I/O or apply-time work (see §3.2).

| Test | 32 B | 128 B | 512 B | 1 KB | 4 KB |
|---|---|---|---|---|---|
| 2.1 `getLedgerSqn` baseline (size-independent) | 6 | 6 | 6 | 6 | 6 |
| 2.2 read variable-length field (Blob) | 35 | 27 | 37 | 47 | 95 |
| 2.5 hash with SHA-512 | 79 | 129 | 325 | 587 | over limit |
| 2.6 verify a signature — good (valid for the message) | 15,216 | 14,429 | 13,975 | 14,837 | over limit |
| 2.6 verify a signature — bad (well-formed, wrong message) | 14,220 | 13,812 | 14,300 | 14,429 | over limit |
| 2.3 cache a ledger object — **FLOOR** | 14 | 11 | 13 | 15 | 15 |
| 2.4 stage data for update — **FLOOR** | 25 | 22 | 31 | 37 | 57 |

We deliberately do **not** decompose these per-test numbers into
per-cost-source contributions in this report. Decomposing the
~25 ns wrapper intercept into separate "memory read" and "memory
allocation" contributions, or attributing residual ns to plumbing,
turned out to require enough guesswork that the resulting table
would have falsely implied a precision the measurements do not
support. The raw per-test numbers above are what the benchmark
actually measures.

The Blob-read row also shows a small inversion (35 ns at 32 B,
27 ns at 128 B) that is a fixture cold-start effect: the first
sweep size after fixture setup pays ~7–9 ns extra per call,
confirmed by reversing the sweep order. The slope between
the larger sizes (512 / 1024 / 4096 B) is the trustworthy linear
term.

### 3.2 Limitations

The headline numbers are subject to several caveats — important to
keep in mind when reading later sections.

**Unmeasurable cost sources.** The single largest gap is items 6 and
7 from §1 (SLE deserialization paired with NuDB disk read). These
dominate the real cost of caching a ledger object in production but
cannot be reproduced in this test environment, where the NodeStore
is in-memory and synthetic SLEs are constructed without ever being
serialized. Item 8 (ledger persistence at apply time) similarly
escapes the wrapper's timing window for `updateData`. The
`cacheLedgerObj` and `updateData` rows in §3.1 are therefore reported
as FLOOR.

**Warm-loop vs cold-path.** All measurements average ~1000 wrapper
calls per timing sample, ~1000 samples per data point. Even the
first iteration after fixture setup runs in a context where the
wrapper's code paths are L1/icache-warm. Production WASM executions
invoke each host function a small number of times, potentially after
long gaps where the wrapper has fallen out of cache. Production
cold-path cost (per-call branch-predictor and icache cost on first
touch) is **not measured here**. Cold-path measurement requires
either per-call cache flushing or fresh-process timing; neither is
in scope for this report.

**Test environment.** Single machine, single thread, no validator
network, no concurrent load. Synthetic SLEs use the LedgerEntry's
full template (so all required + optional fields with default values
are present) plus our `sfData` override; structurally similar to a
production escrow at the same payload size, but byte distribution
will differ.

**Hardware and build.** Apple M4 Pro on macOS, release build with
default project compiler flags. The slopes for memory-copy and
allocation paths reflect ARM64 + NEON vectorized memcpy at L1 cache
speeds (~80–120 GB/s). Debug-build numbers are 5–10× larger for any
rippled C++ code path; for opaque library code (libcrypto,
libsecp256k1) the debug and release slopes are nearly identical.
Release-build numbers are what we use here, since they reflect the
production deployment.

**Run-to-run variance.** On the laptop used for measurement, IQR per
sample is typically single-digit % but occasionally rises to 10–25%
during background activity. Medians are stable across runs to within
~10%; the baseline's `getLedgerSqn` varied between 5 and 6 ns/call
across runs.

**Compiler optimization.** Cross-iteration optimization in tight
identical-input loops was a concrete concern. For the variable-field
read (Blob) we ruled it out via three independent methodologies
converging on the same slope and intercept; details in appendix
`volatile.md`. Other operations were not validated to that depth —
they're either in opaque library code (where elision is not
plausible) or labeled FLOOR (where extra measurement fidelity does
not change the conclusions).

---

## 4. Dynamic vs fixed gas pricing

**Question.** Should host functions charge gas based on input/output
size, instead of (or in addition to) a flat per-call cost?

**What the measurements show.** Among the wrappers measured, four
have a meaningful size-dependent component in warm-loop measurement:

- `getCurrentLedgerObjField` (Blob): 35 → 95 ns over 32 B → 4 KB
  (~2.7×).
- `computeSha512HalfHash`: 79 → 587 ns over 32 B → 1 KB (~7.4×; max
  input is 1 KB for this wrapper).
- `updateData`: 25 → 57 ns over 32 B → 4 KB (~2.3×, FLOOR).
- `cacheLedgerObj` (FLOOR): essentially flat in warm measurement —
  but the real-cost components (items 6 + 7 in §1) scale with
  serialized SLE size and were not measured.

`checkSignature` is effectively flat in N because the curve operation
dominates and is roughly constant-time per verify.

These slopes, taken at face value, would argue for per-byte gas on
SHA-512 (largest relative variation), and at least a per-byte
component on the Blob read and update-data paths. However:

**Why we are not recommending a calibration here.** The slopes we
measured cover only the in-memory, cache-warm, in-test-process portion
of each wrapper's real cost. The two largest cost components for the
two wrappers most likely to motivate dynamic pricing —
`cacheLedgerObj` (SLE deserialize + disk read) and `updateData`
(apply-time SHAMap update + NuDB write) — are **not in our numbers
at all**. Adopting a dynamic schedule based on the in-memory slopes
alone risks calibrating against the wrong dominant term: a
~50 ns/call wrapper memcpy is the floor under a 10–100 µs production
operation, and per-byte pricing fitted to the floor will not reflect
what the network actually pays.

There is also a methodological question about warm-loop measurement
vs. production execution. Production WASM executions touch each host
function a small number of times, in some cases after long idle
gaps; the warm-loop numbers reported here do not characterize the
first-touch cost in that regime. Whether and how much this matters
for a fairness-of-pricing argument is open.

**Conclusion (deferred).** This report does not recommend
adopting dynamic gas pricing on the basis of the measurements
presented. A meaningful recommendation needs (a) a measurement
methodology that captures the unmeasured cost sources (or a
defensible model for them), and (b) a position on whether gas
should be calibrated against warm-loop steady state or first-touch
production cost. Both are out of scope for this round.

---

## 5. Calibration

This section is deferred for the same reason as §4. Without a
measurement that captures items 6, 7, and 8 from §1 — or a defensible
model for them, plus a position on warm vs cold pricing — any
numerical calibration emitted here would be calibrating against a
floor rather than against real network cost. The infrastructure for
size-based charging (the `chargeMem` / `chargeRead` / `chargeWrite`
/ `chargeCompute` helpers and global rate constants in
`HostFuncWrapper.cpp`) is in place and wired through ~20 wrappers
with all rates set to 0, so no calibration constants need to be
chosen before that follow-up work has settled which costs the
schedule should reflect.

---

## 6. Optimization opportunities

One concrete optimization surfaced while studying the wrappers — it
is not required for any gas-pricing decision, but is worth recording
because it reduces actual work the network pays.

**Direct-write refactor for `get*Field` family.** Today every
`getCurrentLedgerObjField`-family wrapper goes through a two-step
output path: `getAnyFieldData` allocates a host-side `Bytes`,
copies the field bytes into it, and returns it; the wrapper then
copies that `Bytes` into wasm linear memory via `setData`. The
intermediate `Bytes` allocation and the first memcpy are avoidable
— the field bytes could be written directly into wasm linear memory
in a single copy. Affected wrappers and their input-side details are
listed in `todos.md`.

The expected per-call saving from this refactor is in the ~5–10 ns
range plus one of the two memcpys (i.e., halving the per-byte slope
on the output path, where it currently shows up). Not large in
absolute terms relative to the unmeasured cost sources discussed
in §3.2, but a strictly-positive correctness/efficiency win that
does not depend on resolving the calibration question.

---

## Appendix

- `volatile.md` — Defending microbenchmarks against compiler
  optimization. Documents the three-layered defense (volatile sink,
  asm-volatile compiler barrier, per-iteration input variation) and
  the differential measurement that together gave three-way
  convergence on the `getCurrentLedgerObjField` slope and intercept.
- `hf_fee_wip.md` — Working notes (methods + results only) intended
  to be portable across branches that do not contain the
  `chargeCall` / `chargeMem` etc. infrastructure changes.
- `todos.md` — Followups identified during the measurement work,
  primarily the `get*Field` direct-write refactor referenced in §6.