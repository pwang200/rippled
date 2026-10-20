# Smart Escrow performance test plan (draft)

Scope of this doc: the input to the perf test: settings, tx flows, ledger/DB snapshots, and the test cases for EscrowCreate and EscrowFinish. What is recorded, and how results are analyzed, are left to the perf platform's existing tools.

## 1. Purpose

The perf test has two goals:

1. **Impact**: find the average and worst-case impact of wasm EscrowCreate / EscrowFinish on node resources, network performance and anomalies, as for other xrpld features.
2. **Calibration**: check whether the parameters smart escrow introduces are fair: GasLimit, BytecodeSizeLimit, GasPrice, the host function cost table, and possibly wasm engine flags.

Both come from comparing three kinds of contract under the same load:

- **Baseline**: the cheapest possible contract, for both EscrowCreate and EscrowFinish (§5).
- **Average**: one realistic contract, for both EscrowCreate and EscrowFinish (§6).
- **Worst-case candidates**: contracts whose real cost may be high for the fee or gas they are charged: for EscrowCreate (§7) and for EscrowFinish (§8). We don't know which one is worst, so we test several per category.

Each case says which tx flow (§3) and which snapshot (§4) it uses.

The cases in this doc are the set we see as necessary now. Results from them will likely call for new tests, so this is a living doc, updated as the tests progress. Appendix B lists additions we can already foresee.

## 2. Settings and fees

Network: 5-validator networks dedicated for performance testing. The `ripple/se/supported` branch will be used for build. We might also use other branches as needed as the performance testing progresses. 

Settings (S1): the code defaults, which the network keeps throughout. There is no fee voting: validators run without a `[voting]` section.

| GasLimit | BytecodeSizeLimit | GasPrice |
| --- | --- | --- |
| 1,000,000 | 100,000 | 1,000,000 micro-drops (1 drop per gas) |

Every case runs under these limits. Worst cases are hunted under S1 only: a worst case under S1 shows which code paths are underpriced, and S2 rechecks the top ones at smaller limits.

S2 follow-up: once the worst cases are known, a couple of them also run with blobs of 50 KB or less and a Gas allowance of 400,000. We will configure the wasm code generator to use a 50 KB limit, and the gas allowance is capped by the gas field in the tx. The network is still under the S1 limits. No voting needed. 

Fees. The tx generator must compute them, or its autofill must know these rules. 

- Create = 10 × base + 5 drops per bytecode byte: about 0.5 XRP for 100 KB.
- Finish = base + Gas × GasPrice / 1e6 + 1: about 1 XRP at Gas 1,000,000. The fee is on the Gas allowance, not on GasUsed.

Gas allowance: every Finish in every case sets Gas to 1,000,000, whatever the case actually uses. Every Finish then pays the same fee, so the comparison is how much validator time the same fee buys. GasUsed is recorded per case, to also report time per gas used.

The Create side is not equal across cases: modules range from under 100 bytes to ~100 KB, so the Create fee and the work done at Create differ.

Funding and reserve: Owner reserves per escrow = ⌈bytecode bytes / 500⌉, at least 1: 200 for 100 KB. Held until Finish or Cancel. Actor funding = account reserve + escrows in stock × (owner reserves per escrow × owner reserve + Amount + Create fee) + Finish fees. Actors are funded far above this for the largest stock we expect; genesis holds the whole supply on a fresh network, so overfunding costs nothing. A Create failing with tecINSUFFICIENT_RESERVE means the stock outgrew the funding.

## 3. Tx flows

All escrows are self-escrows (Account = Destination = Owner). Every module exports `escrow_finish`. Only the parts unique to wasm escrows are here; generic perf-testing practice (actor accounts, sequences, signing, submission, rates) is the same as regular escrow. One exception: W4b's Creates must spread evenly across all test accounts, and its Finishes must rotate evenly through them (W4b notes, §8).

| Flow | Transactions | Expected |
| --- | --- | --- |
| F1 | EscrowCreate {Bytecode, Amount, CancelAfter} → EscrowFinish {Owner, OfferSequence, Gas} | tesSUCCESS, tesSUCCESS. Escrow removed. |
| F2 | EscrowCreate → EscrowFinish → EscrowCancel after CancelAfter | Create tesSUCCESS. Finish tecBYTECODE_REJECTED (fee charged, escrow kept). Cancel tesSUCCESS. Escrow removed. |

F1 is the default. F2 is needed only where the case requires the escrow to survive a Finish (W3).

Each phase submits only one transaction type, so what is measured during a phase belongs to that type alone. F1 cases run in two phases: a Create-only phase builds a stock of escrows, then a Finish-only phase consumes it. An F2 case runs in three: Create, one Finish per escrow, then Cancel after CancelAfter.

Mixing the phases and mixing with other transaction types such as payment are planned as follow-up tests, after we find the worst cases and decide network parameters.

### EscrowCreate

| Field | Value |
| --- | --- |
| Account, Destination | The actor (self-escrow) |
| Amount | A small XRP amount |
| Bytecode | The case's fixed blob |
| CancelAfter | Required whenever Bytecode is present. Long enough for the finish phase to run, and short enough to cancel leftovers if we want to keep the same number of ledger objects before and after the test. |
| Data | None, except W3: 1,024 bytes |
| FinishAfter, Condition | None |

One blob for every Create in a case is fine: the check reads only the blob, i.e. the performance is not ledger data dependent.

Size effects:

- A 100 KB Create is about 100 KB on the wire and is relayed to every peer. At high rates, bandwidth may become the limit instead of wasm.
- Each escrow stores its full bytecode in the ledger: 10K escrows of 100 KB add about 1 GB of state. 

### EscrowFinish

| Field | Value |
| --- | --- |
| Account, Owner | The actor that created the escrow (self-escrow) |
| OfferSequence | The Create's Sequence |
| Gas | 1,000,000 for every case |
| Fulfillment, Condition, CredentialIDs | None |

The Finish carries no bytecode: the node reads the module from the escrow in the ledger.

Timing: with no FinishAfter, a Finish is valid as soon as its Create is validated, until CancelAfter. After CancelAfter, a Finish fails with tecNO_PERMISSION before the wasm runs.

| Cases | Each Finish | Then |
| --- | --- | --- |
| F1 (all except W3) | tesSUCCESS: every module returns 1 | Escrow removed |
| F2 (W3) | tecBYTECODE_REJECTED: the module returns 0; fee charged, escrow kept | EscrowCancel after CancelAfter, tesSUCCESS |

Any other result means the case is broken, for example tecOUT_OF_GAS (wrong allowance or module) or tecNO_PERMISSION (past CancelAfter). The test should stop, not keep measuring.

### Cleanup (soft goal)

Aim: the ledger is about the same size before and after the test, which helps to read xrpld memory usage. 

- F1 cleans itself: each Finish deletes its escrow, so consuming the whole stock returns the ledger to about its starting size. Only balances and sequence numbers change.
- F2 cleans up through the Cancel it needs anyway.
- We don't currently have a case that needs dedicated clean up. If we do, one or two failed cleanups are acceptable.

## 4. Ledger snapshots

We build two snapshots once, on a standalone node, with standard transactions, using the same build, amendments and fee settings (S1) as the network. We thought about using a mainnet snapshot, but it is unclear how to add the smart escrow amendment. We also need to add accounts and NFTs as we planned for fresh snapshots below. If we can easily customize the mainnet snapshot, we should use it since all planned tests write to the ledger, and a large and realistic ledger helps. Appendix A gives the build steps.

| Snapshot | Contents | Used by |
| --- | --- | --- |
| Small | The test accounts, funded per §2 | B1, A1, W1–W3, CW1–CW3 |
| Data | The small snapshot, plus filler accounts and full NFT pages held by the test accounts | W4, plus B1 as W4's own baseline |

- The data snapshot is built on top of the small one, so the test accounts and keys are identical and the tool config doesn't change.
- B1 also runs on the data snapshot because every transaction's ledger work (account reads, escrow writes) is slower in a bigger state. W4 compared against B1 on the small snapshot would count that as wasm cost.
- The small snapshot is quick to build, copy and start, and gives a clean memory baseline.

The perf platform copies the snapshot's DB directory to each validator and starts the validators with the load option. We confirm the exact start flags with a local 5-node trial first. Restoring the snapshot before each case keeps the cases comparable.

Data snapshot contents:

- Every test account holds 200 full pages of NFTs it minted itself: 32 NFTs per page, each with a 256-byte URI. So every test account is a W4b actor.
- 10M filler accounts, so W4a's misses walk about mainnet's tree depth, through a tree far larger than the CPU cache (W4a notes, §8).

The number of test accounts is still open. More test accounts keep W4b's reads uncached up to a higher Finish rate (W4b notes, §8). For reference, mainnet has about 8.4M accounts (mid-2026); its total object count isn't published. 

### Data snapshot results are likely worse than mainnet

- A load start leaves the state in the DB, so a read of state not touched since the start is a DB fetch. The fetch is usually served from the OS page cache, because startup reads every node once to check the ledger.
- A mainnet validator starts normally and acquires the ledger from the network, which puts the whole state tree in memory. Its reads are in-memory lookups.
- So W4 on the network is a pessimistic bound.
- Start sending transactions at least one tree-cache age after the validators are up (30 seconds to 15 minutes, depending on node_size), so nodes read at startup have expired from the tree node cache.
- `get_counts` (admin) reports node store reads, hits and read time, which shows whether W4's reads reach the DB.
- Physical disk reads: a contract can't reliably cause a large number of them if the above arguments hold. So we don't plan to introduce OS page-cache drop between tests.

Note, pre-warm (not planned; only if results need it): the first read of a tree node fetches it from the DB, and the node then stays in memory for later ledgers, so W4's DB reads fade during a run. For W4b this takes one round of the rotation. For W4a at 10M objects, the deepest tree level stays mostly cold for 100K+ Finishes, so its result depends on run length and rate. If that makes the results hard to read, page through the whole ledger with `ledger_data` on each validator before W4 and its B1 run: every node then stays in memory, as on mainnet. We would provide the script.

## 5. Baseline

This is the simplest contract. It returns 1 when executed. 

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| B1 | `return_1` | | | Returns 1, no host calls | F1 | Small, and Data (as W4's baseline) |

Notes:

- Local time: Create check ~0.002 ms [r].

Footnote:
Local time sources used in this plan. They are not directly comparable, so compare candidates within one source:

- **[r]** xrpld's own code from the Rust crates on `se-hostfn-timing`, in one long-running process on a Mac, median of 25 runs: the Create check, or the Finish path (compile, instantiate, call). Run-to-run noise is about ±20%.
- **[w]** standalone wasmi on a Mac, median of 25 runs, not through xrpld. Its interpreter build runs execution about 3× faster than xrpld's (W1a: 0.34 ms [w] against 0.89 ms [r]).
- **[x]** xrpld release build (Oct 1), `WASM_TIMING`, single shot. Includes a fixed cost of about 140 µs per Finish, and reads high against [r]: se-soak's 15 KB translation case took 838 µs [x] but 0.11 ms [r].

## 6. Average case

**`refill_when_low`**: "the escrow is a reserve tank that unlocks only when the owner's XRP balance is low."

The contract:

1. Reads the escrow's `Account` (the owner).
2. Computes the owner's account keylet, loads it with `cache_ledger_obj`, and reads `Balance` (drops).
3. Converts drops to XRP with float ops (`float_from_int`, `float_div` by 1e6).
4. Checks whether the balance is below 1,000,000 XRP (`float_cmp`), but always returns 1 to simplify the tx flow.

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| A1 | `refill_when_low` | | | Reads the owner's balance, compares it, returns 1 | F1 | Small |

Notes:

- Compiled Rust, not hand-written WAT. Most of its gas is translation (7 gas per byte of code that runs), so a WAT version would be unrealistically cheap. The same goes for its Create: a compiled module has the realistic shape for the check.
- Its Create and Finish together give the report one cost for the average escrow's whole life.

## 7. Worst-case candidates (EscrowCreate)

Create charges no gas. Its fee is 10 × base + 5 drops per bytecode byte, and its node cost is the check EscrowCreate preclaim runs after reading the ledger's fee settings (upload switch and size limit): compile the module, then screen its imports and entry point, etc. The check reads only the blob, builds a fresh engine and caches nothing, so a fixed blob per case costs the same on every Create. Preclaim runs on every apply, so the check runs at least twice on each validator. A potential future measurement, with the timing log build (Appendix B): compare Create cases on time per Create and time per fee drop.

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| CW1 | `09-many-exports` (= W1h) | | | ~19.4k exports of one function | F1 | Small |
| CW2 | `08-many-types` (= W1g) | | | ~7.7k distinct function types | F1 | Small |
| CW3 | `t-0-control` (= W1f) | | | ~2,400 unused functions with 50k locals each | F1 | Small |

Why chosen (local times; all ~100 KB; time per fee drop assumes base = 10 drops):

- CW1: one function exported under ~19.4k short names. Create check 4.20 ms [r], `Module::new` 5.22 ms [w], ~8.4 ns per fee drop.
- CW2: ~7.7k distinct function types. 3.22 ms [r], 4.30 ms [w], ~6.4 ns per fee drop.
- CW3: Create case 07's shape: ~2,400 never-called 40-byte functions, each declaring 50k locals, filled with a short `br_table`. W1f's blob (wasmi T-0) has this shape with a finish that returns 1. 1.64 ms [r], 1.79 ms [w], ~3.3 ns per fee drop.

Notes:

- The 40-byte average rule limits function bodies only. CW1 and CW2 fill the module with sections that have no bodies; CW3 sits on the rule.
- [r] and [w] are defined in §5. Both are a Mac, not a server, so the ranking may differ on the network; that is why all three are kept.
- The same blobs serve W1f–W1h (§8), so finishing one of these escrows also exercises its W1 case.
- Source: the wasmi experiment `experiments/module-new-time` on branch `experiment/module-new-time`. CW1 and CW2 are from `create_cases` (07 is from the earlier hunt; 08–11 were added on 2026-10-15). CW3 is from `real_total` (T-0).
- Dropped:
  - `10-duplicate-types`: the same code path as CW2, and about equal (2.94 ms [r]).
  - `11-many-imports` (~4.8k copies of a host-function import): 0.91 ms [r]; xrpld's import screening is cheap.
  - `01-active-element`: 0.98 ms [r], and its escrow can never finish, because instantiation refuses the oversized table.
  - 02–06: single tricks that 07 combines or beats.

## 8. Worst-case candidates (EscrowFinish)

Numbers in the "Why chosen" notes are from earlier versions of these modules, in our unit tests on `se-hostfn-timing` (based on smart-escrow 4103ade0ba) or in the local runs noted. The regenerated modules will differ. The tables' Size and GasUsed columns are filled from the network build.

Combo cases come later. Each case below isolates one trick. Once they are measured on the network, we build a few combos from the worst ones. Only tricks that spend different budgets add up: module load (W1f–W1h) spends bytes and almost no gas, while execution and host calls (W1a–W1c, W2–W4) spend gas in a small module. W1d and W1e spend both, so they don't combine with either. Two tricks that spend the same budget only trade against each other. For example, W1h's exports use bytes and almost no gas, so the gas left over can run the worst execution or host-call kernel in the same module.

### W1. Pure wasm instructions

Cost comes from the engine alone: no host calls. The time goes to five different places, and each is a separate pricing question, so there is one candidate per place, except module load, which has three different code paths.

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| W1a | `t-00-br-loop` | | | Execution (reference): counted loop | F1 | Small |
| W1b | `call_indirect_unroll` | | | Execution: 32 `call_indirect` per loop | F1 | Small |
| W1c | `chain_8mb` | | | Execution, memory: pointer chase over the full 8 MB | F1 | Small |
| W1d | `if_else_100k` | | | Translation: one 100 KB if/else body | F1 | Small |
| W1e | `dos_large_finish_many_helpers` | | | Function entry: calls ~100 KB of small functions once each | F1 | Small |
| W1f | `t-0-control` | | | Module load: ~2,400 unused functions with 50k locals each | F1 | Small |
| W1g | `08-many-types` | | | Module load: ~7.7k distinct function types | F1 | Small |
| W1h | `09-many-exports` | | | Module load and instantiation: ~19.4k exports of one function | F1 | Small |

Why chosen:

- W1a (wasmi T-00): the reference, not a worst case. A 59-byte counted loop: ~948K gas, 0.89 ms, ~0.94 ns/gas [r]; 0.34 ms [w]. Any candidate well above its ~0.94 ns/gas [r] is underpriced.
- W1b: 32 `call_indirect` per loop, in a small module. The earlier version was padded to ~100 KB: 4.25 ms [r] near the full 1M gas, of which ~1.6 ms was loading the padding, so ~2.6 ms was execution.
- W1c: 8 MB is the largest working set a module can have, so it is the memory case with the most room to leave the cache. Building the working set and chasing it share the gas budget. It replaces se-soak's 1 MB `chain_full_footprint` (~1.0 ns/gas [r]), which fits in a Mac's cache and mostly in a server core's L2, so it measured barely above W1a. The validators' CPU cache sizes help read W1c's result.
- W1d: one ~100 KB function body of repeated `local.get 0; if; else; end`, translated on first call: ~750K gas, 2.0–2.4 ms [r]. It replaces se-soak's 15 KB `dos_large_finish_linear`. Translation is 7 gas per byte, and each byte is translated at most once per run, so at the 100 KB cap translation gives ~700K gas; the rest is the body running once. Reaching 1M would need a loop, which would mix in execution. The shape comes from a hunt on [r] (2026-10-16), one ~100 KB body each, in ns/gas: if/else 2.6–3.2, if without else 2.4, nested ifs 2.4, if/else with a result 2.2, nested blocks 1.9, `br_if` in a block 1.5, select 1.4, two-locals add 1.3, `br_table` 1.3, se-soak's const + add 0.85.
- W1e (se-soak): calls small functions once each. se-soak's version has 250 in ~18 KB, at ~1.9 ns/gas [r]; ours fills the module to ~100 KB, about 5× as many, to spend more of the gas.
- W1f (wasmi T-0, Create case 07's shape): ~2,400 never-called functions at the 40-byte floor, each declaring 50k locals; trivial finish. ~275 gas, 1.66 ms [r].
- W1g (wasmi create_cases 08): ~7.7k distinct function types, one trivial function. 30 gas, 3.21 ms [r].
- W1h (wasmi create_cases 09): one function exported under ~19.4k short names; every instantiation builds the export map again. 30 gas, 8.40 ms [r], about half compile, half instantiate.
- W1f–W1h: loading and instantiating the module is not charged gas; xrpld's Finish path reports 30 gas for W1g and W1h. Every Finish pays the load again, and Create pays it once too. Their gas can't be raised to match the others, so compare them on time per Finish against B1. They are the same blobs as the Create worst cases (CW1–CW3, §7).

Notes:

- The se-soak versions patch a random pad into each Create. That isn't needed here, because the node builds a fresh engine for every run.
- Dropped: `inst_data` / `inst_elem` (instantiation work, 30 gas, 231 / 303 µs [x]) are the same kind of unbilled cost as W1f but much smaller. `many_locals` / `inst_locals` are fixed by the wasmi locals PR.

### W2. Host functions with compute-only cost

Flat-priced host functions whose real cost depends on input or algorithm. No ledger reads.

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| W2a | `sha512_half_loop` | | | `sha512_half` on the max input, repeated | F1 | Small |
| W2b | `check_sig_loop` | | | `check_sig` on a valid signature, repeated | F1 | Small |
| W2c | `float_loop` | | | Float ops on the costliest inputs, repeated | F1 | Small |

W2a: `sha512_half` on 1024 bytes (the max input), as many calls as fit in 1M gas.

W2b: `check_sig` is priced at 35,000 gas. W2b checks that the price holds.

Not tested: the NFT ID functions (issuer, taxon, flags, transfer fee, serial; 60–70 gas) are compute-only, but they only decode bits of the 32-byte ID, so their cost is small and constant.

### W3. Write path: `set_data`

`set_data` is the only call that writes, and the write persists only when the escrow is not released. On tesSUCCESS the escrow is deleted. So this category uses F2.

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| W3 | `set_data_max` | | | Replaces the 1 KB of Data set at Create, returns 0 | F2 | Small |

Each EscrowCreate sets the escrow's Data field to one fixed, non-repeating 1024-byte value, the same for every Create, so every Finish replaces 1 KB of existing Data.

The module, about 1,000 gas:

1. A data segment at offset 0 holds 1024 fixed, non-repeating bytes, chosen once when we build the module, so every Create uses the same blob. The data segment is part of the wasm module, not the escrow's Data field. It costs no gas: it is loaded at instantiation, and `set_data` reads it from memory, not the wasm code. Non-repeating bytes keep the DB's compression from shrinking the write.
2. `set_data(0, 1024)` (1000 gas), called once, then return 0.

Why the Data must differ: when a field changes, the metadata also records its old value, about 1 KB more per Finish. A contract that really updates its state pays that, so the worst case must too. The Create's Data and the module's data segment are two different fixed values, so every Finish changes the Data.

Why one `set_data`: the escrow is written once per Finish, from the last `set_data` buffer. More calls only add 1 KB memory copies at a flat 1000 gas each, which is unlikely to beat W1a's reference rate.

Each escrow is finished once, then cancelled after CancelAfter. The local soak driver does this (`cancel_removes`).

### W4. Data-dependent ledger reads

Every ledger read costs a flat 5000 gas, so W4 looks for the read that takes the validator the most real time. A read's time has three parts:

1. Find: walk the state tree from the root to the object.
2. Load: get the object's bytes, from memory or the DB.
3. Parse and use: turn the bytes into an object, then do the host function's work on it. This grows with the object's size.

W4a pushes part 1: its keys don't exist, so the walk runs and nothing is loaded. W4b pushes parts 2 and 3: each call loads a full NFT page, parses it and scans its 32 NFTs. A full page with 256-byte URIs is about 9 KB, a large object for a 5000-gas read; a typical object such as an AccountRoot is about 200 bytes. W4b's walk is long too: an NFT page's key starts with the owner's account ID, not a hash of it, so an owner's pages sit about 50 levels deep, against about 6.5 for a random key. All 199 calls of one Finish walk the actor's own path, so after the first call it is in the CPU cache.

Data read recently is cheaper to read again, so each case must keep reading data that isn't in a cache:

- CPU cache: tens to hundreds of MB on a server. The data a case cycles through must be far larger.
- Parsed-object cache: the open ledger keeps a parsed object until a full minute passes with no read of it. Only applying a transaction to the open ledger uses it. Building the consensus ledger reads the ledger directly, so every Finish parses its objects at least once on each validator, at any rate. `get_counts` reports this cache's `SLE_hit_rate`.

B1 runs on the same data snapshot (§4) as their baseline.

| ID | Module | Size | GasUsed | What it does | Flow | Snapshot |
| --- | --- | --- | --- | --- | --- | --- |
| W4a | `cache_ledger_obj_soak` | | | `cache_ledger_obj` on missing keys | F1 | Data |
| W4b | `get_nft_pages` | | | `get_nft` on the actor's own full NFT pages, one page per call | F1 | Data |

#### W4a notes

Each call asks for a 32-byte key that matches no object. A miss is not cheap: the node walks the state tree from the root toward the key, reading each node on the path from memory or the DB, until it reaches an empty branch. A bigger state means a longer path and more DB reads.

The module, about 1M gas:

1. Call `home_le_field` once to read the escrow's PreviousTxnID into a 32-byte key buffer (70 gas). In F1 this is the Create's transaction ID, so it differs for every escrow.
2. Repeat 199 times, the most that fit in 1M gas:
   - Add 1 to the key's first byte (a few gas).
   - Call `cache_ledger_obj` (wasm name `cache_le`) on the key (5000 gas). It returns not found, which the module ignores.
3. Return 1.

Why changing the first byte is enough: the tree branches on the key's hex digits, one per level: the root on the first byte's high digit, the next level on its low digit. So each new first byte sends the key into a different subtree two levels down, and below that the paths share no nodes, even though the other 31 bytes are the same. The top two levels are about 270 nodes that every lookup touches anyway. The first byte has 256 values, enough for 199 keys.

Example, calls 1 and 2 of one Finish whose PreviousTxnID is `3A7F91C2…5168`:

```
call 1: 3B7F91C2E04B5D86A1F3C7290B6E8D4152A9F0C3B7E6D1844F2A9C0E7B3D5168
call 2: 3C7F91C2E04B5D86A1F3C7290B6E8D4152A9F0C3B7E6D1844F2A9C0E7B3D5168
```

Both go from the root down branch `3`, then split into `3B…` and `3C…`, different subtrees from there on.

Why 10M filler accounts (§4): the first byte can't decide how deep a miss walks or whether its nodes are in the CPU cache; the state size does. A simulation of the tree with random keys:

| State objects | Nodes per miss | Below the top ~270 | Inner nodes in memory |
| --- | --- | --- | --- |
| 1M | 5.7 | 2.7 | ~100 MB |
| 10M | 6.5 | 3.5 | ~0.9 GB |

At 10M the tree is far larger than a server's CPU cache, and a miss walks about as deep as on mainnet. At 1M, a large server's last-level cache could hold much of it.

Why PreviousTxnID as the base: no two Finishes read the same keys, even two Finishes from one account in the same ledger or with Tickets. Repeated keys would find the path already in memory and under-measure.

#### W4b notes

Every test account in the data snapshot holds 200 full pages of NFTs it minted itself (Appendix A). Page i holds exactly the NFTs whose ID byte 24 is i. Each Finish reads 199 of its own actor's pages, one per call.

How the node finds a page: `get_nft` (wasm name `nft_uri`) takes an account and an NFT ID. The node builds a lookup key from the account (20 bytes) and the ID's last 12 bytes, reads the first of the account's pages whose key is greater, and compares each of its 32 NFTs with the ID. Those last 12 bytes are the issuer's last 4 bytes (ID bytes 20–23), the stored taxon (24–27) and the mint sequence (28–31). The other 20 bytes are used only in the final compare.

The lookup ID, 32 bytes, preset by a data segment:

| Bytes | Value | Why |
| --- | --- | --- |
| 0–19 | 00 | Not used to find the page |
| 20–23 | The actor's last 4 bytes, copied at run time | Every NFT the actor issued has them, so the lookup lands among its pages |
| 24 | i, set per call | Picks page i |
| 25–31 | FF | Keeps the lookup above page i−1's key, which has i in byte 24 and 00 in byte 25. No real NFT has FF in byte 25, so every call misses |

A miss is the worst case: the page is parsed either way, and a miss compares all 32 NFTs, while a hit can stop early.

The module, about 1M gas:

1. `tx_field(Account)` (field code 524289, 70 gas) writes the actor's 20-byte account to memory. In a self-escrow, the Finish's Account is the escrow owner and holds the pages.
2. Copy the account's last 4 bytes into ID bytes 20–23.
3. For i = 0 … 198 (199 calls, the most that fit in 1M gas):
   - Write i into ID byte 24.
   - Call `get_nft` with the account, the ID and a 256-byte output buffer (5000 gas). It returns not found, which the module ignores.
4. Return 1.

The blob has no account list and nothing per escrow, so every Create uses the same blob.

Requirement for the tx generator: every Finish by one actor reads the same 199 pages, so W4b's Finishes must rotate evenly through all test accounts (round-robin). Then each actor's pages come back as late as possible.

How the caches apply:

- Parsed-object cache: an actor's pages are still cached only if it finishes again within a minute. About 60 actors per Finish/s keep the open-ledger work at 0% hits. With fewer, part of the open-ledger work is cached; the consensus-ledger work never is. `SLE_hit_rate` shows the share.
- CPU cache: an actor's 200 pages are about 1.8 MB. With hundreds of actors in rotation, the pages read between two turns of one actor far exceed the CPU cache.

#### W4c (skipped for now)

We considered a W4c that reads other smart escrow objects with `cache_ledger_obj`. An escrow holds up to 100 KB of bytecode, and the read loads and parses all of it. But the wasm needs each escrow's exact key, a hash of its owner and Create sequence, which makes the setup more complex, and it is uncertain whether it costs more than W4b. So we skip it for now.

#### W4d (skipped for now)

We considered a W4d that misses under many NFT owners' page keys. Such a miss walks about 48 levels, against about 6.5 for W4a's random keys. To keep the walks out of the CPU cache, each call needs a different owner holding 2 or more pages, so the module would carry an owner list (100 KB fits about 5,000) and the snapshot would need many such owners. Mainnet already has them, so this is a real attack shape. But it is more complex to set up, and it is uncertain whether it costs more than W4b. So we skip it for now.

## Appendix A. Building the ledger snapshots

We build both snapshots once, on a standalone node with the network's build, amendments and S1 settings (§2), using standard transactions.

Build node config:

- Raise `minimum_txn_in_ledger_standalone` (`[transaction_queue]`, default 1,000) well above the transactions sent per ledger, so fee escalation doesn't start. Close ledgers often with `ledger_accept`.
- Turn on `online_delete`. Every mint rewrites a page, and its metadata holds the page twice (old and new), so without pruning the DB keeps tens of GB of history.

Small snapshot:

1. Fund the test accounts from genesis with Payments, per §2.
2. Close the ledger and save the DB.

Data snapshot, on top of the small one:

1. Fund the 10M filler accounts from genesis with Payments (W4a). At 1,000 Payments/s, that is about 3 hours.
2. Fund every test account with 200 more owner reserves, for its NFT pages.
3. Every test account mints its 200 pages (below).
4. Close the ledger and save the DB.

For both snapshots, confirm they hold the S1 fee settings, and confirm a 5-validator start with a local 5-node trial (§4).

### NFT pages, per test account

Each account mints 6,400 NFTs to itself: 200 pages × 32. Each mint is an NFTokenMint {Account = the account, NFTokenTaxon = computed, URI = 256 bytes}, with no Flags and no TransferFee.

- URI: 256 bytes, different for every NFT and non-repeating (for example built from SHA-512 of account, page and NFT number), so the DB's compression can't shrink the pages.
- Placement: NFT k (0–31) of page i (0–199) must have the stored taxon (i << 24) | (k << 16) in ID bytes 24–27. For example page 5, NFT 3 → 0x05030000. Byte 24 is then the page number.
- Taxon to send: the ID stores Taxon XOR (384160001 × Seq + 2459) mod 2^32, where Seq is the NFT's mint sequence: the Sequence of the account's first NFTokenMint plus the number of NFTs it minted before (FirstNFTokenSequence + MintedNFTokens on its AccountRoot). So send Taxon = stored taxon XOR (384160001 × Seq + 2459) mod 2^32. If the account sends only mints, Seq is the mint's own Sequence.
- Mint order: first NFTs 0–15 of every page, page by page (0, 1, …, 199), then NFTs 16–31 of every page, page by page. This gives 200 pages of exactly 32, page i holding exactly page i's NFTs (checked with a simulation of the split rule). Why the order matters is below.
- Accounts mint in parallel; each account's mints go in Sequence order.

Example, for an account whose first NFTokenMint has Sequence 7:

| Mint # | Page | NFT | Seq | Stored taxon | Taxon to send |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 | 0 | 7 | 0x00000000 | 0xA048C0A2 |
| 2 | 0 | 1 | 8 | 0x00010000 | 0xB72F91A3 |
| 17 | 1 | 0 | 23 | 0x01000000 | 0x0FA5D0B2 |
| 3,200 | 199 | 15 | 3206 | 0xC70F0000 | 0x05187C21 |
| 3,201 | 0 | 16 | 3207 | 0x00100000 | 0xD8ED4D22 |
| 6,400 | 199 | 31 | 6406 | 0xC71F0000 | 0x3DD308A1 |

NFTokenTaxon takes the value as a plain integer, for example 0xA048C0A2 = 2,689,122,466.

Failures and restarts: the Taxon is fixed when a mint is signed. If one mint fails, every later mint already signed for that account lands on the wrong page. After each ledger, check the results; on any failure, drop that account's pending mints, read FirstNFTokenSequence and MintedNFTokens with `account_info`, and sign again. MintedNFTokens also tells which NFT is next (the first 3,200 mints are NFTs 0–15 of each page), so a stopped build resumes from the ledger alone.

Time: 6,400 mints per account. At 100 mints/s, that is about 64 seconds per account, so 1,000 accounts take about 18 hours. Signing millions of transactions needs a compiled signer or parallel processes.

Checks: for a few accounts, `account_objects` type `nft_page` shows 200 pages of 32 NFTs, and every NFT on page i has ID byte 24 = i.

### Why the mint order matters

After the first mint, a new page is created only by splitting a full page in half, at the middle of what it holds at that moment. So every page starts with 16 NFTs and must receive its other 16 after the split.

Scaled-down example: a page holds 4 and splits 2 + 2. Page 0 should hold {00 01 02 03}, page 1 {10 11 12 13}, page 2 {20 21 22 23}. Both orders below are the same 12 mints, one at a time:

```
increasing:   00 01 02 03 10 11 12 13 20 21 22 23
  The first page fills with 00 01 02 03 and splits between 01 and 02.
  [00 01] never receives another NFT; every page ends half full.

halves first: 00 01 10 11 20 21 | 02 03 12 13 22 23
  The first page fills with 00 01 10 11 and splits between 01 and 10, on the page boundary.
  02 03 fill [00 01]. 12 splits [10 11 20 21] between 11 and 20, then 12 13 fill [10 11] and 22 23 fill [20 21].
  Result: page 0 = {00 01 02 03}, page 1 = {10 11 12 13}, page 2 = {20 21 22 23}.
```

## Appendix B. Possible additions

Not planned yet; they may be needed once first results are in:

- A new xrpld build with a per-transaction timing log for EscrowCreate and EscrowFinish, and analysis of that log. The current code has none, and the perf platform's metrics give the time per transaction only indirectly. Starting point: the "time measurement logging" commit 6973159c7e on branch `se-soak`, which times EscrowCreate and EscrowFinish.
- New or changed cases suggested by first-round results.
