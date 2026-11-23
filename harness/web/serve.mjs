// Serves harness/web/dist on this machine, for trying the page: nothing else, and to nobody else.
//
//   node harness/web/serve.mjs [port]      then open http://localhost:8000
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";

const dist = fileURLToPath(new URL("dist/", import.meta.url));
const port = Number(process.argv[2] ?? 8000);
const TYPES = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8", ".wasm": "application/wasm", ".svg": "image/svg+xml", ".txt": "text/plain; charset=utf-8",
  ".gz": "application/gzip",
};

createServer(async (req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, "http://localhost").pathname)).replace(/^[/\\]+/, "");
  const file = join(dist, path === "" ? "index.html" : path);
  try {
    if (!file.startsWith(dist)) throw new Error("outside dist");
    const bytes = await readFile(file);
    // The manifest says which wasm and data are current: it is never cached. They are named by hash.
    res.writeHead(200, { "Content-Type": TYPES[extname(file)] ?? "application/octet-stream", "Cache-Control": "no-cache" });
    res.end(bytes);
  } catch {
    res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
    res.end("not found\n");
  }
}).listen(port, "127.0.0.1", () => console.log(`harness/web/dist at http://localhost:${port}`));
