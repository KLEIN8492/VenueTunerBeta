// Renders an HTML file to PDF with Electron's Chromium (A4, backgrounds on).
const { app, BrowserWindow } = require('electron');
const fs = require('fs');
const [input, output] = process.argv.slice(-2);
app.whenReady().then(async () => {
  const win = new BrowserWindow({ show: false, width: 1000, height: 1400 });
  await win.loadFile(input);
  const pdf = await win.webContents.printToPDF({ pageSize: 'A4', printBackground: true, margins: { marginType: 'custom', top: 0.5, bottom: 0.6, left: 0.5, right: 0.5 },
    displayHeaderFooter: true, headerTemplate: '<span></span>',
    footerTemplate: '<div style="width:100%;font:8px -apple-system,Helvetica;color:#888;text-align:center">VenueTuner manual · <span class="pageNumber"></span> / <span class="totalPages"></span></div>' });
  fs.writeFileSync(output, pdf);
  console.log('pdf written', output, pdf.length, 'bytes');
  app.quit();
});
