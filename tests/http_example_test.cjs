// Exercise the example over real TCP, independently of the DMM implementation.
const { spawn } = require('node:child_process');
const net = require('node:net');
const assert = require('node:assert/strict');
const server = spawn(process.argv[2], ['0'], { windowsHide: true });
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
  assert.equal(Number(headers.match(/Content-Length: (\d+)/)[1]), Buffer.byteLength(body));
  assert.ok(headers.includes('Connection: close'));
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
    assert.ok(post.includes('Allow: GET, HEAD\r\n'));
    check(await request(['garbage\r\n\r\n']), 400, 'Bad request\n');
    check(await request(['GET / HTTP/9.0\r\n\r\n']), 400, 'Bad request\n');
    check(await request(['GET / HTTP/1.1\r\nHost:'], true), 400, 'Bad request\n');
    check(await request(['x'.repeat(8192)]), 431, 'Headers too large\n');
    assert.equal(await request(['GET / HTTP/1.1\r\n']), '');
    check(await request(['GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n']), 200, 'ok\n');
    console.log('13 HTTP cases passed, including fragmentation, timeout and recovery');
  } finally { server.kill(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
