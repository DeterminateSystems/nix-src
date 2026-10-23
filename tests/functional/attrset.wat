(module
  ;; A module that copies an attrset into Wasm memory using `get_attrset`
  ;; and returns it as a list `[ name1 value1 name2 value2 ... ]`.
  (import "env" "make_string" (func $make_string (param i32 i32) (result i32)))
  (import "env" "make_list" (func $make_list (param i32 i32) (result i32)))
  (import "env" "get_attrset" (func $get_attrset (param i32 i32 i32) (result i64)))

  (memory (export "memory") 1)

  ;; Memory layout: a buffer for `get_attrset` at 1024, the list
  ;; elements at 8192, and the heap for `nix_wasm_alloc` from 16384.
  (global $heap (mut i32) (i32.const 16384))

  (func (export "nix_wasm_init_v1"))

  ;; A bump allocator. The heap pointer stays aligned to 4 bytes, which is
  ;; the only alignment that `get_attrset` asks for.
  (func (export "nix_wasm_alloc") (param $size i32) (param $align i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $heap))
    (global.set $heap
      (i32.and (i32.add (i32.add (global.get $heap) (local.get $size)) (i32.const 3)) (i32.const -4)))
    (local.get $ptr))

  ;; Decode the buffer written by `get_attrset`.
  (func $decode (param $buf i32) (result i32)
    (local $n i32)
    (local $i i32)
    (local $name i32)
    (local $end i32)
    (local $out i32)
    (local.set $n (i32.load (local.get $buf)))
    ;; The names start after the count and the value IDs.
    (local.set $name
      (i32.add (local.get $buf) (i32.add (i32.const 4) (i32.mul (local.get $n) (i32.const 4)))))
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        ;; Find the null terminator of the name.
        (local.set $end (local.get $name))
        (block $found
          (loop $scan
            (br_if $found (i32.eqz (i32.load8_u (local.get $end))))
            (local.set $end (i32.add (local.get $end) (i32.const 1)))
            (br $scan)))
        (local.set $out (i32.add (i32.const 8192) (i32.mul (local.get $i) (i32.const 8))))
        (i32.store
          (local.get $out)
          (call $make_string (local.get $name) (i32.sub (local.get $end) (local.get $name))))
        (i32.store
          (i32.add (local.get $out) (i32.const 4))
          (i32.load
            (i32.add (local.get $buf) (i32.add (i32.const 4) (i32.mul (local.get $i) (i32.const 4))))))
        (local.set $name (i32.add (local.get $end) (i32.const 1)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (call $make_list (i32.const 8192) (i32.mul (local.get $n) (i32.const 2))))

  ;; Pass a buffer that's large enough, so the host must use it.
  (func (export "attrs_inline") (param $value i32) (result i32)
    (local $res i64)
    (local.set $res (call $get_attrset (local.get $value) (i32.const 1024) (i32.const 4096)))
    (if (i32.ne (i32.wrap_i64 (local.get $res)) (i32.const 1024))
      (then unreachable))
    (call $decode (i32.wrap_i64 (local.get $res))))

  ;; Pass a buffer that's too small, so the host must call `nix_wasm_alloc`.
  (func (export "attrs_alloc") (param $value i32) (result i32)
    (local $res i64)
    (local.set $res (call $get_attrset (local.get $value) (i32.const 1024) (i32.const 0)))
    (if (i32.ne (i32.wrap_i64 (local.get $res)) (i32.const 16384))
      (then unreachable))
    (call $decode (i32.wrap_i64 (local.get $res)))))
