#!/usr/bin/env node
// changelog_section.js — print one version's section of CHANGELOG.md (the release notes).
//
//   node tools/changelog_section.js 1.0.0
//
// The section is everything between `## <version> ` and the next `## ` heading, without the
// heading itself and the trailing `---`. Exit 1 if there is no such section, so a tag whose
// version has no entry cannot be released with empty notes.
'use strict';
const fs = require('fs');
const path = require('path');

const ver = process.argv[2];
if (!ver) { console.error('usage: changelog_section.js <version>'); process.exit(2); }

const lines = fs.readFileSync(path.join(__dirname, '..', 'CHANGELOG.md'), 'utf8').split(/\r?\n/);
const start = lines.findIndex(l => l.startsWith(`## ${ver} `) || l === `## ${ver}`);
if (start < 0) { console.error(`CHANGELOG.md has no "## ${ver}" section`); process.exit(1); }

let end = lines.findIndex((l, i) => i > start && l.startsWith('## '));
if (end < 0) end = lines.length;
const body = lines.slice(start + 1, end);
while (body.length && /^(\s*|---)$/.test(body[body.length - 1])) body.pop();
while (body.length && body[0].trim() === '') body.shift();
if (!body.length) { console.error(`the "## ${ver}" section is empty`); process.exit(1); }
process.stdout.write(body.join('\n') + '\n');
