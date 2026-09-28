(module
  (import "host_lib" "ldgr_index" (func $ldgr_index (param i32 i32) (result i32)))
  (import "host_lib" "tx_field" (func $tx_field (param i32 i32 i32) (result i32)))
  (import "host_lib" "sha512_half" (func $sha512_half (param i32 i32 i32 i32) (result i32)))
  (import "host_lib" "cache_le" (func $cache_le (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)

  ;; Handoff soak probe: one reusable contract, no per-escrow bytecode generation and no
  ;; pre-staged accounts. Each call to escrow_finish derives its own 142 target keys from
  ;; live execution state, so distinct finishes (different ledger, or different finishing
  ;; account within the same ledger) automatically land in unrelated parts of keyspace with
  ;; no coordination between callers required.
  ;;
  ;; Seed layout in memory:
  ;;   [0,20)  finisher's Account (sfAccount, field code 524289 = (STI_ACCOUNT=8 << 16) | 1;
  ;;           include/xrpl/protocol/detail/sfields.macro:334, include/xrpl/protocol/SField.h
  ;;           fieldCode()) via `tx_field` — differs whenever a different account finishes.
  ;;   [20,52) current ledger sequence via `ldgr_index` — differs every ledger close; the
  ;;           host writes fewer than 32 bytes, the unused tail stays zero, which still
  ;;           contributes to (rather than weakens) the hash input.
  ;;   [52,56) per-iteration counter, so the 142 calls within one run don't repeat a key.
  ;;   [56,88) sha512_half's output (32 bytes) — used directly as cache_le's obj_id, giving
  ;;           142 effectively-independent, uniformly-distributed keys per run.
  ;;
  ;; Fuel: 142 was sized for the flat 5000 (cache_le) + 2000 (sha512_half) gas per iteration
  ;; against a 1,000,000 budget (~7020/iteration incl. loop bookkeeping), leaving headroom
  ;; over the one-time tx_field/ldgr_index setup cost so the run completes rather than
  ;; trapping OutOfFuel. See HostFunctionTiming.cpp for the calibration methodology.
  (func (export "escrow_finish") (result i32)
    (local $i i32)
    (local $r i32)

    (drop (call $tx_field (i32.const 524289) (i32.const 0) (i32.const 20)))
    (drop (call $ldgr_index (i32.const 20) (i32.const 32)))

    (local.set $i (i32.const 0))
    (block $done
      (loop $again
        (br_if $done (i32.eq (local.get $i) (i32.const 142)))
        (i32.store (i32.const 52) (local.get $i))
        (drop (call $sha512_half (i32.const 0) (i32.const 56) (i32.const 56) (i32.const 32)))
        (local.set $r (call $cache_le (i32.const 56) (i32.const 32) (i32.const 1)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $r)))
