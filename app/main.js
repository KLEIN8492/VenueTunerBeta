// VenueTuner desktop shell: supervises the headless engine and hosts the windows.
//   Measurement ⌘1 · Live/Tuning ⌘2 · Settings ⌘,
const { app, BrowserWindow, Menu, dialog, net, protocol, screen, systemPreferences } = require('electron');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const nodeNet = require('node:net');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

const PORT = Number(process.env.VT_PORT || 47800);
// Packaged app: engine and UI live in Contents/Resources. Development: in the repo.
const ROOT = path.resolve(__dirname, '..');
const UI_DIST = app.isPackaged ? path.join(process.resourcesPath, 'ui') : path.join(ROOT, 'ui', 'dist');
const ENGINE = process.env.VT_ENGINE
  || (app.isPackaged ? path.join(process.resourcesPath, 'engine', 'VenueTunerEngine') : path.join(ROOT, 'engine', 'build', 'VenueTunerEngine'));
const DATA_DIR = process.env.VT_DATA_DIR || path.join(app.getPath('appData'), 'VenueTuner');

protocol.registerSchemesAsPrivileged([
  { scheme: 'app', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

// ------------------------------------------------------------------ engine supervision
let engine = null;
let quitting = false;
const restarts = [];

function portOpen() {
  return new Promise((resolve) => {
    const s = nodeNet.connect(PORT, '127.0.0.1');
    s.once('connect', () => { s.destroy(); resolve(true); });
    s.once('error', () => resolve(false));
  });
}

async function ensureEngine() {
  if (quitting || engine) return;
  if (await portOpen()) {
    // An engine is already running (e.g. the UI was restarted): attach, keep audio going.
    console.log(`[shell] attaching to running engine on :${PORT}`);
    setTimeout(ensureEngine, 2000);
    return;
  }
  const now = Date.now();
  while (restarts.length && now - restarts[0] > 60_000) restarts.shift();
  if (restarts.length >= 5) {
    dialog.showErrorBox('VenueTuner engine', 'The audio engine crashed 5 times in a minute and will not be restarted automatically.\nCheck the audio device, then relaunch the app.');
    return;
  }
  restarts.push(now);
  console.log(`[shell] starting engine ${ENGINE}`);
  engine = spawn(ENGINE, ['--port', String(PORT), '--data-dir', DATA_DIR], { stdio: ['ignore', 'pipe', 'pipe'] });
  engine.stdout.on('data', (d) => process.stdout.write(`[engine] ${d}`));
  engine.stderr.on('data', (d) => process.stderr.write(`[engine] ${d}`));
  engine.on('error', (e) => {
    dialog.showErrorBox('VenueTuner engine', `Could not start the engine:\n${e.message}` + (app.isPackaged ? '' : '\n\nBuild it first:  npm run build'));
  });
  engine.on('exit', (code, signal) => {
    console.log(`[shell] engine exited (code ${code}, signal ${signal})`);
    engine = null;
    if (!quitting) setTimeout(ensureEngine, 1000); // restart; it comes back with outputs muted
  });
}

function stopEngine() {
  return new Promise((resolve) => {
    if (!engine) return resolve();
    const child = engine;
    const timer = setTimeout(() => { child.kill('SIGKILL'); resolve(); }, 2000);
    child.once('exit', () => { clearTimeout(timer); resolve(); });
    child.kill('SIGTERM'); // engine autosaves on SIGTERM
  });
}

// ------------------------------------------------------------------ windows
const windows = {};
const PAGES = {
  measurement: { title: 'VenueTuner · Measurement', w: 1280, h: 820 },
  live: { title: 'VenueTuner · Live / Tuning', w: 1280, h: 620 },
  settings: { title: 'VenueTuner · Settings', w: 900, h: 760 },
};

// Window positions are remembered per window, if that place is still on a connected screen.
const boundsFile = () => path.join(app.getPath('userData'), 'windows.json');
let savedBounds = {};
try { savedBounds = JSON.parse(fs.readFileSync(boundsFile(), 'utf8')); } catch { savedBounds = {}; }
function onScreen(b) {
  return b && screen.getAllDisplays().some(({ workArea: a }) =>
    b.x < a.x + a.width - 50 && b.x + b.width > a.x + 50 && b.y >= a.y - 10 && b.y < a.y + a.height - 50);
}
let saveTimer = null;
function rememberBounds(name, win) {
  savedBounds[name] = win.getBounds();
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => { try { fs.writeFileSync(boundsFile(), JSON.stringify(savedBounds)); } catch { /* not critical */ } }, 500);
}

function openWindow(name, bounds) {
  if (onScreen(savedBounds[name])) bounds = savedBounds[name];
  if (windows[name] && !windows[name].isDestroyed()) {
    windows[name].show();
    windows[name].focus();
    return windows[name];
  }
  const p = PAGES[name];
  const win = new BrowserWindow({
    title: p.title,
    width: p.w, height: p.h, ...bounds,
    minWidth: 640, minHeight: 400,
    backgroundColor: '#161718',
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true },
  });
  win.loadURL(`app://ui/${name}.html?port=${PORT}`);
  win.on('page-title-updated', (e) => e.preventDefault());
  win.on('moved', () => rememberBounds(name, win));
  win.on('resized', () => rememberBounds(name, win));
  win.on('closed', () => { delete windows[name]; });
  windows[name] = win;
  return win;
}

function openMain() {
  const area = screen.getPrimaryDisplay().workArea;
  const topH = Math.round(area.height * 0.6);
  openWindow('measurement', { x: area.x, y: area.y, width: area.width, height: topH });
  openWindow('live', { x: area.x, y: area.y + topH, width: area.width, height: area.height - topH });
}

function buildMenu() {
  const isMac = process.platform === 'darwin';
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    ...(isMac ? [{
      label: app.name,
      submenu: [
        { role: 'about' }, { type: 'separator' },
        { label: 'Settings…', accelerator: 'Cmd+,', click: () => openWindow('settings') },
        { type: 'separator' }, { role: 'hide' }, { role: 'hideOthers' }, { type: 'separator' }, { role: 'quit' },
      ],
    }] : []),
    {
      // Undo/redo go to the page: text fields undo typing, everything else undoes tuning
      // changes in the engine (shared by all windows).
      label: 'Edit',
      submenu: [
        { label: 'Undo', accelerator: 'CmdOrCtrl+Z', click: (_, win) => win?.webContents.send('vt-history', 'undo') },
        { label: 'Redo', accelerator: 'Shift+CmdOrCtrl+Z', click: (_, win) => win?.webContents.send('vt-history', 'redo') },
        { type: 'separator' }, { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' },
      ],
    },
    {
      label: 'Window',
      submenu: [
        { label: 'Measurement', accelerator: 'CmdOrCtrl+1', click: () => openWindow('measurement') },
        { label: 'Live / Tuning', accelerator: 'CmdOrCtrl+2', click: () => openWindow('live') },
        ...(isMac ? [] : [{ label: 'Settings…', accelerator: 'Ctrl+,', click: () => openWindow('settings') }]),
        { type: 'separator' }, { role: 'minimize' }, { role: 'close' },
      ],
    },
    { label: 'View', submenu: [{ role: 'reload' }, { role: 'toggleDevTools' }, { type: 'separator' }, { role: 'togglefullscreen' }] },
  ]));
}

// ------------------------------------------------------------------ lifecycle
app.setName('VenueTuner');

app.whenReady().then(async () => {
  protocol.handle('app', (req) => {
    const { pathname } = new URL(req.url);
    const file = path.normalize(path.join(UI_DIST, decodeURIComponent(pathname)));
    if (!file.startsWith(UI_DIST)) return new Response('forbidden', { status: 403 });
    return net.fetch(pathToFileURL(file).toString());
  });
  buildMenu();
  if (process.platform === 'darwin') {
    // The engine's microphone access is granted through this app.
    await systemPreferences.askForMediaAccess('microphone').catch(() => false);
  }
  await ensureEngine();
  openMain();
});

app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) openMain(); });
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });

app.on('before-quit', async (e) => {
  if (quitting) return;
  e.preventDefault();
  quitting = true;
  await stopEngine();
  app.quit();
});
