-- dynamic tier semantics; each check prints "name ok" or "name FAIL got=..."
local fails = 0
local function eq(name, got, want)
  if got == want then printh(name.." ok") else fails += 1 printh(name.." FAIL got="..tostr(got).." want="..tostr(want)) end
end
-- numbers (16.16 fixed point, PICO-8 rules)
eq("div", tostr(1/3, 1), "0x0000.5555")
eq("divzero", tostr(1/0, 1), "0x7fff.ffff")
eq("negdivzero", tostr(-1/0, 1), "0x8000.0001")
eq("mod", -1 % 3, 2)
eq("modneg", 5 % -3, 2)
eq("idiv", 7 \ 2, 3)
eq("idivneg", -7 \ 2, -4)
eq("wrap", 32767 + 1, -32768)
eq("mul", 1.5 * 2.5, 3.75)
eq("shl", 1 << 4, 16)
eq("shr", -16 >> 2, -4)
eq("lshr", tostr(-1 >>> 16, 1), "0x0000.ffff")
eq("rotl", tostr(0x8000 <<> 1, 1), "0x0000.0001")
eq("bxor", 5 ^^ 3, 6)
eq("tilde", 5 ~ 3, 6)
eq("bnot", ~0, -0x0.0001)
eq("pow", 2^3, 8)
eq("abs8000", tostr(abs(0x8000), 1), "0x7fff.ffff")
eq("flr", flr(-1.5), -2)
eq("ceil", ceil(-1.5), -1)
eq("mid", mid(3, 1, 2), 2)
eq("tostr", tostr(0.1), "0.1")
eq("tostrneg", tostr(-2.5), "-2.5")
eq("tonum", tonum("0x10"), 16)
eq("tonumb", tonum("0b101.1"), 5.5)
eq("tonumbad", tonum("zz"), nil)
eq("sqrt", sqrt(16), 4)
eq("sin", sin(0.25), -1)
eq("cos", cos(0), 1)
eq("atan2", atan2(1, 0), 0)
-- strings
eq("concat", "a"..1 .."b"..2.5, "a1b2.5")
eq("len", #"hello", 5)
eq("sub", sub("hello", 2, 3), "el")
eq("subneg", sub("hello", -3), "llo")
eq("index", ("hello")[2], "e")
eq("indexoob", ("hello")[9], nil)
eq("ord", ord("A"), 65)
eq("chr", chr(72, 105), "Hi")
local s = split("1,2,x")
eq("split", s[1] + s[2], 3)
eq("splitstr", s[3], "x")
eq("coerce", "10" + 5, 15)
eq("cmpstr", "a" < "b", true)
-- tables
local t = {1, 2, 3, x = 4, [10] = 5}
eq("tlen", #t, 3)
eq("tkey", t.x + t[10], 9)
add(t, 9)
eq("add", t[4], 9)
del(t, 2)
eq("del", t[2], 3)
eq("deli", deli(t, 1), 1)
eq("count", count({1, nil, 3}), 2)
local n = 0
for v in all({5, 6, 7}) do n += v end
eq("all", n, 18)
n = 0
for k, v in pairs({a = 1, b = 2, c = 3}) do n += v end
eq("pairs", n, 6)
n = 0
for i, v in ipairs({1, 2, nil, 4}) do n += v end
eq("ipairs", n, 3)
local order, lst = "", {"a", "b", "c", "d"}
for v in all(lst) do order ..= v if v == "b" then del(lst, v) end end
eq("allorder", order, "abcd")
-- multiple assignment / returns / varargs
local function mr() return 1, 2, 3 end
local a, b, c, d = mr()
eq("mret", a + b + c, 6)
eq("mretnil", d, nil)
local function va(...) local p = pack(...) return select("#", ...), p.n end
local c1, c2 = va(1, nil, 3)
eq("vararg", c1 + c2, 6)
eq("unpack", select(2, unpack({7, 8, 9})), 8)
local x, y = 1, 2
x, y = y, x
eq("swap", x * 10 + y, 21)
-- closures
local function counter() local i = 0 return function() i += 1 return i end end
local c3 = counter()
c3() c3()
eq("closure", c3(), 3)
local fns = {}
for i = 1, 3 do fns[i] = function() return i end end
eq("loopcapture", fns[1]() + fns[3](), 4)
-- metatables
local V = {}
V.__index = V
V.__add = function(p, q) return setmetatable({x = p.x + q.x}, V) end
V.__eq = function(p, q) return p.x == q.x end
V.__lt = function(p, q) return p.x < q.x end
V.__call = function(self, k) return self.x * k end
V.__tostring = function(self) return "v"..self.x end
V.__len = function() return 42 end
V.__concat = function(p, q) return "cat" end
function V.get(self) return self.x end
local p1, p2 = setmetatable({x = 1}, V), setmetatable({x = 2}, V)
eq("mtadd", (p1 + p2).x, 3)
eq("mteq", p1 + p1 == p2, true)
eq("mtlt", p1 < p2, true)
eq("mtcall", p2(5), 10)
eq("mtstr", tostr(p1), "v1")
eq("mtlen", #p1, 42)
eq("mtconcat", p1..p2, "cat")
eq("method", p2:get(), 2)
local log = {}
local prox = setmetatable({}, {__newindex = function(t, k, v) add(log, k) rawset(t, k, v) end})
prox.a = 1 prox.a = 2
eq("newindex", #log, 1)
-- coroutines
local co = cocreate(function(a) local b = yield(a + 1) return b * 2 end)
local ok1, r1 = coresume(co, 1)
local ok2, r2 = coresume(co, 10)
eq("co", r1 + r2, 22)
eq("codead", costatus(co), "dead")
local ok3, e3 = coresume(cocreate(function() error("boom") end))
eq("coerr", ok3, false)
-- control flow
local total = 0
for i = 1, 10 do
  if i % 2 == 0 then goto continue end
  total += i
  ::continue::
end
eq("goto", total, 25)
local k2 = 0
repeat local z = k2 k2 += 1 until z >= 3
eq("repeat", k2, 4)
local cnt = 0
for i = 32760, 32767 do cnt += 1 end
eq("forwrap", cnt, 8)
cnt = 0
for i = 10, 1, -3 do cnt += 1 end
eq("fordown", cnt, 4)
if (true) eq("shortif", 1, 1)
?"print ok"
-- _ENV
local obj = {z = 3}
local function envf() local _ENV = obj return z end
eq("env", envf(), 3)
-- gc stress: lots of garbage, live data survives
local keep = {}
for i = 1, 2000 do local g = {i, tostr(i), {i}} if i % 100 == 0 then add(keep, g) end end
eq("gc", keep[20][2], "2000")
-- poke2/poke4 with several values write consecutively
poke2(0x4300, 0x1234, 0x5678)
eq("poke2multi", peek2(0x4302), 0x5678)
poke4(0x4300, 1.5, -2)
eq("poke4multi", peek4(0x4304), -2)
-- poke(a, unpack(t)) with more values than the value stack could hold
local big = {}
for i = 1, 9000 do big[i] = i % 256 end
poke(0x8000, unpack(big))
eq("pokeunpack", peek(0x8000 + 8999), 9000 % 256)
poke2(0x4300, unpack({7, 9}, 2))
eq("poke2unpack", peek2(0x4300), 9)
local pk = poke
pk(0x4300, unpack({3, 4}))
eq("pokeunpack_generic", peek(0x4301), 4)
eq("unpackbig", select("#", unpack(big, 1, 5000)), 5000)
printh(fails == 0 and "ALL OK" or (fails.." FAILED"))
