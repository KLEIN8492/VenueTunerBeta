// Is whatever listens on the engine port really our engine? Used before attaching to an engine
// that is already running, so the shell never "attaches" to a foreign process or to an engine
// that does not accept this launch's session token (see engine/src/server/Server.h).
//
// A minimal WebSocket client handshake (Electron's main process has no stable WebSocket):
// upgrade to /?token=…, then read the first frame. Our engine sends its state as a text frame
// right after accepting; it refuses a wrong token with a close frame, code 4001.
//   'ours'    — the engine accepted the token
//   'refused' — a VenueTuner engine, but not started by this shell (other token)
//   'foreign' — something else answers on the port
//   'closed'  — nothing listens
const http = require('node:http');
const crypto = require('node:crypto');

const UNAUTHORIZED = 4001;

function probeEngine(port, token, timeoutMs = 1500) {
  return new Promise((resolve) => {
    let done = false;
    const finish = (verdict, socket) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      if (socket) socket.destroy();
      req.destroy();
      resolve(verdict);
    };
    const req = http.request({
      host: '127.0.0.1', port, path: `/?token=${encodeURIComponent(token)}`,
      headers: {
        Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13',
        'Sec-WebSocket-Key': crypto.randomBytes(16).toString('base64'),
      },
    });
    const timer = setTimeout(() => finish('foreign'), timeoutMs);
    req.on('error', (e) => finish(e.code === 'ECONNREFUSED' ? 'closed' : 'foreign'));
    req.on('response', (res) => { res.resume(); finish('foreign'); }); // plain HTTP: not a WebSocket server
    req.on('upgrade', (_res, socket, head) => {
      let buf = head;
      const look = () => {
        if (buf.length < 2) return;
        const opcode = buf[0] & 0x0f;
        if (opcode === 0x1) return finish('ours', socket); // state event: token accepted
        if (opcode === 0x8) {                               // close frame: [len][code hi][code lo]
          const code = buf.length >= 4 ? buf.readUInt16BE(2) : 0;
          return finish(code === UNAUTHORIZED ? 'refused' : 'foreign', socket);
        }
        finish('foreign', socket);
      };
      socket.on('data', (d) => { buf = Buffer.concat([buf, d]); look(); });
      socket.on('error', () => finish('foreign', socket));
      look();
    });
    req.end();
  });
}

module.exports = { probeEngine };
