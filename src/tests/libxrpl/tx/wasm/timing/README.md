# Smart Escrow timing probes

Test contracts for comparing real execution time (engine and host functions) with gas pricing. `HostFunctionTiming.cpp` runs them through the real VM and prints wall time and gas used.

| Module (`wat/`, `wasm/`) | Does | Gas (of budget) |
| --- | --- | --- |
| `sha512_half_loop` | 496 × `sha512_half` on 1024 bytes | 999,277 of 1,000,000 |
| `sha512_half_loop_400k` | 198 × `sha512_half` on 1024 bytes | 399,105 of 400,000 |
| `cache_ledger_obj_soak` | 142 × (`sha512_half` + `cache_le`, key not found) | 997,833 of 1,000,000 |
| `cache_ledger_obj_soak_400k` | 56 × (`sha512_half` + `cache_le`, key not found) | 393,934 of 400,000 |
| `call_indirect_unroll` | ~100 KB; 9200 iterations × 32 `call_indirect` (no host calls) | 939,965 of 1,000,000 |
| `call_indirect_unroll_400k` | ~50 KB; 3700 iterations × 32 `call_indirect` (no host calls) | 378,958 of 400,000 |
| `return_1` | no host calls, returns 1 (also the timing baseline) | 30 |

- Every module exports `escrow_finish` and returns 1. Gas assumes this branch's flat host costs, so another gas schedule changes it.
- `call_indirect_unroll*` come from the wasmi `module-new-time` experiment (`real_total/t-b-*`). The `.wat` is only a sketch, and the export was renamed from `finish` to `escrow_finish` in the `.wasm`.
- The `.wasm` files are committed. `CommittedWasmFilesRun` and `CallIndirectUnrollRuns` run them through EscrowCreate preflight and the VM.

Run: `xrpl_tests --gtest_filter='HostFunctionTiming.*'`
