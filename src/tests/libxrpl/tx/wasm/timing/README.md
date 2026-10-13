# Host-function timing probes

Hand-written Smart Escrow contracts used to measure real host-function cost against flat gas pricing. `HostFunctionTiming.cpp` runs them through the real VM and prints wall time and gas used.

| Module (`wat/`, `wasm/`) | Does | Gas (of budget) |
| --- | --- | --- |
| `sha512_half_loop` | 496 × `sha512_half` on 1024 bytes | 999,277 of 1,000,000 |
| `sha512_half_loop_400k` | 198 × `sha512_half` on 1024 bytes | 399,105 of 400,000 |
| `cache_ledger_obj_soak` | 142 × (`sha512_half` + `cache_le`, key not found) | 997,833 of 1,000,000 |
| `cache_ledger_obj_soak_400k` | 56 × (`sha512_half` + `cache_le`, key not found) | 393,934 of 400,000 |
| `baseline_no_host_calls` (`.wat` only) | returns 1 | 30 |

- Every module exports `escrow_finish` and returns 1.
- Imports come from `host_lib`: `ldgr_index`, `tx_field`, `sha512_half`, `cache_le`.
- Gas figures assume this branch's flat costs (2000 for `sha512_half`, 5000 for `cache_le`). A different gas schedule changes them, and a higher one can trap OutOfFuel.
- The `.wasm` files are generated from the `.wat` by the tests and committed. `CommittedWasmFilesRun` fails if one is stale.

Run: `xrpl_tests --gtest_filter='HostFunctionTiming.*'`
