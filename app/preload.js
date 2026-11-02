// Minimal bridge: file paths for <input type=file> (used to load mic calibration files).
const { contextBridge, ipcRenderer, webUtils } = require('electron');

contextBridge.exposeInMainWorld('vt', {
  pathForFile: (file) => webUtils.getPathForFile(file),
  // Edit › Undo / Redo from the app menu ('undo' | 'redo').
  onHistory: (cb) => ipcRenderer.on('vt-history', (_e, kind) => cb(kind)),
  // Session token for the engine's WebSocket handshake (ws://…/?token=…). NOT dead code: the
  // engine closes (code 4001) any connection without it, so removing this locks the UI out.
  // It is fetched over IPC rather than passed in the URL or argv so it never leaves this app.
  sessionToken: () => ipcRenderer.invoke('vt-token'),
});
