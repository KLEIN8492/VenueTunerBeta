// Minimal bridge: file paths for <input type=file> (used to load mic calibration files).
const { contextBridge, webUtils } = require('electron');

contextBridge.exposeInMainWorld('vt', {
  pathForFile: (file) => webUtils.getPathForFile(file),
});
