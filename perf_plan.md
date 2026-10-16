# Smart Escrow performance test plan (draft)

Scope of this doc: the input to the perf test: settings, tx flows, ledger/DB snapshots, and the test cases for EscrowCreate and EscrowFinish. What is recorded, and how results are analyzed, are existing perf platform's tools. Note the smart escrow tx processing code has no per-transaction timing log, but can be added if needed. 

## 1. Purpose

Measure what wasm EscrowCreate / EscrowFinish cost a real network, by comparing three kinds of contract under the same load:

- **Baseline**: the cheapest possible contract, for both EscrowCreate and EscrowFinish (§5).
- **Average**: one realistic contract, for both EscrowCreate and EscrowFinish (§6).
- **Worst-case candidates**: contracts whose real cost may be high for the fee or gas they are charged: for EscrowCreate (§7) and for EscrowFinish (§8). We don't know which one is worst, so we test several per category.

Each case says which tx flow (§3) and which snapshot (§4) it uses.

## 2. Settings and fees

Network: 5-validator networks dedicated for performance testing. The `ripple/se/supported` branch will be used for build. We might also use other branches as needed as the performance testing progress. 

Settings (S1): the code defaults, which the network keeps throughout. There is no fee voting: validators run without a `[voting]` section.

| GasLimit | BytecodeSizeLimit | GasPrice |
| --- | --- | --- |
| 1,000,000 | 100,000 | 1,000,000 micro-drops (1 drop per gas) |

Every case runs under these limits. Worst cases are hunted under S1 only, since a worst case applies to any limits.

S2 follow-up: once the worst cases are known, a couple of them also run with blobs of 50 KB or less and a Gas allowance of 400,000. We will config the wasm code generator to use 50KB limit, and the gas allowance is capped by the gas field in the tx. The network is still under the S1 limits. No voting needed. 

Fees. The tx generator must compute them, or its autofill must know these rules. 

- Create = 10 × base + 5 drops per bytecode byte: about 0.5 XRP for 100 KB.
- Finish = base + Gas × GasPrice / 1e6 + 1: about 1 XRP at Gas 1,000,000. The fee is on the Gas allowance, not on GasUsed.

Gas allowance: every Finish in every case sets Gas to 1,000,000 (400,000 in the S2 follow-up), whatever the case actually uses. Every Finish then pays the same fee, so the comparison is how much validator time the same fee buys. GasUsed is recorded per case, to also report time per gas used.

The Create side is not equal across cases: modules range from 59 bytes to ~100 KB, so the Create fee and the work done at Create differ.

Funding and reserve: Owner reserves per escrow = ⌈bytecode bytes / 500⌉, at least 1: 200 for 100 KB. Held until Finish or Cancel. Actor funding = account reserve + escrows in stock × (owner reserves per escrow × owner reserve + Amount + Create fee) + Finish fees.

TODO footnote:
Each Create check and each Finish run at least twice on every validator: once when applied to the open ledger, once when the consensus ledger is built.

TODO footnote:
Local time sources used in this plan. They are not directly comparable, so compare candidates within one source:

- **[r]** xrpld's own code from the Rust crates on `se-hostfn-timing`, in one long-running process on a Mac, median of 25 runs: the Create check, or the Finish path (compile, instantiate, call). Run-to-run noise is about ±20%.
- **[w]** standalone wasmi on a Mac, median of 25 runs, not through xrpld. Its interpreter build runs execution about 3× faster than xrpld's (W1a: 0.34 ms [w] against 0.89 ms [r]).
- **[x]** xrpld release build (Oct 1), `WASM_TIMING`, single shot. Includes a fixed cost of about 140 µs per Finish, and reads high against [r]: se-soak's 15 KB translation case took 838 µs [x] but 0.11 ms [r].

## 3. Tx flows

All escrows are self-escrows (Account = Destination = Owner). Every module exports `escrow_finish`. Only the parts unique to wasm escrows are here; generic perf-testing practice (actor accounts, sequences, signing, submission, rates) is the same as regular escrow.

| Flow | Transactions | Expected |
| --- | --- | --- |
| F1 | EscrowCreate {Bytecode, Amount, CancelAfter} → EscrowFinish {Owner, OfferSequence, Gas} | tesSUCCESS, tesSUCCESS. Escrow removed. |
| F2 | EscrowCreate → EscrowFinish × N → EscrowCancel after CancelAfter | Create tesSUCCESS. Each Finish tecBYTECODE_REJECTED (fee charged, escrow kept). Cancel tesSUCCESS. |

F1 is the default. F2 is needed only where the case requires the escrow to survive a Finish (W3).

Each phase submits only one transaction type, so what is measured during a phase belongs to that type alone. F1 cases run in two phases: a Create-only phase builds a stock of escrows, then a Finish-only phase consumes it. A F2 case runs in three: Create, repeated Finishes on the same escrows, then Cancel after CancelAfter.

Mixing the phases and mixing with other transaction types such as payment are planned as follow-up tests, after we found the worst cases and decided network parameters.

### EscrowCreate

| Field | Value |
| --- | --- |
| Account, Destination | The actor (self-escrow) |
| Amount | A small XRP amount |
| Bytecode | The case's fixed blob (its S1 version, or its S2-sized version in the follow-up) |
| CancelAfter | Required whenever Bytecode is present. Long enough for the finish phase to run, and short enough to cancel leftovers if we want to keep the same number of ledger objects before and after the test. |
| Data | None, except W3b: 1,024 bytes |
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
| Gas | 1,000,000 for every case (400,000 in the S2 follow-up) |
| Fulfillment, Condition, CredentialIDs | None |

The Finish carries no bytecode: the node reads the module from the escrow in the ledger.

Timing: with no FinishAfter, a Finish is valid as soon as its Create is validated, until CancelAfter. After CancelAfter, a Finish fails with tecNO_PERMISSION before the wasm runs.

| Cases | Each Finish | Then |
| --- | --- | --- |
| F1 (all except W3) | tesSUCCESS: every module returns 1 | Escrow removed |
| F2 (W3) | tecBYTECODE_REJECTED: the module returns 0; fee charged, escrow kept | Repeat on the same escrow; EscrowCancel after CancelAfter, tesSUCCESS |

Any other result means the case is broken, for example tecOUT_OF_GAS (wrong allowance or module) or tecNO_PERMISSION (past CancelAfter). The test should stop, not keep measuring.

### Cleanup (soft goal)

Aim: the ledger is about the same size before and after the test, which helps to read xrpld memory usage. 

- F1 cleans itself: each Finish deletes its escrow, so consuming the whole stock returns the ledger to about its starting size. Only balances and sequence numbers change.
- F2 cleans up through the Cancel it needs anyway.
- We don't currently have a case that needs dedicated clean up. If we do, one or two failed clean up is acceptable.

## 4. Ledger snapshots

We build two snapshots once, on a standalone node, with standard transactions, using the same build, amendments and fee settings (S1) as the network. We thought about using mainnet snapshot, but unclear how to add the smart escrow amendment. We also need to add accounts and NFTs as we planned for fresh snapshots below. If we can easily customize the mainnet snapshot, we should use it since all planned tests write to the ledger, and a large and realistic ledger helps.  

| Snapshot | Contents | Used by |
| --- | --- | --- |
| Small | The test accounts, funded per §3 | B1, A1, W1–W3, CW1–CW3 |
| Data | The small snapshot, plus filler accounts and NFT owners holding full pages | W4, plus B1 as W4's own baseline |

- The data snapshot is built on top of the small one, so the test accounts and keys are identical and the tool config doesn't change.
- B1 also runs on the data snapshot because every transaction's ledger work (account reads, escrow writes) is slower in a bigger state. W4 compared against B1 on the small snapshot would count that as wasm cost.
- The small snapshot is quick to build, copy and start, and gives a clean memory baseline.

The perf platform copies the snapshot's DB directory to each validator and starts the validators with the load option. We confirm the exact start flags with a local 5-node trial first. Restoring the snapshot before each case keeps the cases comparable.

Data snapshot contents:

- NFT owners with keypairs, each minting full pages: 32 NFTs per page, each with a 256-byte URI (W4b).
- Filler accounts, so W4a's misses walk a realistic tree depth.

Sizes are still open: the number of filler accounts and of NFT pages. For reference, mainnet has about 8.4M accounts (mid-2026); its total object count isn't published. 

### Data snapshot results are likely worse than mainnet

- A load start leaves the state in the DB, so a read of state not touched since the start is a DB fetch. The fetch is usually served from the OS page cache, because startup reads every node once to check the ledger.
- A mainnet validator starts normally and acquires the ledger from the network, which puts the whole state tree in memory. Its reads are in-memory lookups.
- So W4 on the network is a pessimistic bound.
- Start the load at least one tree-cache age after the validators are up (30 seconds to 15 minutes, depending on node_size), so nodes read at startup have expired from the tree node cache.
- `get_counts` (admin) reports node store reads, hits and read time, which shows whether W4's reads reach the DB.
- Physical disk reads: a contract can't reliably cause a large number of them if the above arguments hold. So we don't plan to introduce OS page-cache drop between tests.

## 5. Baseline

The contract simply return 1.

| ID | Module | Flow | Snapshot | Create | Finish |
| --- | --- | --- | --- | --- | --- |
| B1 | `return_1` (60 bytes, no host calls) | F1 | Small, and Data (as W4's baseline) | tesSUCCESS. Fee 10 × base + 300 drops; check ~0.002 ms [r] (for a 39-byte module) | tesSUCCESS, GasUsed 30 |

## 6. Average case

**A1 `refill_when_low`**: "the escrow is a reserve tank that unlocks only when the owner's XRP balance is low."

The contract:

1. Reads the escrow's `Account` (the owner).
2. Computes the owner's account keylet, loads it with `cache_ledger_obj`, and reads `Balance` (drops).
3. Converts drops to XRP with float ops (`float_from_int`, `float_divide` by 1e6).
4. Check if the balance is below 1,000,000 XRP (`float_compare`), but always return 1 to simply tx flow.

| Flow | Snapshot | Create | Finish |
| --- | --- | --- | --- |
| F1 | Small; actors funded with less than 1,000,000 XRP | tesSUCCESS. Size to measure, likely a few KB; fee 10 × base + 5 drops per byte | tesSUCCESS. GasUsed estimated 15K–40K. |

Notes:

- Compiled Rust, not hand-written WAT. Most of its gas is translation (7 fuel per byte of code that runs), so a WAT version would be unrealistically cheap. The same goes for its Create: a compiled module has the realistic shape for the check.
- Its Create and Finish together give the report one cost for the average escrow's whole life.

## 7. Worst-case candidates (EscrowCreate)

Create charges no gas. Its fee is 10 × base + 5 drops per bytecode byte, and its node cost is the check EscrowCreate preclaim runs after reading the ledger's fee settings (upload switch and size limit): compile the module, then screen its imports, entry point and exported memory and table. The check reads only the blob, builds a fresh engine and caches nothing, so a fixed blob per case costs the same on every Create. Preclaim runs on every apply, so the check runs at least twice on each validator, like a Finish. Create cases are compared on time per Create and time per fee drop.

All Create worst cases use the small snapshot. Each is the Create phase of an F1 run (§3), so its Finish phase follows.

All are ~100 KB. Times per fee drop assume base = 10 drops.

| ID | Module | What it does | Create check [r] | `Module::new` [w] | Time per fee drop | Status |
| --- | --- | --- | --- | --- | --- | --- |
| CW1 | `09-many-exports` | One function exported under ~19.4k short names | 4.20 ms | 5.22 ms | ~8.4 ns | Same blob as W1h |
| CW2 | `08-many-types` | ~7.7k distinct function types | 3.22 ms | 4.30 ms | ~6.4 ns | Same blob as W1g |
| CW3 | `07-combo` shape | ~2,400 never-called 40-byte functions, each declaring 50k locals, filled with a short `br_table` | 1.64 ms | 1.79 ms | ~3.3 ns | Use W1f's blob (T-0): the same shape with a finish that returns 1 |

Notes:

- The 40-byte average rule limits function bodies only. CW1 and CW2 fill the module with sections that have no bodies; CW3 sits on the rule.
- [r] and [w] are defined in §2. Both are a Mac, not a server, so the ranking may differ on the network; that is why all three are kept.
- The same blobs serve W1f–W1h (§8), so finishing one of these escrows also exercises its W1 case.
- Source: the wasmi experiment `experiments/module-new-time/create_cases` on branch `experiment/module-new-time`. 07 is from the earlier hunt; 08–11 were added on 2026-10-15.
- Dropped:
  - `10-duplicate-types`: the same code path as CW2, and about equal (2.94 ms [r]).
  - `11-many-imports` (~4.8k copies of a host-function import): 0.91 ms [r]; xrpld's import screening is cheap.
  - `01-active-element`: 0.98 ms [r], and its escrow can never finish, because instantiation refuses the oversized table.
  - 02–06: single tricks that 07 combines or beats.

## 8. Worst-case candidates (EscrowFinish)

Gas values below are from our unit tests on `se-hostfn-timing` (based on smart-escrow 4103ade0ba) or from the local runs noted. Re-measure them on the network build. Each case is listed with its S1 module. `_400k` variants exist where noted; they are set aside for the S2-sized follow-up (§2).

Combo cases come later. Each case below isolates one trick. Once they are measured on the network, we build a few combos from the worst ones. Only tricks that spend different budgets add up: module size (W1d, W1f–W1h) versus gas (execution in W1a–W1c, host calls in W2–W4). Two tricks that spend the same budget only trade against each other. For example, W1h's exports use bytes and almost no gas, so the gas left over can run the worst execution or host-call kernel in the same module.

### W1. Pure wasm instructions

Cost comes from the engine alone: no host calls. The time goes to five different places, and each is a separate pricing question, so there is one candidate per place, except module load, which has three different code paths. All W1 cases use F1 and the small snapshot.

| ID | Where the time goes | Module | What it does | GasUsed | Local time | Status |
| --- | --- | --- | --- | --- | --- | --- |
| W1a | Execution (reference) | `br_loop` (wasmi T-00) | 59-byte counted loop | ~948K | 0.89 ms, ~0.94 ns/gas [r]; 0.34 ms [w] | Exists; export needs renaming to `escrow_finish` |
| W1b | Execution | `call_indirect_unroll` (+ `_400k`) | 32 `call_indirect` per loop, in a ~100 KB / ~50 KB module | 939,965 (`_400k`: 378,958) | 4.25 ms [r], of which ~1.6 ms is module load | Exists |
| W1c | Execution, memory | `chain_8mb` (new, from se-soak's `chain_full_footprint`) | Pointer chase whose working set spans the full 8 MB memory cap | to measure | to measure | To build; replaces se-soak's 1 MB version |
| W1d | Translation | `if_else_100k` (new) | One ~100 KB function body of repeated `local.get 0; if; else; end`, translated on first call | ~750K | 2.0–2.4 ms [r] | To build; replaces se-soak's 15 KB `dos_large_finish_linear` |
| W1e | Function entry | `dos_large_finish_many_helpers` (se-soak) | Calls 250 small functions once each (~18 KB) | 136,671 | 0.26 ms, ~1.9 ns/gas [r] | Exists in se-soak; extract a fixed blob |
| W1f | Module load, not in gas: function bodies | `create_padding` (wasmi T-0; Create case 07's shape) | ~2,400 never-called functions at the 40-byte floor, each declaring 50k locals; trivial finish | ~275 | 1.66 ms [r] | Exists; export needs renaming to `escrow_finish` |
| W1g | Module load, not in gas: types | `08-many-types` (wasmi create_cases) | ~7.7k distinct function types, one trivial function | 30 | 3.21 ms [r] | Exists; needs `escrow_finish` returning 1 |
| W1h | Module load and instantiation, not in gas: exports | `09-many-exports` (wasmi create_cases) | One function exported under ~19.4k short names; every instantiation builds the export map again | 30 | 8.40 ms [r] (about half compile, half instantiate) | Exists; needs `escrow_finish` returning 1 |

Notes:

- W1a is the reference, not a worst case: any candidate well above its ~0.94 ns/gas [r] is underpriced.
- W1c: 8 MB is the largest working set a module can have, so it is the memory case with the most room to leave the cache. Building the working set and chasing it share the gas budget. It replaces se-soak's 1 MB `chain_full_footprint` (623,563 gas, 0.63 ms, ~1.0 ns/gas [r]), which fits in a Mac's cache and mostly in a server core's L2, so it measured barely above W1a. The validators' CPU cache sizes help read W1c's result.
- W1d: translation is 7 gas per byte, and each byte is translated at most once per run, so at the 100 KB cap translation gives ~700K gas; the rest is the body running once. Reaching 1M would need a loop, which would mix in execution. The shape comes from a hunt on [r] (2026-10-16), one ~100 KB body each, in ns/gas: if/else 2.6–3.2, if without else 2.4, nested ifs 2.4, if/else with a result 2.2, nested blocks 1.9, `br_if` in a block 1.5, select 1.4, two-locals add 1.3, `br_table` 1.3, se-soak's const + add 0.85.
- W1f–W1h: loading and instantiating the module is not charged gas; xrpld's Finish path reports 30 gas for W1g and W1h. Every Finish pays the load again, and Create pays it once too. Their gas can't be raised to match the others, so compare them on time per Finish against B1. They are the same blobs as the Create worst cases (CW1–CW3, §7).
- The se-soak versions patch a random pad into each Create. That isn't needed here, because the node builds a fresh engine for every run.
- Dropped: `inst_data` / `inst_elem` (instantiation work, 30 gas, 231 / 303 µs [x]) are the same kind of unbilled cost as W1f but much smaller. `many_locals` / `inst_locals` are fixed by the wasmi locals PR.

### W2. Host functions with compute-only cost

Flat-priced host functions whose real cost depends on input or algorithm. No ledger reads. All W2 cases use F1 and the small snapshot.

| ID | Module | Shape | Status | GasUsed (`_400k` in brackets) |
| --- | --- | --- | --- | --- |
| W2a | `sha512_half_loop` (+ `_400k`) | `sha512_half` on 1024 bytes (the max input), 496 calls (`_400k`: 198) | Exists | 999,277 (399,105) |
| W2b | `check_sig_loop` | `check_sig` on a constant valid signature, repeated to the cap | To build | to measure |
| W2c | `float_loop` | Float ops repeated to the cap, with the most expensive inputs we know | To build | to measure |

W2b: `check_sig` is 300 gas on the remote branch. Our `update-sig-price` change raises it to 35,000. If that change is in the network build, W2b becomes a check that the new price holds.

W2c: if the Number exponent-loop fix is not in the network build, use the inputs from that finding. W2c is then likely the worst case overall.

Not tested: the NFT ID functions (issuer, taxon, flags, transfer fee, serial; 60–70 gas) are compute-only, but they only decode bits of the 32-byte ID, so their cost is small and constant.

### W3. Write path: `set_data`

`set_data` is the only call that writes, and the write persists only when the escrow is not released. On tesSUCCESS the escrow is deleted. So this category uses F2, with the small snapshot.

| ID | Module | Shape | Status | Expected |
| --- | --- | --- | --- | --- |
| W3a | `set_data_max` | Writes 1024 bytes of Data (the max), different each call, returns 0 | To build | Each Finish tecBYTECODE_REJECTED, escrow Data changed (check with ledger_entry). GasUsed a few thousand. |
| W3b | `set_data_max` with Create carrying a 1024-byte Data | Same, plus a full-size Data stored at Create | Same module | Same |

W3a derives each write from the Finish's own Sequence. Every Finish on the same escrow shares its OfferSequence, and two in one ledger would share the ledger sequence, so neither keeps the writes distinct.

F2 needs the tool to Finish the same escrow repeatedly and Cancel it after CancelAfter. The local soak driver does this (`cancel_removes`, `retry_finish_until_success`).

### W4. Data-dependent ledger reads

Ledger reads cost a flat 5000 gas, but their real cost depends on where the object is (in memory or in the DB) and how large it is. All W4 cases use F1 and the data snapshot (§4), with B1 on the same snapshot as their baseline.

| ID | Module | Shape | Status | GasUsed (`_400k` in brackets) |
| --- | --- | --- | --- | --- |
| W4a | `cache_ledger_obj_soak` (+ `_400k`) | 142 `cache_ledger_obj` lookups of random keys (`_400k`: 56), all not found | Exists; key seed to change (below) | 997,833 (393,934) |
| W4b | `get_nft_pages` | `get_nft` on owners holding full NFT pages, with random NFT IDs; each call reads, parses and scans a ~9 KB page | To build | to measure |

#### W4a notes

- It spends about 29% of its gas on `sha512_half`, which it uses to derive keys. A variant with cheaper key derivation would fit more lookups per gas.
- Its keys now come from the finisher account, the ledger sequence and the loop index, so one account finishing two escrows in the same ledger reads the same keys twice, and the second Finish under-measures. Change the seed to the finisher Account + OfferSequence instead of the ledger sequence: unique per escrow, even if the tool uses Tickets, so the tool needs no scheduling rule.

#### W4b notes

- `get_nft` (wasm name `nft_uri`) costs 5000 gas, the same as `cache_ledger_obj`, but each call reads, parses and scans a page of up to 32 NFTs: about 9 KB with 256-byte URIs, against about 200 bytes for an AccountRoot.
- A miss on an owner who has pages costs about the same as a hit: the call still parses and scans the page, then reports not found. So the module needs only owner IDs, not real NFT IDs.
- Owner IDs are embedded in the blob, 20 bytes each: up to about 4,800 owners in 100 KB, 2,400 in 50 KB.
- Pages are ordered by the low 12 bytes of the NFT ID, which begin with the issuer's last 4 bytes. A random NFT ID must keep those 4 bytes (the owner's own, since owners mint their own NFTs), or every lookup lands on the owner's first or last page.
- The open ledger keeps parsed objects for 1 minute, and a page read again within that minute skips parsing. So the data snapshot needs more full pages than the test reads per minute.
- Dropped: AccountRoot hits (the earlier `cache_hit_storm`). A hit is W4a's lookup plus a small parse, and W4b's page parse covers it.

## 9. Before delivery (our side)

1. Build the modules: A1 `refill_when_low` (Rust, with a measured GasUsed), W1c `chain_8mb`, W1d `if_else_100k`, W2b `check_sig_loop`, W2c `float_loop`, W3 `set_data_max`, W4b `get_nft_pages`. Rename the exports of W1a and W1f, give W1g / W1h an `escrow_finish` that returns 1, and change W4a's key seed to Account + OfferSequence. Extract a fixed blob for W1e from se-soak; it exports `escrow_finish` and returns 5001, a positive value, which releases the escrow. S2-sized versions wait for the follow-up (§2).
2. Check every blob against the network build of the time: run each through that build's Create check and one Finish, so the host-function names and signatures and the entry point all match. Repeat whenever the network build changes.
3. Deliver the blobs as one file per case, in one place.
4. Build the small and data snapshots (sizes in §4), confirm both hold the S1 fee settings, and confirm a 5-validator start from each with a local 5-node trial.
