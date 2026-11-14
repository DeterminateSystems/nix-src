(module
  (import "env" "get_attr" (func $get_attr (param i32 i32 i32) (result i32)))
  (import "env" "call_function" (func $call_function (param i32 i32 i32) (result i32)))

  (memory (export "memory") 1)

  ;; Attribute names "f" (offset 0) and "x" (offset 1).
  (data (i32.const 0) "fx")

  (func (export "nix_wasm_init_v1"))

  ;; Given an attrset `{ f, x }`, return `f x` by calling back into Nix.
  (func (export "apply") (param $arg i32) (result i32)
    ;; Store the ValueId of `x` at offset 8 as the argument array.
    (i32.store (i32.const 8)
      (call $get_attr (local.get $arg) (i32.const 1) (i32.const 1)))
    (call $call_function
      (call $get_attr (local.get $arg) (i32.const 0) (i32.const 1))
      (i32.const 8)
      (i32.const 1))
  )
)
