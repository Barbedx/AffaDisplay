#!/usr/bin/env node
// readme_consumer.js — build a consumer project from README.md, so the README cannot rot.
//
//   node tools/readme_consumer.js <dir> <platform>     e.g. /tmp/consumer espressif32@6.13.0
//
// Writes <dir>/platformio.ini from the README's Install block (lib_deps pointed at this
// checkout instead of the tag) and <dir>/src/main.cpp from its Minimal example. Then
// `pio run -d <dir>` builds it as a user would.
'use strict';
const fs = require('fs');
const path = require('path');

const [dir, platform] = process.argv.slice(2);
if (!dir || !platform) { console.error('usage: readme_consumer.js <dir> <platform>'); process.exit(2); }

const root = path.join(__dirname, '..');
const readme = fs.readFileSync(path.join(root, 'README.md'), 'utf8').replace(/\r\n/g, '\n');

// The first ```<lang> block after the heading.
function block(heading, lang) {
  const at = readme.indexOf(`\n## ${heading}\n`);
  if (at < 0) { console.error(`README.md has no "## ${heading}"`); process.exit(1); }
  const m = readme.slice(at).match(new RegExp('\\n```' + lang + '\\n([\\s\\S]*?)\\n```\\n'));
  if (!m) { console.error(`no \`\`\`${lang} block under "## ${heading}"`); process.exit(1); }
  return m[1];
}

const install = block('Install', 'ini');
const repoLine = /^\s*https:\/\/github\.com\/Barbedx\/AffaDisplay\.git#v\S+\s*$/m;
if (!repoLine.test(install)) { console.error('the Install block has no AffaDisplay lib_deps line'); process.exit(1); }
const ini = [
  '[env:consumer]',
  `platform = ${platform}`,
  'board = esp32dev',
  'framework = arduino',
  'build_src_flags = -Wall -Wextra',
  install.replace(repoLine, `  symlink://${root.replace(/\\/g, '/')}`),
  '',
].join('\n');

fs.mkdirSync(path.join(dir, 'src'), { recursive: true });
fs.writeFileSync(path.join(dir, 'platformio.ini'), ini);
fs.writeFileSync(path.join(dir, 'src', 'main.cpp'), block('Minimal example', 'cpp') + '\n');
console.log(ini);
