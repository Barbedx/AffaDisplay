#!/usr/bin/env node
// check_log_args.js — refuse a side effect inside an AFFA_LOG* argument list.
//
//   node tools/check_log_args.js
//
// With AFFA_ENABLE_LOG=0 (or a lower AFFA_LOG_LEVEL) the macro discards its arguments, so
// AFFA_LOGD("X", "%d", n++) compiles to nothing and the increment goes with it
// (src/util/AffaLog.h). This scans src/ and examples/ for ++, -- or an assignment inside
// the parentheses of any AFFA_LOGE/W/I/D/T call, with string and character literals removed.
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const files = [];
function walk(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (/\.(h|hpp|c|cpp)$/.test(e.name)) files.push(p);
  }
}
walk(path.join(ROOT, 'src'));
walk(path.join(ROOT, 'examples'));

// Literals become empty, comments become spaces; offsets stay put so line numbers are right.
function strip(src) {
  return src
    .replace(/\/\*[\s\S]*?\*\//g, m => m.replace(/[^\n]/g, ' '))
    .replace(/\/\/[^\n]*/g, m => ' '.repeat(m.length))
    .replace(/"(?:\\.|[^"\\\n])*"/g, m => '"' + ' '.repeat(m.length - 2) + '"')
    .replace(/'(?:\\.|[^'\\\n])*'/g, m => "'" + ' '.repeat(m.length - 2) + "'");
}

let bad = 0, calls = 0;
const CALL = /\bAFFA_LOG[EWIDT]\s*\(/g;
for (const f of files) {
  const src = strip(fs.readFileSync(f, 'utf8'));
  let m;
  CALL.lastIndex = 0;
  while ((m = CALL.exec(src))) {
    let depth = 1, i = CALL.lastIndex;
    while (i < src.length && depth > 0) {
      if (src[i] === '(') ++depth;
      else if (src[i] === ')') --depth;
      ++i;
    }
    const args = src.slice(CALL.lastIndex, i - 1);
    if (/#\s*define/.test(src.slice(src.lastIndexOf('\n', m.index) + 1, m.index))) continue;
    ++calls;
    if (/\+\+|--|(?<![=!<>+\-*\/%&|^])=(?!=)|[+\-*\/%&|^]=|<<=|>>=/.test(args)) {
      const line = src.slice(0, m.index).split('\n').length;
      console.error(`${path.relative(ROOT, f)}:${line}: side effect inside an AFFA_LOG argument`);
      ++bad;
    }
  }
}
console.log(`${calls} AFFA_LOG calls checked, ${bad} with a side effect`);
process.exit(bad ? 1 : 0);
