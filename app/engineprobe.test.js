// node --test: the attach handshake against a real engine and against foreign listeners.
const { test } = require('node:test');
const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { probeEngine } = require('./engineprobe');

const ENGINE = path.join(__dirname, '..', 'engine', 'build', process.platform === 'win32' ? 'VenueTunerEngine.exe' : 'VenueTunerEngine');

function startEngine(port, token) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vt-probe-'));
  const child = spawn(ENGINE, ['--port', String(port), '--data-dir', dir, '--token-stdin', '--quit-on-stdin-eof'], { stdio: ['pipe', 'pipe', 'pipe'] });
  child.stdin.write(`${token}\n`);
  const ready = new Promise((resolve, reject) => {
    child.stdout.on('data', (d) => { if (String(d).includes('VENUETUNER_READY')) resolve(); });
    child.on('exit', (code) => reject(new Error(`engine exited (${code})`)));
  });
  const stop = () => new Promise((resolve) => { child.once('exit', resolve); child.stdin.end(); });
  return { ready, stop };
}

test('our engine with the right token: attach', { skip: !fs.existsSync(ENGINE) && 'engine not built' }, async () => {
  const e = startEngine(47861, 'right-token');
  await e.ready;
  try {
    assert.strictEqual(await probeEngine(47861, 'right-token'), 'ours');
    assert.strictEqual(await probeEngine(47861, 'other-launch'), 'refused'); // another shell's engine
  } finally { await e.stop(); }
});

test('a foreign TCP listener is not an engine', async () => {
  const srv = net.createServer((s) => s.end('SSH-2.0-OpenSSH\r\n'));
  await new Promise((r) => srv.listen(47862, '127.0.0.1', r));
  try { assert.strictEqual(await probeEngine(47862, 'x', 800), 'foreign'); } finally { srv.close(); }
});

test('a plain HTTP server is not an engine', async () => {
  const srv = http.createServer((_q, res) => res.end('hello'));
  await new Promise((r) => srv.listen(47863, '127.0.0.1', r));
  try { assert.strictEqual(await probeEngine(47863, 'x', 800), 'foreign'); } finally { srv.close(); }
});

test('nothing listening', async () => {
  assert.strictEqual(await probeEngine(47864, 'x', 800), 'closed');
});
