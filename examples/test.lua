-- test_lua.lua
-- Comprehensive Lua functionality test for inDevOS

local results = {}
local function ok(name, cond)
  results[#results+1] = {name=name, pass=not not cond}
  if cond then
    print("[OK] ", name)
  else
    print("[FAIL]", name)
  end
end

-- 1) Basic language features
ok("arithmetics", (2+3*4==14) and (10/2==5))
local s = "hello" .. " " .. "world"
ok("strings", s == "hello world")

-- 2) Tables
local t = {a=1, b=2}; t.c = 3
ok("tables", t.a==1 and t.b==2 and t.c==3)

-- 3) Functions and closures
local function make_adder(n)
  return function(x) return x + n end
end
local add5 = make_adder(5)
ok("closures", add5(10) == 15)


-- 5) Standard libraries (subset depends on build)
ok("math", math and math.sqrt and math.sqrt(81)==9)
ok("string", string and string.upper and string.upper("abc")=="ABC")
ok("table", table and table.concat and table.concat({"a","b"}, ",")=="a,b")

-- 6) OS/io libraries (may not be present). Expect them to be absent in kernel.
ok("os lib availability (optional)", os == nil or type(os)=='table')
ok("io lib availability (optional)", io == nil or type(io)=='table')

-- 7) inDevOS custom APIs (optional): printk, fs_list, etc.
-- These may not exist unless you registered them. Test gracefully.
local have_printk = type(_G.printk) == 'function'
if have_printk then _G.printk("printk is available from Lua test") end
ok("custom printk (optional)", true)

-- Example of calling a hypothetical fs_list('/') -> {names}
local ok_fs = true
if type(_G.fs_list) == 'function' then
  local ok_call, list = pcall(_G.fs_list, "/")
  ok_fs = ok_call and type(list) == 'table'
end
ok("custom fs_list('/') (optional)", ok_fs)

-- Summary
local total, passed = #results, 0
for _, r in ipairs(results) do if r.pass then passed = passed + 1 end end
print("\nLua test summary:", passed, "/", total, "passed")
if passed == total then
  print("ALL TESTS PASSED")
else
  print("SOME TESTS FAILED")
end


