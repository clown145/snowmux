import { copyFile, mkdir } from 'node:fs/promises';
import { build } from 'esbuild';

await mkdir('dist', { recursive: true });
await Promise.all([
  copyFile('index.html', 'dist/index.html'),
  copyFile('style.css', 'dist/style.css'),
  build({
    entryPoints: ['app.mjs'],
    bundle: true,
    format: 'esm',
    outfile: 'dist/app.js',
    legalComments: 'linked',
  }),
]);
await mkdir('dist/licenses', { recursive: true });
for (const [name, file] of [
  ['esptool-js', 'LICENSE'],
  ['atob-lite', 'LICENSE.md'],
  ['pako', 'LICENSE'],
  ['web-serial-polyfill', 'LICENSE'],
]) {
  await copyFile(`node_modules/${name}/${file}`, `dist/licenses/${name}.txt`);
}
