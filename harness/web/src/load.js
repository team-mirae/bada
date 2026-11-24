// Loads the engine for the page: the manifest (build.mjs), the wasm, the engine's data and the
// stock layouts' files, with progress.
import { Engine, HARNESS } from "./engine.js";

async function fetchBytes(url, gz, onBytes) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  let body = res.body.pipeThrough(new TransformStream({ transform(chunk, c) { onBytes(chunk.length); c.enqueue(chunk); } }));
  if (gz) body = body.pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(body).arrayBuffer());
}

/**
 * `onProgress(got, total)` in bytes. Returns the engine (one session, started with the harness's
 * name alone: harness.js feeds it the settings, layouts and dictionary), the manifest, and the
 * stock layouts' files by id.
 */
export async function loadEngine(onProgress = () => {}) {
  const manifest = await (await fetch("manifest.json", { cache: "no-cache" })).json();
  // The data files (the standard, §11), by the names of the engine's buffers. The manifest lists
  // none until the engine loads data (build.mjs: DATA); then they are fetched here, unzipped, and
  // passed to bada_data_new as named buffers.
  const names = Object.keys(manifest.data ?? {});
  const total = manifest.wasm.size + names.reduce((n, name) => n + manifest.data[name].size, 0);
  let got = 0;
  const tick = n => { got += n; onProgress(got, total); };
  const [wasm, ...files] = await Promise.all([
    fetchBytes(manifest.wasm.path, false, tick),
    ...names.map(name => fetchBytes(manifest.data[name].path, true, tick)),
  ]);
  const buffers = names.map((name, i) => ({ name, bytes: files[i] }));
  const engine = await Engine.load(wasm, { init: { harness: { ...HARNESS, version: manifest.version } }, buffers });
  const layouts = {};
  await Promise.all(manifest.layouts.map(async path => {
    const layout = await (await fetch(path)).json();
    layouts[layout.id] = layout;
  }));
  return { engine, manifest, layouts };
}
