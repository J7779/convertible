#!/usr/bin/env node
/*! Licence banner: kept */
// Regular expressions versus division
const a = 10, b = 2, g = 5;
const div = a / b / g;               // 1
const re = /ab+c/gi.test("xABBC");   // true
const re2 = [/\//, /[/]/, /a\/b/];   // slashes inside classes and escapes
let x = a
/b/g;                                // division across a line: (a / b) / g
const t = `sum ${a + b} and ${`nested ${b * 2}`} and } braces { ${ { k: 1 }.k }`;
const s = 'it\'s // not a comment' + "/* nor this */";
// Automatic semicolon insertion
let i = 1
let j = i
++j
const f = function () {
  return
  42
}
const obj = { "key": 1, other: [1, 2, 3] }
const arrow = (p) => p + + 1;        // unary plus after binary plus
const neg = a - -b;
const minus = a - --i;
const num = 1 .toString() + 1.5.toFixed(1) + 0x1f.toString();
label: for (const v of [1, 2]) { if (v > 1) break label; }
const tpl = (s => s.raw[0])`raw\n`;
class C { #priv = 3; get priv() { return this.#priv; } static [`computed`]() { return 'c'; } }
async function af() { await null; return typeof x }
console.log(div, re, re2.length, x, t, s, i, j, f(), obj.other.length, arrow(1), neg, minus, num, tpl, new C().priv, C.computed())
af().then(v => console.log(v))
