// check_web_ui.js — parse the JS out of every example's web_ui.h and syntax-check it.
//
//   node tools/check_web_ui.js
//
// WHY THIS EXISTS. A console page is a C++ raw string literal, so a broken script compiles,
// links, flashes and serves perfectly — and then does nothing, with no diagnostic anywhere
// except a browser console nobody has open. One lost backslash turned `\n` into a real
// newline inside a string literal and every button on the page stopped working, while the
// firmware reported itself healthy and the panel kept updating.
//
// The build cannot catch it. This can, in about a second.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = path.join(__dirname, '..', 'examples');
let checked = 0, bad = 0;

for (const dir of fs.readdirSync(root)) {
  const f = path.join(root, dir, 'web_ui.h');
  if (!fs.existsSync(f)) continue;
  const src = fs.readFileSync(f, 'utf8');
  for (const m of src.matchAll(/<script>([\s\S]*?)<\/script>/g)) {
    ++checked;
    try {
      new vm.Script(m[1], { filename: f });
    } catch (e) {
      ++bad;
      console.error(`${dir}/web_ui.h: ${e.message}`);
      const line = (e.stack.match(/web_ui\.h:(\d+)/) || [])[1];
      if (line) {
        const around = m[1].split('\n');
        const n = parseInt(line, 10);
        for (let i = Math.max(0, n - 3); i < Math.min(around.length, n + 2); i++)
          console.error(`  ${i + 1 === n ? '>' : ' '} ${around[i]}`);
      }
    }
  }
}
console.log(`${checked} script block(s) checked, ${bad} broken`);
process.exit(bad ? 1 : 0);
