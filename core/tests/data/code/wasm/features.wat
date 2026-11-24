(module
  (memory 1)
  (table $t 4 externref)
  (data $passive "abc")
  (elem $e func $a $b)
  (table $ft 3 funcref)
  (func $a (result i32 i32) i32.const 1 i32.const 2)
  (func $b (param i32) (result i32) local.get 0 i32.extend8_s)
  (func (export "bulk") (param i32)
    local.get 0 i32.const 0 i32.const 3 memory.init $passive
    data.drop $passive
    local.get 0 i32.const 100 i32.const 3 memory.copy
    local.get 0 i32.const 255 i32.const 10 memory.fill
    i32.const 0 i32.const 0 i32.const 2 table.init $ft $e
    elem.drop $e)
  (func (export "refs") (param externref) (result i32)
    i32.const 0 local.get 0 table.set $t
    ref.null extern
    ref.is_null
    ref.func $a
    drop
    table.size $t
    i32.add)
  (func (export "sat") (param f32) (result i32) local.get 0 i32.trunc_sat_f32_s)
  (func (export "multi") (result i32) call $a i32.add)
  (func (export "typed_select") (param i64 i64 i32) (result i64) local.get 0 local.get 1 local.get 2 select (result i64))
  (func (export "block_types") (result i32)
    i32.const 5
    block (param i32) (result i32)
      i32.const 1
      i32.add
    end)
)
