# Pickup prompt: WASM host-function gas pricing work (paused)

This is a paused investigation into whether WASM host functions in
rippled should charge gas dynamically based on input/output size,
instead of (or in addition to) the existing flat per-call gas schedule.
The branch is `supported_May_2_hf_fee` in
`/Users/pwang/CLionProjects/rippled`.

## What the work was about

Today every host function has a fixed gas cost charged per call. The
question was whether that flat schedule should be replaced (or
supplemented) by a size-dependent component, and if so, at what
slopes. We measured per-call cost across input/output sizes for the
size-sensitive wrappers (Blob field read, SHA-512 hash, signature
verify, `updateData`, `cacheLedgerObj`), documented the methodology
and its limits, and arrived at a deferred conclusion rather than a
calibration. The reason for the deferral is that the production cost
components that almost certainly dominate (SLE deserialization paired
with NuDB disk read, plus apply-time ledger persistence) are not
measurable in the available test environment.

## Files to read first (all at repo root)

- **`hf_fee_report_draft.md`** — the main report. Sections 1–3 cover
  cost sources, the benchmark tests and their coverage, and the
  measurement results plus limitations. Sections 4–6 are written
  deliberately as deferred conclusions; the reasoning for each
  deferral is in the section. Read this first.
- **`hf_fee_wip.md`** — branch-portable working notes (methods and
  results only, no references to the in-branch infrastructure
  changes). Useful if the report is being reviewed on a branch that
  doesn't carry the `chargeCall` / `chargeMem` / etc. helpers.
- **`volatile.md`** — appendix on defending the microbenchmark
  against cross-iteration compiler optimization. Three independent
  methodologies converged on the same `getCurrentLedgerObjField`
  slope, giving high confidence the headline numbers are real.
- **`rippled_optimization_wip.md`** — a standalone code-optimization
  proposal for the `get*Field` wrapper family (direct-write into
  wasm linear memory instead of going through an intermediate
  `Bytes`). Independent of the gas-pricing question; can be shipped
  on its own.

## Where the code changes live

In-branch infrastructure (all rates set to 0, no behavior change):

- `src/libxrpl/tx/wasm/HostFuncWrapper.cpp` — `chargeCall`,
  `chargeMem`, `chargeRead`, `chargeWrite`, `chargeCompute` helpers
  plus global rate constants `gasPerParamByte`, `gasPerReadByte`,
  `gasPerWriteByte`, `gasPerComputeUnit`. Call sites wired into
  ~20 wrappers.
- `src/test/app/HostFuncBenchmark_test.cpp` — manual BeastTest
  suite (`BEAST_DEFINE_TESTSUITE_MANUAL(HostFuncBenchmark, app, xrpl)`)
  exercising the real `WasmHostFunctionsImpl` against an in-memory
  `OpenView`. Six main testcases plus several investigative ones
  (commented out / not part of `run()`).

Run as `rippled --unittest=HostFuncBenchmark` (release build for
production-relevant numbers).

## What's deferred and why

The report explicitly declines two things and explains why in §3.2,
§4, and §5:

1. **Recommendation on dynamic vs fixed pricing.** The warm-loop
   in-memory slopes are real, but they cover only the floor of each
   wrapper's real production cost. The dominant components for the
   wrappers most likely to motivate dynamic pricing (`cacheLedgerObj`,
   `updateData`) live in code paths the test harness cannot reach
   (SLE deserialization, NuDB disk read, apply-time SHAMap update).
   Calibrating against the floor risks pricing the wrong term.
2. **Numeric calibration.** Deferred for the same reason. The
   infrastructure to charge per byte is in place with rates set to 0,
   so nothing has to be chosen until the underlying measurement
   question is settled.

There is also a methodological open question about warm-loop
measurement vs. production execution: all our measurements average
millions of calls in tight loops with the wrapper's code paths
L1/icache-warm. Production WASM executions touch each host function
a small number of times, in some cases after long idle gaps. Whether
and how to measure first-touch cost was opened as side research in a
different chat session; not resolved here.

## Related work (separate effort, not done by this session)

A different chat session reviewed the **current fixed host-fee
schedule** itself — i.e., are the existing per-call gas values
reasonable given what the wrappers actually do. The outputs of that
work are in the repo root as:

- `fee_review_prompt.md` — the prompt that drove that session.
- `fee_review_report.md` — the resulting review.

The work in this session is adjacent to but distinct from the fee
review: this session measured wrappers and asked whether per-byte
slopes are warranted; that session looked at the schedule's existing
fixed values and judged them in isolation. A future session
continuing the dynamic-pricing question should read both reports
before deciding whether to fold the two threads together (e.g., to
recommend a revised schedule that combines updated fixed values with
new per-byte slopes).

## Options for picking up

Three plausible directions, not mutually exclusive:

1. **Resolve the cold-path measurement question.** What does the
   literature say about measuring first-touch cost for short, rarely-
   invoked C++ paths inside a long-running process? Per-call icache
   flushing, fresh-process timing, or just accepting the warm-loop
   numbers as a steady-state proxy and pricing against that. Until
   this is resolved, the report's §4 and §5 deferrals remain.
2. **Push forward on calibration with warm-only numbers.** Treat the
   measured slopes as the best available data, accept the caveats in
   §3.2 explicitly, and propose initial non-zero rates for
   `gasPerParamByte` / `gasPerReadByte` / `gasPerWriteByte` /
   `gasPerComputeUnit`. The infrastructure is ready.
3. **Ship `rippled_optimization_wip.md` independently.** It does not
   depend on any pricing decision and stands on its own as a code
   improvement (fewer allocations + memcpys, plus genuine fast-fail
   on oversize requests).
