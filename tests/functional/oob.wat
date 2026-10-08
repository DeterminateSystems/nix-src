(module
  ;; A module that passes an out-of-range pointer to a host function. The
  ;; host must reject it rather than access memory outside the Wasm memory.
  (import "env" "make_string" (func $make_string (param i32 i32) (result i32)))

  (memory (export "memory") 1)

  (func (export "nix_wasm_init_v1"))

  ;; The memory is one page (64 KiB); this offset is far beyond it.
  (func (export "oob") (param i32) (result i32)
    (call $make_string (i32.const 0xFFFFFF00) (i32.const 16))))
