# Smart Escrow WASM Fee Schedule — Review Report

**Branch reviewed:** `supported_May_2_fee_ai_review` (no per-byte / dynamic
gas infrastructure; all host-function charges are fixed-fee, per-call only).

**Scope:** Per-host-function gas prices registered in `createWasmImport()`
(`src/libxrpl/tx/wasm/WasmVM.cpp`), the host-function implementations in
the same folder, the `checkGas` infrastructure in
`src/libxrpl/tx/wasm/HostFuncWrapper.cpp`, and the wasmi engine
configuration in `src/libxrpl/tx/wasm/WasmiVM.cpp`. **Hard cap per
EscrowFinish: 1,000,000 gas** (default
`extension_compute_limit` from `src/xrpld/core/Config.h:60`;
fee-voteable). The original review prompt stated 100K; that was wrong
by 10×. Severity arithmetic below uses the correct 1M cap.

**Companion doc:** `bug_session_wip.md` tracks the calibration work
(in particular the interpreter-regime anchor and the choice between
fixed-fee and per-byte pricing); it documents process state. This
file documents findings.

**Calibration anchor (verified empirically from `HostFuncBenchmark`
unittest, release build, M4 Pro):**
`getLedgerSqn` @ 60 gas measures **6 ns/call** with IQR 0% across
1000 samples ⇒ **1 gas ≈ 0.10 ns** of in-memory compute. Reference
hardware (AMD Threadripper) is expected to agree within ~2× for pure
CPU; NVMe random-read latency on the reference hardware is ~80–150 μs
per cold 4 KB page (per the prompt).

**Measurement caveat (from `wip.md` §4, confirmed by inspection):** every
benchmark runs its host fn in a tight in-memory loop. All numbers are
**floors** — hot-cache, no-disk, steady-state lower bounds. The
interpretation rule used throughout this report:

- *Floor exceeds registered gas* ⇒ definitely under-priced. Reality
  only adds disk / cache-miss / deserialization cost on top.
- *Floor fits inside registered gas* ⇒ proves nothing about reality.
  The real cost may be much higher (the dominant components — disk
  seek, SLE deserialization, apply-time persistence — are absent
  from the harness by design).

**Metering model.** A single helper, `checkGas` (`HostFuncWrapper.cpp:388`),
deducts `impFunc.gas` (the fixed per-fn entry price from
`createWasmImport`) at the top of every wrapper. There is no per-byte,
per-read, per-write, or per-compute-unit component. The schedule is
fixed-fee-only by design; the design choice is discussed in
[§ Dynamic vs Fixed Fees](#dynamic-vs-fixed-fees-is-per-byte-pricing-worth-it).

**WASM runtime configuration note (positive finding, recorded before bugs).**
`WasmiEngine::init()` (`WasmiVM.cpp:521–547`) disables, at engine
configuration time, every wasmi feature that would otherwise create
variable-cost-per-instruction outliers:

- `wasm_bulk_memory_set(false)` — disables `memory.copy`, `memory.fill`,
  `memory.init`, `table.copy`, `table.fill`, `table.init`.
- `wasm_reference_types_set(false)` — closes off most `table.*`.
- `floats_set(false)` — disables FP instructions entirely.
- Plus: `wasm_multi_memory_set`, `wasm_memory64_set`, `wasm_tail_call_set`,
  `wasm_extended_const_set`, `wasm_saturating_float_to_int_set`,
  `wasm_sign_extension_set`, `wasm_multi_value_set`,
  `wasm_mutable_globals_set`, `wasm_wide_arithmetic_set`,
  `wasm_custom_page_sizes_set`.
- `consume_fuel_set(true)` — wasmi's built-in fuel metering is on; the
  host-side `checkGas` reads/writes the same fuel counter via
  `wasm_store_get_fuel`/`wasm_store_set_fuel`. Single source of truth.

Consequence: the prompt's wasmi outlier list (`memory.copy/fill/init`,
`table.copy/fill/init`, FP ops) **does not apply** — those instructions
cannot appear in a valid module because the validator rejects them at
compile time. The flat 1-gas-per-instruction price is therefore
defensible *for the instruction set the engine accepts*. `memory.grow`
is bounded by `MAX_PAGES = 128` (8 MB) plus the WASM-level invariant
that linear memory can only grow, not shrink — discussed under Bug #6
below.

---

## Bug #1: `cacheLedgerObj` priced 5–20× under realistic cold-cache cost
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:27` (registration, 5000 gas);
  `src/libxrpl/tx/wasm/HostFuncWrapper.cpp:559–600` (wrapper);
  `src/libxrpl/tx/wasm/HostFuncImplGetter.cpp:222–249` (impl).
- **Impact:** Critical
- **Confidence:** High
- **Category:** I/O-as-compute / Mispriced (under)

**Description.** `cacheLedgerObj` is the canonical disk-read host
function. Its impl calls `ctx_.view().read(keylet)`
(`HostFuncImplGetter.cpp:245`), which on a cold cache performs a
SHAMap descent (a sequence of inner-node fetches from NuDB) plus a
leaf fetch plus SLE deserialization. The wrapper charges 5000 gas at
entry (`checkGas`) and **no other gas** — the schedule has no per-byte
component, so the price has to absorb the full worst-case
cold-NVMe + deserialization cost as a single fixed number.

5000 gas at 0.1 ns/gas implies an assumed wall-clock budget of
**500 ns per call** — three orders of magnitude smaller than a
single NVMe cold read at ~80–150 μs.

**Evidence.**

Cache assumption: cold for the specific SHAMap pages being touched. A
production validator serves many txs between calls to a specific
escrow's WASM; the relevant ledger pages are not pinned.

Per-call worst-case wall-clock on the reference hardware:
- Inner-node SHAMap descent (depth ~6–8): typically inner nodes are
  hot in TreeNodeCache, but on a busy validator with random access
  patterns at least 1 cold NVMe hit per descent is realistic.
  ~100 μs.
- Leaf read: 1 cold NVMe random 4 KB read at ~100 μs.
- SLE deserialization: highly variable by object type and size. A
  4 KB SLE deserializes in ~5–20 μs on the reference CPU (the
  measurement notes flag this as an unmeasured cost component).

Realistic per-call total: **~100–200 μs**, conservatively. The
measurement notes (`wip.md` §4) explicitly label the in-memory
`cacheLedgerObj` measurement as a FLOOR — disk and deserialization
were not exercised.

Attack tx concept (WAT pseudocode):
```wat
;; pre-stage 200 distinct cold ledger object IDs in linear memory.
(loop $L
  (call $cache_ledger_obj
    (i32.const id_ptr)   ;; cycles through 200 distinct IDs
    (i32.const 0))       ;; "any slot"
  (br_if $L (i32.lt_u (local.get $i) (i32.const 200))))
```
Gas spent: 200 × 5000 = **1,000,000 gas** (the cap; interpreter loop
overhead is single-digit gas per iteration). Calls performed: 200.
Wall-clock at ~100–200 μs each cold: **20–40 ms per tx.**

(Note: `MAX_CACHE = 256` slots per `cacheLedgerObj` cache, so the
attacker can fully populate the cache; the 200 distinct IDs fit.)

Severity per the prompt's table: milliseconds-scale wall-clock per
tx → Critical DoS. At block-fill rate (multiple such txs per block),
this multiplies linearly into block-production delay and validator
lag.

**Suggested fix.** Raise `cacheLedgerObj`'s entry price to **at least
20,000 gas** (giving ~5 cold reads per tx, ~500 μs–1 ms wall-clock
budget — Warning range, not Critical). A safer choice is ~50,000 gas
which limits the tx to 2 cold reads, capping wall-clock at ~300 μs
and leaving headroom for other work. This is the simplest
one-line mitigation. See also [§ Dynamic vs Fixed
Fees](#dynamic-vs-fixed-fees-is-per-byte-pricing-worth-it) for why
fixed-at-worst-case is the right shape for I/O-bound reads.

---

## Bug #2: `getNFT` priced 5× cheaper than `cacheLedgerObj` for comparable disk work
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:64` (registration, 1000 gas);
  `src/libxrpl/tx/wasm/HostFuncImplNFT.cpp:12–31` (impl);
  `src/libxrpl/ledger/helpers/NFTokenHelpers.cpp:520–537` (`findToken`);
  `src/libxrpl/ledger/helpers/NFTokenHelpers.cpp:22–33` (`locatePage`);
  `src/libxrpl/ledger/Ledger.cpp:372–380` (`Ledger::succ`).
- **Impact:** Critical
- **Confidence:** High (relative mispricing); Medium (absolute wall-clock,
  see caveats)
- **Category:** I/O-as-compute / Mispriced (under)

**Description.** `getNFT(account, nftId)` is priced at **1000 gas** — half
the cost of a simple keylet build (350–500 gas) and 5× cheaper than
`cacheLedgerObj` — yet does **comparable disk work plus extra CPU**.

Disk-read structure (re-checked against `Ledger::succ` and the
`NFTokenHelpers.cpp` impl):

- `locatePage` → `view.succ(first, last)` — a SHAMap traversal (one
  descent, `stateMap_.upper_bound(key)`).
- `view.read(Keylet(ltNFTOKEN_PAGE, located_key))` — leaf fetch. Shares
  its descent path with the preceding `succ`, so does **not** add a
  separate cold read in the typical case.

Net disk-read structure: **one cold leaf fetch**, the same as
`cacheLedgerObj`. The earlier framing of "two cold reads" was wrong.

What getNFT does *in addition* to cacheLedgerObj:
- Page deserialization (NFTokenPage can hold up to 32 NFTs, ~12 KB):
  ~10–20 μs CPU.
- Linear scan through the page's `sfNFTokens` array looking for the
  target `nftokenID`: ~1 μs.
- URI extraction and byte copy: negligible.

So **getNFT ≈ cacheLedgerObj on disk, plus ~10–20 μs of extra CPU.**
It should be priced at least equal to cacheLedgerObj (5000 gas),
arguably higher (~6000–7000 gas) to cover the deserialization. The
current 1000 gas is wrong on both axes.

**Evidence and severity.**

Cache assumption: cold for the targeted account's NFTokenPage SHAMap
region. Per-call wall-clock:

- *Typical case* (warm validator, only the located leaf is cold):
  ~110–170 μs (1 cold read + page deserialize + scan).
- *Adversarial case* (account's NFTokenPage subtree has cold inner
  nodes too): ~3–5 cold inner-node reads + 1 cold leaf read =
  ~4–6 cold reads at ~100 μs each = **400–600 μs per call**, with an
  upper bound of ~8 reads (~800 μs).

Same adversarial caveat applies to `cacheLedgerObj` (any disk-touching
host fn inherits it).

Attack tx concept: target an account holding a single NFT so every
`getNFT` call has to walk to a real page:

```wat
(loop $L
  (call $get_nft
    (i32.const account_ptr) (i32.const 20)
    (i32.const nft_id_ptr)  (i32.const 32)
    (i32.const out_ptr)     (i32.const 256))
  (br_if $L (i32.lt_u (local.get $i) (i32.const 1000))))
```

Gas spent: 1000 × 1000 = **1,000,000 gas** (the cap). Calls performed:
1000.

| Case | Per-call wall-clock | Per-tx wall-clock |
|---|---|---|
| Typical (1 cold read + dsr+scan) | ~110–170 μs | **110–170 ms** |
| Adversarial (cold descent path) | ~400–600 μs | **400–600 ms** |
| Worst case | ~800 μs | ~800 ms |

Even the *typical* case is in the hundreds-of-ms range per tx —
Critical DoS per the prompt's severity table. The adversarial case
is approaching Severe DoS / consensus risk.

If the attacker chooses an account+nftId where `findToken` is called
but the NFT *is not* in the located page, the impl still pays the
descent + page-deserialize + scan and returns `LEDGER_OBJ_NOT_FOUND`.
Same wall-clock; error path doesn't short-circuit the work.

**Open question on adversarial construction.** Whether an attacker can
actually engineer ledger state where the NFTokenPage subtree's inner
nodes are reliably cold on a busy validator depends on (a) what they
can place in the ledger, and (b) TreeNodeCache eviction policy under
realistic mainnet load. Not verified in this review; would need
direct measurement against a real validator.

**Suggested fix.** Until a route-through-cache redesign is feasible,
**lift `getNFT`'s entry gas to ≥ 10,000** (10 calls/tx → ~1–3 ms
wall-clock — Warning range). Better, when bandwidth permits: route
`getNFT` through `cacheLedgerObj` semantics — require the caller to
cache the NFTokenPage SLE first (paying the cache-load gas), then
have `getNFT` operate on the cached page only, at a small fixed cost.
The same model is used for `getLedgerObjField` (which operates on
already-cached SLEs at 70 gas).

---

## Bug #3: `updateData` price doesn't reflect apply-time write cost
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:103` (registration, 1000 gas);
  `src/libxrpl/tx/wasm/HostFuncWrapper.cpp` (`updateData_wrap`);
  `src/libxrpl/tx/wasm/HostFuncImpl.cpp:11–20` (impl);
  `src/libxrpl/tx/transactors/escrow/EscrowFinish.cpp:399–408` (commit).
- **Impact:** Medium today / Critical for state growth
- **Confidence:** High
- **Category:** State-growth-disproportionate / Mispriced (under)

**Description.** This bug is **not** about the timing of the write.
The two-phase pattern — stage during WASM, persist at apply time — is
the standard XRPL tx-processing pattern and is correct as designed.
The bug is that the **1000 gas charged for the `updateData` host fn
call does not reflect the apply-time work it implies**, which on a
4 KB payload is non-trivial:

- An SLE re-serialization (~5–20 μs CPU for a 4 KB SLE).
- A SHAMap inner-node update along the path to the escrow's leaf
  (~6–8 rehashes).
- A NuDB write of the new node(s). Without fsync, ~tens of μs and
  contention for write-buffer bandwidth; with the eventual fsync at
  ledger close, durable cost.
- **Permanent ledger state growth of up to 4 KB.**

Multiple `updateData` calls inside one WASM execution all overwrite
`data_`; only the last one's bytes are persisted. So the wall-clock
attack is weak (one tx → at most one apply-time write of up to
4 KB → at most one `O(SLE_size)` re-serialize and one SHAMap leaf
update). The DoS angle on apply-time wall-clock is not by itself
serious.

The serious vector is **state growth disproportionate to gas**:
at 1000 gas per 4 KB write, the schedule charges roughly 0.25 gas
per byte of permanent ledger inflation. Whether this is exploitable
in practice depends on the absolute ComputationAllowance-per-drop
conversion and on the per-escrow reserve, which are out of scope
here — but the asymmetry is large enough to flag.

**Evidence.** Per-tx host-fn wall-clock is dominated by the
host-side `Bytes` copy (~2 μs for 4 KB) plus the staging overhead
inside `WasmHostFunctionsImpl::updateData` (`HostFuncImpl.cpp:12–19`).
The actual ledger write happens once in
`EscrowFinish::doApply` (`EscrowFinish.cpp:406–407`):
```cpp
slep->setFieldVL(sfData, makeSlice(*data));
ctx_.view().update(slep);
```
which is the standard pattern shared by every XRPL transactor. The
write is metered by the surrounding `EscrowFinish` fee envelope, not
by WASM gas, and that envelope was sized before
`sfData` (variable, up to 4 KB) existed on escrows.

**Suggested fix.** Two complementary changes:

1. **Lift `updateData`'s entry gas to reflect the apply-time work.**
   At ~20 μs of apply-time CPU per call (re-serialize 4 KB SLE +
   SHAMap rehash + NuDB write submission), the matching gas charge
   is ~200,000 gas. That's larger than the per-tx cap, which means
   the right interpretation is "this is a once-per-tx operation, not
   a callable-in-a-loop primitive." Charge most of the cost
   **outside the gas envelope**, at the transactor level — see (2).
2. **Add a per-byte fee on the persisted `sfData` size at apply
   time**, scaled by serialized size. This is the "state-growth
   surcharge" pattern used by other XRPL features (e.g. reserves
   for trustlines, NFTokenPages). The fee is paid in drops, not
   gas, and is charged on the *escrow*, not on the WASM execution.
   Once this is in place, the 1000 gas for `updateData_wrap`'s
   host-side staging is correctly priced — it's just a `Bytes`
   copy, well below 1 μs.

The methodology notes (`wip.md` §4) explicitly flag the unmeasured
apply-time cost.

---

## Bug #4: Nested-field traversal priced as constant; locator depth up to 256
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:31–33, 37–39` (registration, 110 gas / 70 gas);
  `src/libxrpl/tx/wasm/HostFuncImplGetter.cpp:126–203` (`detail::locateField`).
- **Impact:** Medium → Low (conditional on real STObject nesting depth)
- **Confidence:** Medium
- **Category:** Variable-cost-as-fixed

**Description.** `getTxNestedField`, `getCurrentLedgerObjNestedField`,
`getLedgerObjNestedField`, and the `*NestedArrayLen` siblings accept
a `locator` Slice of up to `maxWasmParamLength = 1024` bytes. The
locator is a sequence of 4-byte SField codes; `locateField` walks
the STObject tree one descent per code
(`HostFuncImplGetter.cpp:164–200`). Maximum legal locator length is
1024 / 4 = **256 descents** per call. Each descent performs a
`peekAtPField` (binary search through STObject fields) or an array
index lookup.

The wrappers charge a *constant* 110 gas (70 for
`getTxNestedArrayLen`) regardless of locator length.

**Evidence.** 100K / 110 = ~909 calls/tx, each performing up to 256
descents. At ~3 ns/descent on the reference CPU, 256 descents ≈
~750 ns. Theoretical total: ~700 μs per tx — within the
hundreds-of-μs Warning–Critical band per the prompt's table.

However, the attack assumes the ledger object being traversed
actually *contains* 256-deep nesting. No well-formed XRPL object
currently nests beyond ~3–4 levels (escrows, NFT pages, etc.),
and `locateField` enforces that simple fields are terminal
(`HostFuncImplGetter.cpp:194–196`), so descent terminates at the
first non-object/non-array. Realistic worst case for any object an
attacker can construct in `ctx_.tx` is ~10 descents → ~30 ns →
~30 ns × 909 calls ≈ 27 μs per tx — comfortably inside the
Warning threshold.

Severity is therefore conditional on whether an attacker can
construct a deeply-nested STObject. As things stand: **Low**. If a
future feature allows deeper nesting (e.g. nested NFT metadata,
arbitrary user object trees), the gas charge will need to grow with
the worst-case depth — or, more cleanly, the locator size cap (1024)
will need to drop.

**Suggested fix.** Today: no change required, but add a comment in
`createWasmImport` or in the wrapper noting that the 110 gas charge
assumes ≤ 10-level nesting. If/when an SLE family adds deeper
nesting, revisit. Charging `locator->size()` units of gas
(1 gas per 4 bytes) would also work and adds ~256 gas to the
worst-case call — trivial.

---

## Bug #5: `checkSignature` priced ~4.4× under measured cost
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:41` (registration, 35,000 gas).
- **Impact:** Medium
- **Confidence:** High (`HostFuncBenchmark` output reproduces the
  ~153K gas-equivalent floor at M4 Pro release build)
- **Category:** Mispriced (under)

**Description.** The `HostFuncBenchmark` unittest measures
`check_sig` at an intercept of **15,309 ns** with a slope of
~0 ns/byte (libsecp256k1 verify is constant in N), independent of
message size up to the 1 KB cap. At the verified anchor of
0.10 ns/gas, the floor is **~153,000 gas-equivalent** vs the
registered 35,000 gas — a **4.4× under-charge** per call, on the
floor alone. The real production cost only adds on top: ECDSA
verify is pure compute (no disk path), so the floor is close to
the real cost here — meaning the under-pricing is genuine, not a
floor artifact.

**Evidence.** `HostFuncBenchmark checkSignature` output:
```
msg bytes   |  good ns | good IQR% |  bad ns  | bad IQR%
------------+----------+-----------+----------+----------
         32 |    15833 |    15.32% |    14808 |    7.26%
        128 |    14808 |     6.95% |    14854 |    6.62%
        512 |    14095 |    10.26% |    14595 |   10.57%
       1024 |    14900 |    10.77% |    14158 |    8.65%
linear fit on max(good,bad): time(N) = 15309.33 + -0.62 * N (ns)
intercept as gas:    153093.30  (using baseline calibration 0.10 ns/gas)
```

100K / 35K ≈ 2.85 calls per tx. Real wall-clock per call ≈ 15 μs;
2.85 × 15 = **~43 μs per tx** of pure compute. Tens of μs →
Warning / Medium per the prompt's severity table. Not an immediate
DoS, but a Medium mispricing worth fixing before release.

**Suggested fix.** Raise to **~150,000 gas** (matching the measured
floor with a small headroom). At the new price the budget allows
~0.66 verify per tx — i.e. at most one signature check per WASM
execution, which matches the typical legitimate use case (verify
one externally-attested signature). If two-or-more signature checks
are a real use case, the budget cap may need to rise alongside;
that's an EscrowFinish-level decision, not a schedule-level one.

---

## Bug #6: `getCurrentLedgerObjField` family under-priced at floor (4–15×)
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:28–33` (registrations, 70 gas);
  `src/libxrpl/tx/wasm/HostFuncWrapper.cpp` (`getTxField_wrap`,
  `getCurrentLedgerObjField_wrap`, `getLedgerObjField_wrap`, and
  the three nested-field variants at 110 gas);
  `src/libxrpl/tx/wasm/HostFuncImplGetter.cpp:253–315`.
- **Impact:** Medium
- **Confidence:** High (measured floor exceeds registered gas at
  every legal input size)
- **Category:** Mispriced (under) / Variable-cost-as-fixed

**Description.** The `HostFuncBenchmark getCurrentLedgerObjField`
sweep shows the wrapper is **under-priced at every legal input
size, even on the floor measurement** (which excludes any disk
path to fetch the "current" ledger object on first use):

```
output bytes |  median ns | IQR/median%
-------------+------------+-------------
          32 |         28 | 10.71%
         128 |         26 | 11.54%
         512 |         37 | 10.81%
        1024 |         51 |  9.80%
        4096 |        104 | 16.35%
linear fit: time(N) = 27.20 + 0.02 * N (ns)
intercept as gas:    272.02  (using baseline calibration 0.10 ns/gas)
```

At the verified anchor of 0.10 ns/gas:

- **Floor intercept = 272 gas-equivalent** vs registered **70 gas**
  → ~4× under-charged before any per-byte work.
- **Floor at 4 KB = 1040 gas-equivalent** vs registered 70 gas
  → ~15× under-charged at worst-case output size.

Since both the `Bytes` allocation and the host-to-wasm memcpy happen
on every call, even a 32-byte read is ~4× under-priced on the floor.
Production-real cost on the first call after a cache miss can add
SHAMap traversal and SLE deserialization on top — see Bug #1's
NVMe arithmetic for the worst case if `getCurrentLedgerObj()`
itself triggers a fetch.

**Evidence.** 100K / 70 = ~1428 calls/tx. Floor wall-clock at
4 KB worst case: 1428 × 104 ns = **~149 μs per tx** of hot-cache
in-memory work — already in the Warning band, before any cold path.

The same shape applies to `getTxField` and `getLedgerObjField`
(also 70 gas) and to the nested-field family at 110 gas (since the
nested variants only add a small constant per descent on top of
the same `getAnyFieldData` allocation + copy cost).

**Suggested fix.** Two options:

1. **Fixed-at-worst-case re-tune.** Raise the entry price to ~1100
   gas (matches the 4 KB floor with a small headroom). This
   over-charges 32-byte reads by ~15× — significant throughput
   waste, since field-reads at small sizes are the bread-and-butter
   operation of any WASM escrow. *Not recommended* on its own.
2. **Refactor per `todos.md` TODO 1**: change the impl signature
   to `(SField, uint8_t* dst, size_t cap)` so the impl writes
   directly into wasm linear memory, removing the intermediate
   `Bytes` allocation + first memcpy. Per the benchmark output's
   own commentary on the slope, this would roughly halve both the
   intercept and the slope. Combined with raising the entry price
   to ~500 gas, the new schedule is honest at worst case (~500 gas
   ≈ 50 ns budget, well above the post-refactor ~25 ns intercept
   and ~10 ns at 1 KB slope) and not too punishing at small inputs
   (~10× over-charge at 32 bytes — bounded).

The refactor is the recommended path. It's the same fix already
described in `todos.md` TODO 1 for a different reason (allocator
overhead removal).

---

## Bug #7: `compute_sha512_half` under-priced at worst-case (3.9×) and over-priced at small inputs (2×)
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:42` (registration, 1500 gas);
  `src/libxrpl/tx/wasm/HostFuncImpl.cpp:39–44`.
- **Impact:** Medium
- **Confidence:** High (measurement is pure compute, libcrypto path
  is opaque to the rippled optimizer, slope is stable across builds
  per `volatile.md` §5)
- **Category:** Variable-cost-as-fixed

**Description.** This is the textbook variable-cost-as-fixed
mispricing, and the one place in the whole schedule where per-byte
pricing actually pulls its weight. The `HostFuncBenchmark`
output:

```
input bytes |  median ns | IQR/median%
------------+------------+-------------
         32 |         78 | 0.00%
        128 |        129 | 1.55%
        512 |        325 | 0.92%
       1024 |        587 | 0.51%
linear fit: time(N) = 62.51 + 0.51 * N (ns)
slope as gas/byte:   5.12
```

At the verified 0.10 ns/gas anchor, and with `maxWasmParamLength
= 1024` capping legal input size:

- **At 32-byte input:** floor 78 ns = 780 gas-equivalent. Registered
  1500 gas → ~2× *over*-priced.
- **At 1024-byte input:** floor 587 ns = 5870 gas-equivalent.
  Registered 1500 gas → ~3.9× *under*-priced.

The wrapper is therefore wrong at both ends of the legal input range,
and any fixed price within a factor of 2 of the worst case would
hurt small-hash use cases by an order of magnitude. This is the
schedule's clearest argument for keeping a per-byte rate on this
specific wrapper.

**Evidence.** 100K / 1500 ≈ 67 calls/tx. At 1 KB worst-case input,
67 × 587 ns = **~39 μs per tx** of pure compute — Warning band.
The under-pricing at 1 KB is real wall-clock attack surface;
the over-pricing at 32 bytes is throughput waste for legitimate
small-hash users.

**Suggested fix.** Carve out an exception to the fixed-fee-only
model for this wrapper. Charge:

```
gas = 625 + 5 * N
```

where `N` is the input slice size in bytes (the intercept matches
the measured ~62.5 ns at 0.10 ns/gas; the slope matches the measured
0.51 ns/byte → ~5 gas/byte). At 32 bytes: 785 gas. At 1024 bytes:
5645 gas. Both within ~10% of the measured wall-clock cost.

Pricing-table-level alternative: keep fixed, but at 6000 gas. Then
small-input calls over-charge by ~30× — bad enough to make this the
exception that proves the rule for the fixed-fee design (see
§ Dynamic vs Fixed below). The per-byte rate option is preferable.

---

## Bug #8: `memory.grow` instruction priced 1 gas per call (per-call, not per page)
- **Location:** `src/libxrpl/tx/wasm/WasmiVM.cpp:529` (fuel enabled);
  `src/libxrpl/tx/wasm/WasmiVM.cpp:562` (memory cap).
- **Impact:** Low (cap + monotonicity defend most of the attack surface)
- **Confidence:** Medium
- **Category:** Variable-cost-as-fixed (hardening only)

**Description.** `memory.grow` is a single bytecode dispatch but
internally calls `mmap`/`mremap` and zero-fills the new pages. Each
page is 64 KB; the engine cap is 128 pages = 8 MB max linear memory.
wasmi's flat fuel model charges 1 gas per `memory.grow` call.

Two defenses are in place at the engine level:
1. **Hard cap.** Linear memory cannot exceed 8 MB
   (`wasm_store_new_with_memory_max_pages(engine, MAX_PAGES = 128)`).
   The maximum wall-clock cost of all `memory.grow` calls in a tx,
   summed, is bounded by the cost of zero-filling 8 MB on first use.
2. **Monotonicity.** The WASM spec guarantees `memory.grow` can only
   grow; there is no `memory.shrink`. An attacker cannot
   grow→shrink→grow in a loop to repeatedly trigger the
   mmap+zero cost. Once the page is in linear memory, it stays.

Combined: an attacker can pay at most ~128 gas (across all
`memory.grow` instructions in the tx) to trigger ~127 × (mmap +
64 KB zero-fill) ≈ ~500 μs of one-time setup work.

Severity: ~500 μs of one-time work per tx is in the
hundreds-of-μs band. The attacker has to "spend the rest of the gas
budget on other things" — the grow itself doesn't dominate the
budget. The total tx wall-clock is bounded by the sum of all
mispricings, not by `memory.grow` alone.

**Suggested fix (hardening only).** Override the wasmi fuel cost
for `memory.grow` to roughly match per-page mmap+zero. At ~5 μs
per 64 KB page and 0.1 ns/gas, ~50,000 gas per page is appropriate.
At that price the budget allows ~2 page grows in a dedicated
grow-attack scenario — far below 128. Optional; the existing
defenses are sufficient.

---

## Bug #9 (informational): Most host functions have no calibration basis
- **Location:** `src/libxrpl/tx/wasm/WasmVM.cpp:22–93`.
- **Impact:** Low (methodology / hardening flag — not a concrete attack)
- **Confidence:** High (per the measurement notes)
- **Category:** Methodology

**Description.** Per `wip.md` §1, only six wrappers have a documented
calibration basis: `getLedgerSqn`, `getCurrentLedgerObjField`,
`cacheLedgerObj`, `updateData`, `computeSha512HalfHash`,
`checkSignature`. The other 50+ host functions in `createWasmImport()`
have prices that are educated guesses against ratios from these six.

Unmeasured wrappers (enumerated for the record):

- Ledger header: `getParentLedgerTime`, `getParentLedgerHash`,
  `getBaseFee`, `isAmendmentEnabled`.
- Ledger object access: `getTxField`, `getLedgerObjField`,
  `getTxNestedField`, `getCurrentLedgerObjNestedField`,
  `getLedgerObjNestedField`, `getTxArrayLen`,
  `getCurrentLedgerObjArrayLen`, `getLedgerObjArrayLen`,
  `getTxNestedArrayLen`, `getCurrentLedgerObjNestedArrayLen`,
  `getLedgerObjNestedArrayLen`.
- Keylets (19): `accountKeylet`, `ammKeylet`, `checkKeylet`,
  `credentialKeylet`, `delegateKeylet`, `depositPreauthKeylet`,
  `didKeylet`, `escrowKeylet`, `lineKeylet`, `mptIssuanceKeylet`,
  `mptokenKeylet`, `nftOfferKeylet`, `offerKeylet`, `oracleKeylet`,
  `paychanKeylet`, `permissionedDomainKeylet`, `signersKeylet`,
  `ticketKeylet`, `vaultKeylet`.
- NFT: `getNFT`, `getNFTIssuer`, `getNFTTaxon`, `getNFTFlags`,
  `getNFTTransferFee`, `getNFTSerial`.
- Trace: `trace`, `traceNum`, `traceAccount`, `traceFloat`,
  `traceAmount`.
- Float family (17): `floatFromInt`, `floatFromUint`,
  `floatFromSTAmount`, `floatFromSTNumber`, `floatToInt`,
  `floatToMantissaAndExponent`, `floatNegate`, `floatAbs`, `floatSet`,
  `floatCompare`, `floatAdd`, `floatSubtract`, `floatMultiply`,
  `floatDivide`, `floatRoot`, `floatPower`, `floatLog`.

The keylet prices (350–500 gas) are plausible by inspection — each
keylet is a SHA-512-half over ~40–100 bytes of structured input,
which is ~0.5–1 μs of libcrypto work, comfortably above the gas
charge (~50 ns at 0.1 ns/gas). The float prices look reasonable in
*ratio* (transcendentals priced higher than add/sub) but the
absolute calibration is not anchored.

**Suggested fix.** Extend the existing benchmark harness
(`src/test/app/HostFuncBenchmark_test.cpp`) to cover at minimum:
one keylet (representative of the rest), one variable-input keylet
(`lineKeylet` or `credentialKeylet`), and the float family's three
transcendentals (`floatPower`, `floatLog`, `floatRoot`). The
remaining keylets and floats can be calibrated by ratio against
the representatives.

---

## Bug #10 (informational): `cacheLedgerObj` permits 256 cached SLEs of unbounded size
- **Location:** `include/xrpl/tx/wasm/HostFuncImpl.h:16` (`MAX_CACHE = 256`);
  `src/libxrpl/tx/wasm/HostFuncImplGetter.cpp:222–249`.
- **Impact:** Low (the gas budget bounds practical exploitation)
- **Confidence:** Low (depends on heap pressure and adjacent budgets)
- **Category:** No upper bound (hardening)

**Description.** `cacheLedgerObj` holds up to `MAX_CACHE = 256`
`shared_ptr<SLE const>` in `cache_`. Each cached SLE can be up to
~4 KB. The gas budget (100K / 5000 = 20 distinct `cacheLedgerObj`
calls per tx) bounds the practical in-memory footprint to
20 × ~4 KB = ~80 KB per WASM execution — well within reason. The
hardening concern is that the array allocation is fixed size 256
but the SLE pointers it holds are not bounded; if the gas budget
ever rises or the per-call `cacheLedgerObj` price falls, the
ceiling grows linearly.

**Suggested fix.** Add a comment documenting the implicit
gas-budget-derived bound, and consider a hard runtime cap of 32 or
64 cache slots — the legitimate use cases are not expected to need
hundreds of cached SLEs in a single WASM execution. After Bug #1
is fixed (entry price raised), the cap is even less reachable.

---

## Dynamic vs Fixed Fees: Is Per-Byte Pricing Worth It?

A previous iteration of this branch (`supported_May_2_hf_fee`)
introduced per-byte / per-read / per-write / per-compute-unit gas
rates (`gasPerMemByte`, `gasPerReadByte`, `gasPerWriteByte`,
`gasPerComputeUnit`) and added `chargeMem` / `chargeRead` /
`chargeWrite` / `chargeCompute` helpers invoked from the wrappers.
That infrastructure has been removed on the current branch — the
schedule is fixed-fee only. The question this section addresses:
**should the dynamic-fee infrastructure be reinstated, or is
fixed-fee correct for this design?**

Recommendation: **fixed-fee is the right shape for this design.**
Rationale follows.

### The case for fixed-only

**1. Input caps are small enough to make worst-case-fixed pricing tight.**

The two caps in `xrpl/protocol/Protocol.h`:

```cpp
std::size_t constexpr maxWasmDataLength = 4 * 1024;  // 4KB
std::size_t constexpr maxWasmParamLength = 1024;     // 1KB
```

mean that the *worst-case* input to any host function is at most
4 KB (and only on the apply-time write path; everything else is
1 KB). The ratio between the worst-case and a typical input is at
most 4 KB / 32 bytes = 128×. Most realistic uses are within 4–8×.
Charging the worst case as a fixed price therefore over-charges
small inputs by at most ~10×, which is mild throughput waste —
not a correctness or security issue.

Compare to EVM, where the precedent for dynamic pricing (EIP-150,
EIP-2929) was set in an environment with no per-tx hard cap on
gas and input sizes ranging over many orders of magnitude. With a
100K hard cap and 1–4 KB input caps, the per-byte pricing problem
is much smaller.

**2. For most host functions, byte-count is not the dominant cost
component.**

- **Keylets.** Inputs are small fixed-or-bounded structured tuples
  (AccountID + maybe currency + maybe seq). Output is always
  32 bytes. The dominant cost is a SHA-512-half over <100 bytes
  ≈ 1 μs. Per-byte scaling is well below measurement noise.
- **Disk reads (`cacheLedgerObj`, `getNFT`).** The dominant cost
  is **NVMe seek + page fetch, which is fixed per read** regardless
  of the SLE's serialized size. A 4 KB SLE deserializes in
  ~5–20 μs of CPU on top of ~100 μs of seek/read — the CPU
  component is ~10–20% of the total. Per-byte pricing would track
  the wrong cost; per-read fixed pricing is structurally
  correct, and the only question is the absolute number (see Bug #1).
- **Float ops.** Operate on fixed 12-byte encoded floats. No
  byte-count parameter at all.
- **Get-field family.** Output up to 4 KB. The actual work is a
  `peekAtPField` (constant) + a memcpy (~80 ns for 4 KB at the
  release-build 0.02 ns/byte slope). Compared to a ~30 ns entry
  intercept, per-byte is ~2–3× of the cost at worst case, ~0 at
  small inputs. Absorbing the worst case as the fixed entry price
  over-charges small calls by ~3× — bounded waste.
- **Trace functions.** Cost is dominated by the journal call,
  which is mostly fixed (small msg formatting).

**3. The two cases where per-byte pricing would carry the most weight:**

- **`compute_sha512_half`.** Real cost is dominated by the SHA
  kernel slope (~0.5 ns/byte release; 1 KB max input → ~500 ns
  slope contribution on top of ~50 ns intercept). Slope dominates
  beyond ~100 bytes. Worst-case-fixed (1500 gas) over-charges
  32-byte inputs by ~10×. A WASM program building a Merkle proof
  or aggregating many small hashes would pay ~10× over real cost.
  But: legitimate use cases at the host-function tier rarely hash
  many small chunks; the typical pattern is "hash one ~variable-size
  blob." The over-pricing at the small end is bounded throughput
  waste, not correctness loss.
- **`check_sig`.** ECDSA verify itself is largely constant
  (the modular arithmetic is fixed-cost); the message-hashing
  component scales but message size is capped at 1 KB. The
  measured slope of ~6 gas/byte vs the 1 KB cap means the slope
  contribution is at most ~6000 gas — small relative to the ~150K
  worst-case fixed price recommended in Bug #5. Fixed pricing
  works here, slightly conservatively.

For both, a fixed-at-worst-case price is bounded by the input cap
times the per-byte slope. The over-pricing at small inputs is
real throughput waste but not catastrophic.

**4. Calibration cost.**

Per-byte rates require measuring slope and intercept separately for
every variable-size wrapper, then re-tuning each entry price. With
60+ host functions, that's a lot of measurement work. Any one
mis-calibrated slope is a new mispricing — and unlike a
mis-calibrated fixed price (which is bounded by the input cap), a
mis-calibrated slope can multiply across calls in unexpected ways.
The existing measurement notes calibrate one variable-input wrapper
end-to-end (`getCurrentLedgerObjField`) with three triangulating
methodologies (`volatile.md`); doing this for every other
variable-size wrapper is high-effort and high-error-rate.

**5. Audit cost.**

Fixed prices are easy to reason about: "worst-case input takes X μs;
charge X / 0.1 ns = 10X gas." A reviewer can do this in their head
per function. Per-byte prices add a second knob per function that
has to be jointly calibrated; the budget envelope becomes
input-dependent and the worst-case attack tx requires solving an
optimization problem to find the highest-real-cost-per-gas ratio.

**6. Metering simplicity.**

Single gas check at function entry, before any work. No metering
gap concern (which was Bug #8 in the previous iteration: with
dynamic per-byte rates, the per-byte charge was deducted **after**
the impl had already produced the bytes — an attacker who runs out
of gas during the per-byte charge has already cost the network the
work). Fixed-fee-only deducts gas *before* work begins; the
metering point is sound by construction.

**7. Hard cap vs gas-per-opcode.**

The 100K per-tx hard cap is a tighter bound on worst-case cost
than any per-opcode pricing scheme can achieve. Per-byte pricing
inside that cap is fine-tuning; what matters most is that the
*per-call* entry prices are honest at worst-case input. Bugs #1
and #2 above are evidence that the current branch is missing the
honest-at-worst-case property; per-byte pricing would not fix that
on its own.

### The case for keeping (or reinstating) dynamic pricing

**1. Throughput waste on small inputs.**

`compute_sha512_half` at 1500 gas for a 32-byte hash is ~20×
over real cost. With 100K budget, this caps legitimate use cases
that need many small hashes (~67/tx max). If a smart-escrow author
needs to verify e.g. 30 separate small attestations, they're tight
on budget for no good reason — the network capacity exists.

This is the one real win for dynamic pricing in the current
schedule.

**2. Future-proofing.**

If `maxWasmDataLength` or `maxWasmParamLength` ever increases
(say, to 16 KB or 4 KB respectively), fixed pricing falls behind
faster than dynamic. With caps stable, this isn't an issue.

**3. Per-byte feels closer to "you pay for what you use."**

Aesthetic. Not a functional argument.

### Verdict and recommendation

**Keep fixed-fee for all wrappers *except* `compute_sha512_half`,
which warrants a `gas = a + b*N` exception.** The measured data
makes the exception unambiguous: at 1500 gas fixed, the wrapper
is ~2× over-priced at 32 bytes and ~3.9× under-priced at 1024
bytes — wrong at both ends of the legal range simultaneously.
No fixed price within the input cap can be right at both ends;
the ratio between worst-case and best-case real cost (587 ns / 78
ns ≈ 7.5×) exceeds any reasonable over-pricing tolerance. See
Bug #7 for the recommended formula (`gas = 625 + 5 * N`).

For every other wrapper the fixed-fee verdict stands. The reason
the verdict pinches on `compute_sha512_half` specifically is that
it has all four of these properties simultaneously: (a) pure-compute
(no I/O), (b) high slope-to-intercept ratio (~8× at the cap), (c)
the slope is the *dominant* cost component beyond ~100 bytes, and
(d) small-input calls are a realistic use pattern (per-element
hashing in Merkle constructions, MAC chains, etc.). The other
wrappers fail at least one of these:

- Keylets, float, trace, get-field: low slope-to-intercept ratio,
  or output size is fixed.
- `check_sig`: slope is essentially zero (the SHA-512-half of the
  message is fast next to ECDSA verify itself).
- Disk-bound wrappers (`cacheLedgerObj`, `getNFT`,
  `getCurrentLedgerObjField`): the dominant cost is per-read NVMe
  seek, not per-byte CPU. Per-byte pricing tracks the wrong cost.

So the rule is: **fixed-fee everywhere, with one surgical
exception for `compute_sha512_half`.** A schedule that's easy to
calibrate, audit, and reason about is still the goal; one
two-parameter exception is a cheap price to pay for fixing the one
case where fixed is genuinely wrong.

What this means for the report's findings: Bug #1, Bug #2, Bug #5,
and Bug #6 (under-priced at floor) are all about the *fixed* prices
being too low at the worst-case input. Per-byte rates would not
fix them — they'd just be a different lever for the same
calibration. Bug #3 is at the transactor level (state-growth
surcharge), also fixed-fee-shaped. Bug #7 is the one bug whose fix
is structural (move from fixed to linear).

---

## Budget-level sanity check

The cap is **1,000,000 gas** per EscrowFinish. The wall-clock that 1M
gas buys depends on the regime the work runs in:

| Regime | ns/gas (measured worst kernel) | 1M gas → wall-clock |
|---|---|---|
| Cheapest interpreted WASM (kernel-floor) | ~0.6 | ~0.6 ms |
| Worst interpreted WASM (call_indirect, kernel 5) | ~1.84 | ~1.84 ms |
| Worst interpreted WASM (asymptotic, dense call_indirect; estimated) | ~5–6 | ~5–6 ms |
| Host-fn anchor (getLedgerSqn wrapper-only) | ~0.10 | ~0.1 ms |

The interpreter-regime anchor calibration is in progress; see
`bug_session_wip.md` for the open work. For the attack arithmetic
below, the worst-case interpreter wall-clock budget envelope is
**single-digit ms at 1M gas**.

The envelope collapses dramatically when the budget is spent on host
functions the schedule under-prices:

| Worst-case budget spend | Wall-clock per tx | Severity |
|---|---|---|
| Pure interpreted WASM (worst measured kernel) | ~1.84 ms | OK |
| 200 × `cacheLedgerObj` (5000 gas) | 20–40 ms | Critical (Bug #1) |
| 1000 × `getNFT` (1000 gas) | 110–170 ms typ; 400–600 ms adversarial | Critical (Bug #2) |
| 14,285 × `getCurrentLedgerObjField` (70 gas) on 4 KB | ~1.5 ms (floor only) | Medium (Bug #6) |
| ~28.5 × `checkSignature` (35K gas) | ~430 μs | Medium (Bug #5) |
| ~666 × `computeSha512HalfHash` (1500 gas) on 1 KB inputs | ~390 μs | Medium (Bug #7) |
| 1000 × `updateData` (1000 gas, 4 KB) | ~2 ms host-side + 1 unmetered apply-time write | Medium (Bug #3) |

The gap between pure-compute (~2 ms) and the worst realistic host-fn
mix (~600 ms for adversarial getNFT-spam) is **~300×**. The budget
cap is correctly sized for the cheapest legal tx and catastrophically
loose for the most expensive one. The fix is not the budget cap; it
is honest worst-case pricing on the disk-touching host fns (Bugs #1,
#2) and apply-time-cost accounting (Bug #3).

---

## Summary

- **Total bugs found: 10** (7 substantive + 3 informational/hardening).
- **Critical: 2** — Bugs #1 (`cacheLedgerObj`), #2 (`getNFT`).
  Score: **+20**.
- **Medium: 5** — Bugs #3 (`updateData` apply-time), #4 (nested-field),
  #5 (`check_sig`), #6 (`getCurrentLedgerObjField` family at floor),
  #7 (`compute_sha512_half` over+under). Score: **+25**.
- **Low: 3** — Bugs #8 (`memory.grow`), #9 (unmeasured fns),
  #10 (cache slots). Score: **+3**.
- **Total Score: 48 points.**

**Disk-read host fn priced correctly?** **No.** Bugs #1
(`cacheLedgerObj`) and #2 (`getNFT`) both under-charge realistic
cold-NVMe + SHAMap + deserialization work by ~5–30×. The `getNFT`
case is the most acute (10–30 ms per tx). Both are fixable with
one-line entry-price changes.

**Disk-write host fn priced correctly?** **No / Uncertain.** The
two-phase pattern (stage during WASM, persist at apply time) is
correct by design — that's not the bug. The bug is that
`updateData`'s 1000 gas entry charge doesn't reflect the apply-time
cost it implies (~20 μs CPU plus a NuDB write plus permanent state
growth of up to 4 KB). Per-tx wall-clock attack is weak because
only one apply-time write occurs per finish; the state-growth
disproportionality is the real concern. Severity depends on the
absolute ComputationAllowance-per-drop conversion (out of scope).

**Per-byte / dynamic pricing necessary?** **Fixed-fee is right for
the schedule, with one surgical exception for `compute_sha512_half`.**
Empirical data from `HostFuncBenchmark` shows `compute_sha512_half`
is wrong at both ends of its legal input range simultaneously
(~2× over at 32 bytes, ~3.9× under at 1024 bytes); no fixed price
in that range can be right. Recommended formula: `gas = 625 + 5 * N`
(Bug #7). For everything else, fixed-at-honest-worst-case is the
right shape — Bugs #1, #2, #5, and #6 are all about that
worst-case calibration being too low today, not about the absence
of per-byte rates. See [§ Dynamic vs Fixed Fees](#dynamic-vs-fixed-fees-is-per-byte-pricing-worth-it).

**Budget sanity check.** At 1,000,000 gas the pure-compute
wall-clock is ~1.8 ms in the worst measured interpreter kernel — fine
on its own. The host-fn-mix worst case is ~110–170 ms typical for
`getNFT` spam (~400–600 ms adversarial) — Critical. The budget cap
is *not* the lever; the host-fn entry prices are.

**Top five remediations, ordered by payoff/effort:**

Numbers below assume the 1M cap stays at its default. Pricing target
is "honest worst-case per call × calls allowed at the cap stays
within a few ms of wall-clock per tx."

1. Lift `getNFT`'s entry price to **≥ 7,000 gas**, parity with (or
   above) `cacheLedgerObj` since it does the same disk work plus
   ~10–20 μs CPU. One-line change in `WasmVM.cpp:64`. At 7K gas,
   1M / 7K = ~143 calls/tx → ~16–24 ms wall-clock typical /
   ~57–86 ms adversarial. Larger bumps (e.g. 50K) give tighter caps
   if needed.
2. Lift `cacheLedgerObj`'s entry price to **≥ 20,000 gas**, ideally
   ~50,000. One-line change in `WasmVM.cpp:27`. At 50K: 20 calls/tx
   → ~2–4 ms wall-clock typical.
3. Lift `check_sig`'s entry price to ~150,000 gas (matches measured
   floor; `WasmVM.cpp:41`). At 1M cap, 1M / 150K = ~6 verifies/tx.
4. Change `compute_sha512_half` to a `gas = 625 + 5 * N` formula
   (Bug #7). The one wrapper in the schedule that genuinely needs
   per-byte pricing — measured data shows fixed cannot be right at
   both ends of the 32–1024 byte legal range.
5. Land `todos.md` TODO 1 (`get*Field` refactor: impl writes
   directly into wasm memory, removing the intermediate `Bytes`
   allocation). Roughly halves the floor cost for the entire
   `getCurrentLedgerObjField` family and lets the existing 70 gas
   be close to honest after a modest bump to ~500 gas (Bug #6).

Bug #3 (`updateData` apply-time accounting) is a transactor-level
change (state-growth surcharge at the escrow level), larger scope
than a one-liner.

---

## Appendix: Provenance of measured numbers

Every quantitative claim in this report ultimately rests on one or
more of three sources. The provenance is split out here so that
re-running validation can target the right inputs.

**Verified directly from current-branch code (no measurement
dependency):**

- The full gas table in `createWasmImport()` (`WasmVM.cpp:21–93`).
- The metering shape: `checkGas` is the only helper; the schedule
  is fixed-fee only.
- wasmi engine config (`WasmiVM.cpp:521–547`): bulk memory off,
  FP off, reference types off, fuel metering on.
- `MAX_PAGES = 128`, `maxWasmDataLength = 4096`,
  `maxWasmParamLength = 1024`, `MAX_CACHE = 256`.
- The architectural claims: `cacheLedgerObj` → `view.read`,
  `getNFT` → SHAMap successor + page read + array scan,
  `updateData` → in-WASM stage then `EscrowFinish::doApply` write,
  `locateField` accepts up to 256 descents.
- NVMe cold-read latency 80–150 μs (from the prompt, not measured).

**Verified empirically from `HostFuncBenchmark` unittest (release
build, M4 Pro):**

- Calibration anchor 1 gas ≈ 0.10 ns (`getLedgerSqn` 6 ns @ 60 gas,
  IQR 0%).
- `getCurrentLedgerObjField` intercept 27 ns, slope 0.02 ns/byte.
- `cacheLedgerObj` in-memory floor 12 ns (uselessly low; the
  meaningful cost is excluded by design).
- `updateData` host-side staging floor 25 ns + 0.01 ns/byte
  (apply-time write excluded).
- `compute_sha512_half` intercept 62.5 ns, slope 0.51 ns/byte.
- `checkSignature` constant ~15 μs across all legal input sizes.

**Inferred from architecture + NVMe physics, not benchmarks:**

- The ~100–200 μs / ~10–30 ms wall-clock estimates for Bug #1
  (`cacheLedgerObj`) and Bug #2 (`getNFT`) on cold caches. These
  depend on the 80–150 μs cold-NVMe figure and on architectural
  reasoning about SHAMap depth and the `findToken` algorithm.
- The ~20 μs apply-time CPU estimate for Bug #3 (`updateData`).
  This is an order-of-magnitude inference from SLE serialization
  cost at 4 KB; not measured.

**On what could be wrong:**

- Hardware-shift risk: the M4 Pro → AMD Threadripper transition
  is expected to be within ~2× for pure CPU per the prompt.
  Anchor and per-byte slopes may shift by that factor; ratios
  between wrappers should be preserved.
- The Bug #3 apply-time estimate is the least empirically
  grounded number in the report and would benefit from a direct
  measurement (instrument `EscrowFinish::doApply` to time the
  `slep->setFieldVL` + `ctx_.view().update` path with a 4 KB
  `sfData`).
- Cold-NVMe assumptions for Bugs #1 and #2 could be confirmed by
  patching the benchmark harness to run against an on-disk
  NodeStore with cache flushing between samples. Out of scope for
  this round but the right next step if Bug #1 or #2 enters a
  formal security review.
