// Minimal bridge: file paths for <input type=file> (used to load mic calibration files).
const { contextBridge, ipcRenderer, webUtils } = require('electron');

contextBridge.exposeInMainWorld('vt', {
  pathForFile: (file) => webUtils.getPathForFile(file),
  // Edit › Undo / Redo from the app menu ('undo' | 'redo').
  onHistory: (cb) => ipcRenderer.on('vt-history', (_e, kind) => cb(kind)),
});
