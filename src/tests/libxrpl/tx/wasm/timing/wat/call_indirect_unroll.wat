;; Real total T-B. Kernel B + create-7 filler to 100 KB. Returns 1 (no OOF).
;;
;; `$escrow_finish` unrolls `call_indirect` × 32 per counted iter into empty `$fat`.
;; Survives a local-zero fuel PR. Remaining bytes are unused combo dummies.
(module
  (memory 128)
  (table 1024 funcref)
  (elem (i32.const 0) $fat) ;; × 1024
  (func $fat)
  (func $dummy
    (local i32) ;; × 50000
    (block
      i32.const 0
      br_table 0 0 0
    )
  ) ;; repeated to fill 100 KB
  (func $escrow_finish (export "escrow_finish") (result i32)
    (local i32)
    (local.set 0 (i32.const 9200))
    (loop
      (call_indirect (type $t) (i32.const 0)) ;; × 32
      (local.tee 0 (i32.sub (local.get 0) (i32.const 1)))
      (br_if 0)
    )
    i32.const 1
  )
)
