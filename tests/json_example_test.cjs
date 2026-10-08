const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const assert = require('node:assert/strict');
const [program, directory] = process.argv.slice(2);
const file = path.join(directory, 'json-generated.json');
let state = 123456;
function random(limit) { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state % limit; }
function generated(depth) {
  const leaves = [null, false, true, -12.5, 0, 'Hello ü 🚀', '\n"\\', '\b\f\r\t/'];
  if (depth === 0) return leaves[random(leaves.length)];
  if (random(2)) return Array.from({ length: random(4) }, () => generated(depth - 1));
  return Object.fromEntries(Array.from({ length: random(4) }, (_, i) => [`key${i}`, generated(depth - 1)]));
}
function tree(value, depth = 0) {
  const indent = '  '.repeat(depth);
  if (value === null || typeof value !== 'object') return indent + JSON.stringify(value) + '\n';
  if (Array.isArray(value)) return indent + 'array\n' + value.map(child => tree(child, depth + 1)).join('');
  return indent + 'object\n' + Object.entries(value).map(([key, child]) =>
    '  '.repeat(depth + 1) + 'key ' + JSON.stringify(key) + '\n' + tree(child, depth + 1)).join('');
}
function run(input, segments = []) {
  fs.writeFileSync(file, input);
  const result = spawnSync(program, [file, ...segments], { encoding: 'utf8', windowsHide: true, timeout: 10000 });
  assert.ifError(result.error);
  return result;
}
for (let i = 0; i < 24; i++) {
  const value = generated(4);
  const result = run(JSON.stringify(value));
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.equal(result.stdout.replace(/\r\n/g, '\n'), tree(value));
}
for (const input of ['"\t"', '"\u0000"', '+1', '00', '[true false]', '{x:1}',
  '"\\uDC00"', '"\\uD800\\u1234"', Buffer.from([0x22, 0xc0, 0xaf, 0x22])]) {
  const result = run(input);
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stdout, /JSON error at byte/);
}
const query = '{"config":{"message":"Hello\\n\\uD83D\\uDE80","ports":[8080,9000]},"ready":false,"nothing":null}';
for (const [segments, expected] of [
  [['config', 'message'], '"Hello\\n🚀"\n'],
  [['config', 'ports', '1'], '9000\n'],
  [['ready'], 'false\n'], [['nothing'], 'null\n']
]) {
  const result = run(query, segments);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.equal(result.stdout.replace(/\r\n/g, '\n'), expected);
}
for (const segments of [['missing'], ['ready', 'child'], ['config', 'ports', '99'], ['config', 'ports', '-1']]) {
  assert.equal(run(query, segments).status, 3);
}
const duplicate = run('{"a":1,"\\u0061":2}', ['a']);
assert.equal(duplicate.status, 0);
assert.equal(duplicate.stdout.trim(), '2');
console.log('24 generated JSON values, 9 malformed/encoding cases and 9 key/index queries passed');
