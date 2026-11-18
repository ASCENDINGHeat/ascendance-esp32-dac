import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(__dirname, '..');
const distDir = path.join(rootDir, 'dist');
const publicDir = path.join(rootDir, 'public');

fs.rmSync(distDir, { recursive: true, force: true });
fs.mkdirSync(distDir, { recursive: true });

if (fs.existsSync(publicDir)) {
  for (const name of fs.readdirSync(publicDir)) {
    fs.copyFileSync(path.join(publicDir, name), path.join(distDir, name));
  }
}

const result = await esbuild.build({
  entryPoints: [path.join(rootDir, 'src/main.jsx')],
  bundle: true,
  minify: true,
  write: false,
  format: 'iife',
  target: ['es2020'],
  jsx: 'automatic',
  define: {
    'process.env.NODE_ENV': '"production"',
  },
  outdir: distDir,
});

let jsCode = '';
let cssCode = '';
for (const file of result.outputFiles) {
  if (file.path.endsWith('.js')) jsCode += file.text;
  else if (file.path.endsWith('.css')) cssCode += file.text;
}

const template = fs.readFileSync(path.join(rootDir, 'index.html'), 'utf-8');
const withoutDevScript = template.replace(
  /<script\s+type="module"\s+src="\/src\/main\.jsx"><\/script>/i,
  ''
);

const safeJs = jsCode.replace(/<\/script>/gi, '<\\/script>');
const bundledHtml = withoutDevScript
  .replace('</head>', () => `    <style>${cssCode}</style>\n</head>`)
  .replace('</body>', () => `    <script>${safeJs}</script>\n</body>`);

fs.writeFileSync(path.join(distDir, 'index.html'), bundledHtml, 'utf-8');
console.log(
  `Built single-file WebHID tuner -> dist/index.html (${(Buffer.byteLength(bundledHtml) / 1024).toFixed(1)} KB)`
);
