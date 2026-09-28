(module
  (memory (export "memory") 1)

  ;; No host import at all: isolates whatever `runEscrowWasm` charges just to compile,
  ;; instantiate, and run a module to its first return, with zero host-function work inside.
  (func (export "escrow_finish") (result i32)
    (i32.const 1)))
