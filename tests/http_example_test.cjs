// Exercise the example over real TCP, independently of the DMM implementation.
const { spawn } = require('node:child_process');
const net = require('node:net');
const assert = require('node:assert/strict');
const server = spawn(process.argv[2], ['0', '--timeout-ms', '2000'], { windowsHide: true });
let log = '', port;
server.stdout.on('data', data => { log += data; });
server.stderr.on('data', data => { log += data; });
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function request(parts, halfClose = false) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: '127.0.0.1', port });
    let output = '';
    socket.setTimeout(15000, () => socket.destroy(new Error('client timeout')));
    socket.on('error', reject);
    socket.on('data', data => { output += data.toString('latin1'); });
    socket.on('end', () => { socket.destroy(); resolve(output); });
    socket.on('connect', async () => {
      for (const part of parts) { socket.write(part); await delay(20); }
      if (halfClose) socket.end();
    });
  });
}
function check(output, status, body, head = false) {
  const split = output.indexOf('\r\n\r\n');
  assert.ok(split >= 0, output);
  const headers = output.slice(0, split);
  assert.ok(headers.startsWith(`HTTP/1.1 ${status} `), headers);
  assert.equal(output.slice(split + 4), head ? '' : body);
  if (status === 204 || status === 304) assert.ok(!headers.includes('Content-Length:'));
  else assert.equal(Number(headers.match(/Content-Length: (\d+)/)[1]), Buffer.byteLength(body, 'latin1'));
  assert.ok(headers.includes('Connection: close'));
}
function checkJson(output, status, expected) {
  const body = output.slice(output.indexOf('\r\n\r\n') + 4);
  check(output, status, body);
  assert.deepEqual(JSON.parse(Buffer.from(body, 'latin1').toString('utf8')), expected);
  assert.ok(output.includes('Content-Type: application/json; charset=utf-8\r\n'));
  return body;
}
(async () => {
  try {
    for (let i = 0; i < 250; i++) {
      const address = log.match(/HTTP server listening on http:\/\/127\.0\.0\.1:(\d+)/);
      if (address) { port = Number(address[1]); break; }
      if (server.exitCode !== null) throw new Error(log);
      await delay(20);
    }
    assert.ok(port, log);
    check(await request(['GET / HTTP/1.1\r\nHost: localhost\r\n\r\n']), 200, 'Hello from DMM!\n');
    check(await request(['GET /health HTTP/1.1\r\nHo', 'st: localhost\r\n', '\r\n']), 200, 'ok\n');
    check(await request(['HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n']), 200, 'Hello from DMM!\n', true);
    check(await request(['HEAD /health HTTP/1.0\r\n\r\n']), 200, 'ok\n', true);
    check(await request(['GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n']), 404, 'Not found\n');
    check(await request(['HEAD /missing HTTP/1.1\r\nHost: localhost\r\n\r\n']), 404, 'Not found\n', true);
    const post = await request(['POST / HTTP/1.1\r\nHost: localhost\r\n\r\n']);
    check(post, 405, 'Method not allowed\n');
    assert.ok(post.includes('Allow: GET, HEAD, OPTIONS\r\n'));
    check(await request(['garbage\r\n\r\n']), 400, 'Bad request\n');
    check(await request(['GET / HTTP/9.0\r\n\r\n']), 400, 'Bad request\n');
    check(await request(['GET / HTTP/1.1\r\nHost:'], true), 400, 'Bad request\n');
    check(await request(['x'.repeat(8192)]), 431, 'Headers too large\n');
    assert.equal(await request(['GET / HTTP/1.1\r\n']), '');
    check(await request(['GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n']), 200, 'ok\n');
    const get = path => request([`GET ${path} HTTP/1.1\r\nHost: localhost\r\n\r\n`]);
    check(await get('/?ignored=yes'), 200, 'Hello from DMM!\n');
    check(await get('/api/items'), 200, '{"items":[]}\n');
    const title = 'Learn DMM "web APIs" 🚀\nnext';
    const body = Buffer.from(title);
    const created = await request([
      `POST /api/items HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ${body.length}\r\n\r\n`,
      body.subarray(0, 5), body.subarray(5),
    ]);
    checkJson(created, 201, {id: 1, title});
    assert.ok(created.includes('Location: /api/items/1\r\n'));
    const item = await get('/api/items/1');
    const itemBody = checkJson(item, 200, {id: 1, title});
    assert.ok(item.includes('Content-Type: application/json; charset=utf-8\r\n'));
    checkJson(await get('/api/items/%31'), 200, {id: 1, title});
    const listed = await get('/api/items');
    checkJson(listed, 200, {items: [{id: 1, title}]});
    check(await request(['HEAD /api/items/1 HTTP/1.1\r\nHost: localhost\r\n\r\n']), 200, itemBody, true);
    check(await get('/api/items/no-number'), 400, 'Bad request\n');
    check(await get('/api/items/99'), 404, 'Not found\n');
    const options = await request(['OPTIONS /api/items/1 HTTP/1.1\r\nHost: localhost\r\n\r\n']);
    check(options, 204, '');
    assert.ok(options.includes('Allow: GET, HEAD, DELETE, OPTIONS\r\n'));
    const wrongMethod = await request(['PUT /api/items HTTP/1.1\r\nHost: localhost\r\n\r\n']);
    check(wrongMethod, 405, 'Method not allowed\n');
    assert.ok(wrongMethod.includes('Allow: GET, HEAD, POST, OPTIONS\r\n'));
    check(await request(['POST /missing HTTP/1.1\r\nHost: localhost\r\n\r\n']), 404, 'Not found\n');
    check(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nabc']), 415, 'Use Content-Type: text/plain\n');
    check(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n']), 422, 'Title must contain 1..256 UTF-8 bytes\n');
    const greeting = await request(['GET /api/greet?na%6De=Gr%C3%BC%C3%9Fe+%22world%22 HTTP/1.1\r\nHost: localhost\r\nx-ReQuEsT-iD: test-42\r\n\r\n']);
    checkJson(greeting, 200, {message: 'Hello, Grüße "world"!'});
    assert.ok(greeting.includes('X-Request-ID: test-42\r\n'));
    check(await get('/api/greet?name=%FF'), 400, 'Bad request\n');
    check(await get('/api/greet?name=%'), 400, 'Bad request\n');
    check(await get('/api/items/'), 404, 'Not found\n');
    check(await request(['DELETE /api/items/1 HTTP/1.1\r\nHost: localhost\r\n\r\n']), 204, '');
    check(await get('/api/items/1'), 404, 'Not found\n');
    check(await get('/api/items'), 200, '{"items":[]}\n');
    for (const headers of [
      'Host: localhost\r\nContent-Length: 0\r\nContent-Length: 0\r\n',
      'Host: localhost\r\nContent-Length: -1\r\n',
      'Host: localhost\r\nContent-Length: 1x\r\n',
      'Host: localhost\r\nContent-Length: 18446744073709551616\r\n',
      'Host: localhost\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n',
      'Host: localhost\r\nHost: other\r\n',
      'Host: \r\n', 'No-Colon\r\n', ' Host: localhost\r\n',
      'Host: localhost\r\nX-Bad: value\x00\r\n', '',
    ]) check(await request([`GET / HTTP/1.1\r\n${headers}\r\n`]), 400, 'Bad request\n');
    check(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 65537\r\n\r\n']), 413, 'Body too large\n');
    check(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n']), 501, 'Transfer-Encoding is unsupported\n');
    check(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n']), 417, 'Expect is unsupported\n');
    check(await request(['HEAD / HTTP/1.1\r\nBad Header: x\r\n\r\n']), 400, 'Bad request\n', true);
    check(await request(['HEAD / HTTP/1.1\r\nHost:'], true), 400, 'Bad request\n', true);
    check(await request(['HEAD / HTTP/1.1\r\nX-Large: ' + 'x'.repeat(8192)]), 431, 'Headers too large\n', true);
    check(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\na'], true), 400, 'Bad request\n');
    assert.equal(await request(['POST /api/items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\na']), '');
    check(await get('/health'), 200, 'ok\n');
    console.log('HTTP API cases passed: async routes, CRUD, HEAD/OPTIONS, UTF-8, fragmented bodies, framing rejection, deadlines and recovery');
  } finally { server.kill(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
