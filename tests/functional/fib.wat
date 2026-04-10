(module
  ;; Import host functions from the Nix env module
  (import "env" "get_int"  (func $get_int  (param i32) (result i64)))
  (import "env" "make_int" (func $make_int (param i64) (result i32)))

  ;; The host requires an exported memory to read/write data
  (memory (export "memory") 1)

  ;; Called once when the module is instantiated; nothing to initialize here
  (func (export "nix_wasm_init_v1"))

  ;; Pure wasm: compute fib(n) recursively
  ;;   fib(0) = 1
  ;;   fib(1) = 1
  ;;   fib(n) = fib(n-1) + fib(n-2)
  (func $fib (param $n i64) (result i64)
    (if (i64.le_s (local.get $n) (i64.const 1))
      (then (return (i64.const 1)))
    )
    (i64.add
      (call $fib (i64.sub (local.get $n) (i64.const 1)))
      (call $fib (i64.sub (local.get $n) (i64.const 2)))
    )
  )

  ;; Entry point: receives ValueId of the input integer, returns ValueId of fib(n)
  ;; Type: fn(arg: u32) -> u32  (ValueId = u32)
  (func (export "fib") (param $arg i32) (result i32)
    (call $make_int
      (call $fib
        (call $get_int (local.get $arg))
      )
    )
  )
)
