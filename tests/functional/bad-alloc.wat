(module
  ;; A module whose `nix_wasm_alloc` export has the wrong type (the size is
  ;; an i64 rather than an i32). The host must reject it at instantiation.
  (memory (export "memory") 1)

  (func (export "nix_wasm_init_v1"))

  (func (export "nix_wasm_alloc") (param i64 i32) (result i32)
    (i32.const 0))

  (func (export "id") (param i32) (result i32)
    (local.get 0)))
