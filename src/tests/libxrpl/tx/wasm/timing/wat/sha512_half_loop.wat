(module
  (import "host_lib" "sha512_half" (func $sha512_half (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)

  ;; Calls sha512_half 496 times: measured exactly (not estimated) by solving
  ;; `totalGas = fixed + count * perIteration` from two runs (count=100 -> 201841 gas,
  ;; count=480 -> 967541 gas), giving fixed=341, perIteration=2015 (2000 host gas + 15 fuel
  ;; of loop bookkeeping: br_if, four i32.const, call, two local.set, i32.sub, br). 496 is the
  ;; largest count with `341 + 496*2015 = 999,781 <= 1,000,000`; 497 would need 1,001,796 and
  ;; trap OutOfFuel before returning.
  ;;
  ;; Input is `kMaxWasmDataLength` (1024 bytes, include/xrpl/protocol/Protocol.h) — the most
  ;; the host will read (`MAX_FIELD_BYTES`, crates/xrpl-wasm-vm/src/vm.rs). The gas charge is
  ;; flat regardless of length (OpenSSL's SHA512_Update, src/libxrpl/protocol/digest.cpp, is
  ;; O(length) in 128-byte blocks), so this is worst-case real time for the price paid: ~9
  ;; compression blocks per call versus 1 at 32 bytes, same 2000 gas either way. Content
  ;; doesn't matter — no data-dependent branching — so zeroed memory is fine.
  (func (export "escrow_finish") (result i32)
    (local $i i32)
    (local $r i32)
    (local.set $i (i32.const 496))
    (block $done
      (loop $again
        (br_if $done (i32.eqz (local.get $i)))
        (local.set $r (call $sha512_half (i32.const 0) (i32.const 1024) (i32.const 8192) (i32.const 32)))
        (local.set $i (i32.sub (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $r)))
