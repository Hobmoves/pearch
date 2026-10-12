// Renders assets/pearch.svg (and pearch-small.svg for tiny sizes) with
// headless Chromium and packs them into assets/pearch.ico + assets/pearch.png.
//
//   node tools/make_icon.mjs        (needs Playwright with Chromium)
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const assets = path.join(here, '..', 'assets');
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');

const sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256];
const browser = await chromium.launch();
const page = await browser.newPage();

async function render(svgFile, size) {
  const svg = fs.readFileSync(path.join(assets, svgFile), 'utf8');
  await page.setViewportSize({ width: size, height: size });
  await page.setContent(`<html><body style="margin:0;background:transparent">
    <img src="data:image/svg+xml;base64,${Buffer.from(svg).toString('base64')}" width="${size}" height="${size}"
         style="display:block"></body></html>`);
  await page.waitForLoadState('load');
  return page.screenshot({ omitBackground: true, clip: { x: 0, y: 0, width: size, height: size } });
}

const pngs = [];
for (const size of sizes) pngs.push(await render(size <= 32 ? 'pearch-small.svg' : 'pearch.svg', size));
fs.writeFileSync(path.join(assets, 'pearch.png'), await render('pearch.svg', 512));
await browser.close();

// ICO: 6-byte header, a 16-byte directory entry per image, then PNG data.
const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0);
header.writeUInt16LE(1, 2);
header.writeUInt16LE(sizes.length, 4);
let offset = 6 + 16 * sizes.length;
const entries = sizes.map((size, i) => {
  const e = Buffer.alloc(16);
  e.writeUInt8(size >= 256 ? 0 : size, 0);
  e.writeUInt8(size >= 256 ? 0 : size, 1);
  e.writeUInt8(0, 2);
  e.writeUInt8(0, 3);
  e.writeUInt16LE(1, 4);
  e.writeUInt16LE(32, 6);
  e.writeUInt32LE(pngs[i].length, 8);
  e.writeUInt32LE(offset, 12);
  offset += pngs[i].length;
  return e;
});
fs.writeFileSync(path.join(assets, 'pearch.ico'), Buffer.concat([header, ...entries, ...pngs]));
console.log('wrote assets/pearch.ico (' + sizes.join(', ') + ') and assets/pearch.png');
