// The host over HTTP: what `bada settings` serves (settings/README.md, "The host's interface").
// The run's token comes in the page's address (`?t=`) and goes back with every request.

export function httpHost(base = location.origin, token = new URLSearchParams(location.search).get("t") ?? "") {
  async function call(method, path, body) {
    const response = await fetch(base + path, {
      method, cache: "no-store",
      headers: { "X-Bada-Token": token, ...(body === undefined ? {} : { "Content-Type": "application/json" }) },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (!response.ok) throw new Error(`${path} answered ${response.status}`);
    return response.json();
  }
  const get = path => call("GET", path);
  const post = (path, body = {}) => call("POST", path, body);

  // The host stops when the page stops saying it is open, and at once when the page is closed.
  const alive = setInterval(() => post("/api/alive").catch(() => {}), 15000);
  addEventListener("pagehide", () => {
    clearInterval(alive);
    // fetch with keepalive carries the token's header, which sendBeacon can't.
    fetch(base + "/api/bye", { method: "POST", keepalive: true, headers: { "X-Bada-Token": token, "Content-Type": "application/json" }, body: "{}" }).catch(() => {});
  });

  return {
    info: () => get("/api/info"),
    describe: () => get("/api/describe"),
    stock: () => get("/api/stock"),
    files: () => get("/api/files"),
    check: what => post("/api/check", what),
    write: (name, text) => post("/api/write", { name, text }),
    remove: name => post("/api/remove", { name }),
    undo: name => post("/api/undo", { name }),
    state: () => get("/api/state"),
    reveal: () => post("/api/reveal"),
  };
}
