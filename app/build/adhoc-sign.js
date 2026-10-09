// electron-builder afterPack hook: ad-hoc sign the final universal app (Apple Silicon refuses
// unsigned apps). Runs before the dmg/zip are created. Without an Apple Developer ID the app
// is not notarized: first launch on another Mac is right-click → Open.
const { execFileSync } = require('node:child_process');
const path = require('node:path');

exports.default = async function adhocSign(context) {
  // Only the merged universal app; the per-arch temp copies are merged afterwards.
  if (!context.appOutDir.endsWith('mac-universal')) return;
  const app = path.join(context.appOutDir, `${context.packager.appInfo.productFilename}.app`);
  execFileSync('codesign', ['--force', '--deep', '--sign', '-', '--timestamp=none', app], { stdio: 'inherit' });
  execFileSync('codesign', ['--verify', '--deep', '--strict', app], { stdio: 'inherit' });
  console.log(`  • ad-hoc signed ${app}`);
};
