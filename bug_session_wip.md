# Bug-review session WIP

Working-state doc for a Claude Code session reviewing the Smart Escrow WASM
fee schedule on branch `supported_May_2_fee_ai_review`. Captures what's been
investigated, what's been settled, what's pending, and the locations of the
relevant code and artifacts so a fresh session can continue without
re-deriving anything.

The actual bug findings live in `fee_review_report.md` (separate file).
This doc is the *process* state, not the *findings* state.

---

## 1. Branch / scope

- Branch: `supported_May_2_fee_ai_review`. Confirmed by `git status` and
  reading source: only `checkGas` exists in `HostFuncWrapper.cpp` — no
  `chargeMem`/`chargeRead`/`chargeWrite`/`chargeCompute` infrastructure,
  no `gasPerXxxByte` constants.
- The schedule is **fixed-fee only** (per-host-fn entry cost in
  `createWasmImport()` in `src/libxrpl/tx/wasm/WasmVM.cpp:21–106`).
- A previous branch (`supported_May_2_hf_fee`) had per-byte / per-read /
  per-write / per-compute-unit infrastructure with all rates set to 0.
  The user said that branch will be dropped, so this review treats the
  per-byte infrastructure as out-of-scope.

## 2. Per-tx gas cap

- **Default cap: 1,000,000 gas.** Defined at
  `src/xrpld/core/Config.h:60`:
  `std::uint32_t extension_compute_limit{1'000'000};`
- Fee-voteable; lowered → smaller cap; 0 → WASM execution deactivated.
- Test env (`src/test/jtx/impl/envconfig.cpp:22`) also uses 1M.
- The original prompt (`fee_review_prompt.md`) stated **100K** as the cap.
  That was wrong. All severity reasoning in the report has been or
  needs to be re-graded against 1M.

## 3. WASM engine config — what's disabled

`WasmiEngine::init()` at `src/libxrpl/tx/wasm/WasmiVM.cpp:521–547`
disables, at engine config time:

- `bulk_memory` — no `memory.copy/fill/init`, `table.copy/fill/init`
- `reference_types` — no reftype tables
- `floats` — no FP instructions
- `multi_value`, `multi_memory`, `memory64`, `tail_call`,
  `extended_const`, `saturating_float_to_int`, `sign_extension`,
  `mutable_globals`, `wide_arithmetic`, `custom_page_sizes`

Enabled: `consume_fuel_set(true)` (wasmi's built-in fuel metering).
`MAX_PAGES = 128` (8 MB linear-memory cap) at
`include/xrpl/tx/wasm/WasmVM.h:21`.

`MAX_CACHE = 256` slots in `cacheLedgerObj` cache, at
`include/xrpl/tx/wasm/HostFuncImpl.h:16`.

`maxWasmDataLength = 4 KB`, `maxWasmParamLength = 1 KB` at
`include/xrpl/protocol/Protocol.h:255, 258`.

## 4. wasmi fuel model — investigated, no per-instruction override available

Inspected `crates/core/src/fuel.rs` in the local wasmi source
(`/Users/pwang/.conan2/p/wasmi55020a9284753/s/`). Key findings:

- wasmi 1.0.9 (matches `wasmi.h:WASMI_VERSION`).
- The `FuelCosts` trait has exactly **6 cost categories**:
  - `base()`, `load()`, `instance()`, `store()`, `call()`, `simd()`
  - Plus `bytes_per_fuel()` for bulk operations (default 64).
- Default values: every category returns 1 fuel; `bytes_per_fuel = 64`.
- **`call_indirect` rolls up under `call()`** — same fuel charge as a
  direct `call`. Not separately configurable.
- `FuelCostsProvider` can take a `custom: Option<Arc<dyn FuelCosts>>`
  in Rust, but this is **not exposed in the wasmi C API** (which is
  the project's only interface).
- `wasmi.h` (in conan cache) only has: `wasmi_config_consume_fuel_set`,
  `wasmi_context_set_fuel`, `wasmi_context_get_fuel`. No cost-customization.

**Consequence:** overriding the fuel cost of `call_indirect` (or any
single instruction) would require either (a) patching wasmi Rust to
split `call` into `call`/`call_indirect` categories and extending the
C API, or (b) writing a Rust shim around wasmi. Neither is in scope
for the schedule-level review.

## 5. Anchor calibration — current state

### 5.1 Two distinct regimes, not one anchor

A long thread early in the session: the user asked why we were
treating `getLedgerSqn` (a host fn) as the schedule-wide anchor. The
correct answer is that there are **two distinct gas-to-wall-clock
regimes**, and they have very different ns/gas values:

- **Host-function regime:** gas is whatever the schedule's
  `createWasmImport()` registration assigns. `getLedgerSqn` at 60 gas
  / ~6 ns measures **0.10 ns/gas**.
- **WASM-instruction regime:** wasmi's internal fuel model charges
  ~0.36–0.55 fuel per WASM opcode on average. Measured (see §5.2)
  ranges from **0.59 to 1.84 ns/gas** across six kernel shapes.

The schedule today is internally inconsistent across these regimes —
1 gas of host-fn work costs ~8–18× less wall-clock than 1 gas of
interpreted WASM work. The user's plan: settle the interpreter
anchor first (this section), then re-derive host-fn prices against
it.

### 5.2 Six kernels measured (release build, M4 Pro)

New test `testInstructionAnchor()` at
`src/test/app/HostFuncBenchmark_test.cpp` runs WAT-compiled kernels
through the actual `WasmEngine::instance().run()` (unlike the rest
of that file, which bypasses the engine). Uses a differential-timing
trick: median of 11 repeats at two iteration counts (100K and 1M),
subtract to cancel engine setup, divide ns/iter by gas/iter.

| # | Kernel | What it stresses | ns/gas |
|---|---|---|---|
| 1 | mixed-opcode | i-cache-friendly floor | 0.59–0.63 |
| 2 | memory-scatter | LCG load/store across 128 KB; d-cache | 0.64–0.66 |
| 3 | branch-heavy | unpredictable data-dependent branches | 0.77–0.82 |
| 4 | diverse-opcode | ~17 ops/iter + div_u + rem_u + 1 call | 0.91–0.95 |
| 5 | call_indirect | 8-way random table dispatch | **1.84–1.88** ← worst |
| 6 | combined-worst | call_indirect + 4 params + div_u/rem_u in callee | 1.65 |

Run-to-run variance ~2% on the worst kernel; numbers reproduce.

WAT source for each kernel is committed in the test file (commented
above each `static constexpr char k*Hex[]`). The pre-compiled WASM
hex strings are embedded as constants.

Output format (printed via `log <<`):
```
=== WASM instruction anchor (wasmi engine, 6-kernel bracket) ===
  --- kernel: mixed-opcode (baseline / i-cache friendly) ---
       iters | median ns  | gas consumed
    ...
    ns/iter (diff):   4.73
    gas/iter (diff):  8.00
    ns/gas:           0.5906
```

### 5.3 Why call_indirect is the outlier

wasmi charges 1 fuel for the `call_indirect` instruction itself (under
the `call()` category). The native code for `call_indirect` does table
bounds check + runtime type check + frame setup + dispatch + return —
~12–15 ns of native execution.

The **kernel-5 ratio of 1.84 ns/gas is a weighted average across all
~10 fuel charged per iteration**, of which the call_indirect
subsystem (push index + call + tiny callee + return) accounts for
roughly **~2–3 fuel for ~13–15 ns of wall-clock**, i.e. ~5–6 ns/fuel
intrinsically.

### 5.4 Why combined-worst (kernel 6) is LOWER than plain call_indirect

Adding div_u/rem_u in the callee + 4-param marshaling **increases gas
charged faster than wall-clock**, diluting the ratio. Costs don't
compound; the worst kernel-level ratio is reached when call_indirect
is *isolated* (kernel 5), not stacked with other expensive ops.

### 5.5 Asymptotic ceiling

A maximally call_indirect-dense kernel — many `call_indirect`s in a
row inside one loop body, amortizing loop control over N calls — would
approach the per-fuel cost of the call_indirect subsystem (~5–6 ns/gas).

The kernel-5 ratio of 1.84 captures only N=1 per loop iter. We have
**not yet measured a high-N kernel**; that's the open data point. The
user's analytical estimate (based on kernel-5 decomposition) is the
ceiling sits at ~5–6 ns/gas.

### 5.6 Anchor decision — pending

The user has not yet committed. Three options on the table:

- **Tight: 2.0 ns/gas.** ~6% margin over kernel-5 worst of 1.84/1.88.
  Vulnerable to a denser call_indirect kernel exceeding the anchor.
- **Safe: 3.0 ns/gas.** ~60% margin. Covers most realistic attacker
  kernels but not the asymptote.
- **Asymptote-covering: 5–6 ns/gas.** Covers a maximally-dense
  call_indirect kernel. Pure compute pays ~10× over real cost.

Open question that would settle the choice: **what ns/gas does a
dense constant-index call_indirect kernel actually hit?** (Kernel 7
to add, if we keep iterating.) Last-message option offered to user;
they paused to log session state first.

### 5.7 Calibration caveats (apply to ALL ns/gas numbers)

1. All measurements are **floors** — fixed bytecode that fits in
   L1 i-cache, hot after warmup. Production WASM with larger code
   working sets could be slower.
2. Hardware: measurements on M4 Pro release build. Production
   reference is AMD Threadripper. Pure-CPU interpreter dispatch
   should agree within ~2× (per prompt). Memory-bandwidth-heavy
   ops or NVMe paths are not directly portable.
3. wasmi version 1.0.9. Future versions may change per-IR fuel
   weights; re-measure.

## 6. Confirmed: host-fn measurement floors

For wrappers that DO go through the bypass-engine harness (the
original `testGetLedgerSqnBaseline` and similar in the same file):
the harness measures wrapper-internal cost only. Production
execution adds wasmi import-call boundary overhead on top
(~30–100 ns per call). The user is aware; the report mentions this
in the appendix.

## 7. getNFT cost-shape — re-analyzed

The original report claimed getNFT does "SHAMap successor walk + page
read + scan" implying multiple disk reads per call. **Re-examination
of `src/libxrpl/ledger/helpers/NFTokenHelpers.cpp:520–537` and the
`Ledger::succ` impl at `src/libxrpl/ledger/Ledger.cpp:372–380` shows
the disk-read shape is the same as cacheLedgerObj.**

- `view.succ()` is `stateMap_.upper_bound(key)` — one SHAMap traversal.
- The subsequent `view.read(located_keylet)` shares the SHAMap descent
  path with `succ`, so it doesn't cost an additional cold read in the
  typical case.

So getNFT does **1 cold leaf read** (like cacheLedgerObj) **plus**
~10–20 μs CPU for page deserialization and the linear scan of up to
32 NFTokens. cacheLedgerObj has zero post-read CPU.

### Adversarial worst case for getNFT

If an attacker can target an account whose NFTokenPage SHAMap
subtree has cold inner nodes (recently-untouched ledger region),
the descent path could include ~3–5 cold inner-node reads in
addition to the leaf. Total ~4–6 cold disk reads per call,
worst case ~8.

This adversarial worst case applies to cacheLedgerObj too; it's
not unique to getNFT. The real Bug #2 finding is that getNFT is
priced 5× cheaper than cacheLedgerObj for comparable work.

## 8. Open items / next steps

In rough priority:

1. **Settle the interpreter anchor.** Either commit on 2.0/3.0
   ns/gas or add a kernel 7 (dense constant-index call_indirect)
   to measure the asymptotic ceiling.
2. **Re-derive host-fn prices against the chosen anchor.** Each
   host fn's wall-clock floor × anchor = target gas. Re-tune the
   `createWasmImport()` table. Disk-touching wrappers need NVMe
   physics, not the host-fn floor.
3. **Update report with corrected 1M gas cap throughout.** Severity
   tables, attack arithmetic for bugs #1, #2 all multiply by 10×.
4. **Soften Bug #2 framing.** The original claim of "100–300 μs per
   call × 1000 calls = 100–300 ms per tx" overstated the typical
   case (1 cold read, not multiple). Adversarial case (cold SHAMap
   descent) still gives Critical wall-clock.
5. **Investigate adversarial cold-page construction for getNFT.**
   Is it actually possible to engineer ledger state where an
   attacker can force the succ walk to traverse many cold pages?
   (Open question; user noted it.)

## 9. Files to read on session pickup

Code:
- `src/libxrpl/tx/wasm/WasmVM.cpp:21–106` — full gas table.
- `src/libxrpl/tx/wasm/WasmiVM.cpp:521–547` — wasmi config.
- `src/libxrpl/tx/wasm/HostFuncWrapper.cpp:380–420` — `checkGas`.
- `src/libxrpl/tx/wasm/HostFuncImplGetter.cpp:222–249` — cacheLedgerObj.
- `src/libxrpl/tx/wasm/HostFuncImplNFT.cpp:12–31` — getNFT.
- `src/libxrpl/ledger/helpers/NFTokenHelpers.cpp:22–33, 520–537` —
  `locatePage` and `findToken`.
- `src/libxrpl/ledger/Ledger.cpp:372–380` — `Ledger::succ` impl.
- `src/xrpld/core/Config.h:60` — default 1M gas cap.

Test harness:
- `src/test/app/HostFuncBenchmark_test.cpp` — manual benchmark.
  Run with `rippled --unittest=HostFuncBenchmark` (release build).
  Six kernels in `testInstructionAnchor()`.

External:
- `/Users/pwang/.conan2/p/wasmi55020a9284753/s/src/crates/core/src/fuel.rs`
  — wasmi's FuelCosts trait.
- `/Users/pwang/.conan2/p/b/wasmifcd47aeedb5c3/p/include/wasmi/` —
  wasmi C API headers.

Session artifacts:
- `fee_review_prompt.md` — original review prompt (note: states 100K
  cap which is wrong).
- `fee_review_report.md` — bug findings (separate from this doc).
- `wip.md`, `volatile.md`, `todos.md` — measurement notes from prior
  session; useful but not all numbers reproducible until re-verified.
- `report_layout.md` — outline for a separate measurement report.
