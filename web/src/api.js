// REST client, API token and the shared live state from /api/v1/events
// (SPEC.md §10, docs/api.md).
import { reactive } from "vue";

// ---- token (BROWSER_API_TOKEN) -------------------------------------------

const TOKEN_KEY = "mxl-browser-source.token";
let token = "";
try {
  token = sessionStorage.getItem(TOKEN_KEY) || "";
} catch {
  /* storage blocked (sandboxed iframe): ask again after a reload */
}

// App.vue shows the token dialog while `auth.asking` is true. Requests that
// get a 401 at the same time all wait for that one dialog.
export const auth = reactive({ asking: false });
let tokenWaiters = [];

function askToken() {
  auth.asking = true;
  return new Promise((resolve) => tokenWaiters.push(resolve));
}

/** Called by the token dialog; an empty string means "cancel". */
export function submitToken(value) {
  if (value) {
    token = value.trim();
    try {
      sessionStorage.setItem(TOKEN_KEY, token);
    } catch {
      /* kept in memory only */
    }
  }
  auth.asking = false;
  const waiters = tokenWaiters;
  tokenWaiters = [];
  waiters.forEach((resolve) => resolve());
}

export const tokenSet = () => token !== "";

/** WebSocket URL on the page's own host; the token goes into the query. */
export function wsUrl(path) {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const query = token ? `?token=${encodeURIComponent(token)}` : "";
  return `${proto}//${location.host}${path}${query}`;
}

// ---- REST ----------------------------------------------------------------

async function request(path, { method = "GET", body, raw = false } = {}, retried = false) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (token) headers.Authorization = `Bearer ${token}`;
  const resp = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: "no-store",
  });
  if (resp.status === 401 && !retried) {
    await askToken();
    return request(path, { method, body, raw }, true);
  }
  if (raw) {
    if (!resp.ok) throw new Error(`${resp.status} ${resp.statusText}`);
    return resp;
  }
  const text = await resp.text();
  let data = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    data = text;
  }
  if (!resp.ok) {
    const reason = data && typeof data === "object" ? data.error : String(data || "").slice(0, 200);
    throw new Error(`${resp.status}: ${reason || resp.statusText}`);
  }
  return data;
}

export const api = {
  get: (path) => request(path),
  post: (path, body) => request(path, { method: "POST", body }),
  put: (path, body) => request(path, { method: "PUT", body }),
  patch: (path, body) => request(path, { method: "PATCH", body }),
  del: (path) => request(path, { method: "DELETE" }),
  blob: async (path) => (await request(path, { raw: true })).blob(),
};

/**
 * Runs an API action and reports into a `{kind, text}` message ref:
 * `ok` text on success, the error otherwise. Returns the result or undefined.
 */
export async function act(msg, fn, okText = "") {
  try {
    const result = await fn();
    msg.value = { kind: "ok", text: okText };
    return result;
  } catch (e) {
    msg.value = { kind: "err", text: e.message || String(e) };
    return undefined;
  }
}

// ---- live state from /api/v1/events --------------------------------------

const LOG_MAX = 200;

export const live = reactive({
  info: null, // GET /api/v1/info
  status: null, // newest {"type":"status"} (4 Hz)
  page: null, // status.page merged with {"type":"page"} events
  connected: false, // events socket open
  console: [], // {time, level, message, source, line}
  log: [], // dialogs and page events: {time, kind, text}
});

function push(list, entry) {
  list.push(entry);
  if (list.length > LOG_MAX) list.splice(0, list.length - LOG_MAX);
}

const now = () => new Date().toISOString();

function onEvent(msg) {
  switch (msg.type) {
    case "status":
      live.status = msg;
      live.page = msg.page || live.page;
      break;
    case "page":
      live.page = { ...(live.page || {}), state: msg.state, url: msg.url, title: msg.title };
      break;
    case "console":
      push(live.console, { time: msg.time || now(), level: msg.level, message: msg.message, source: msg.source, line: msg.line });
      break;
    case "dialog":
      push(live.log, { time: now(), kind: "dialog", text: `${msg.kind}: "${msg.message ?? ""}" → ${msg.result}` });
      break;
    case "event":
      push(live.log, { time: now(), kind: "event", text: JSON.stringify(msg.data) });
      break;
  }
}

async function loadConsole() {
  try {
    const res = await api.get("/api/v1/console");
    live.console = (res?.messages || []).slice(-LOG_MAX);
  } catch {
    /* keep what we have */
  }
}

let started = false;

/** Opens /api/v1/events once and keeps it open, reconnecting with backoff. */
export function startEvents() {
  if (started) return;
  started = true;
  let retry = 0;
  const connect = () => {
    const ws = new WebSocket(wsUrl("/api/v1/events"));
    ws.onopen = () => {
      retry = 0;
      live.connected = true;
      loadConsole();
    };
    ws.onmessage = (ev) => {
      try {
        onEvent(JSON.parse(ev.data));
      } catch {
        /* ignore malformed frames */
      }
    };
    ws.onclose = () => {
      live.connected = false;
      setTimeout(connect, Math.min(10000, 500 * 2 ** retry++));
    };
    ws.onerror = () => ws.close();
  };
  connect();
}

// ---- small formatting helpers --------------------------------------------

export const fmt = (v, digits = 1) =>
  typeof v === "number" && Number.isFinite(v) ? v.toFixed(digits) : "–";

export const clock = (iso) => {
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? "" : d.toLocaleTimeString([], { hour12: false });
};
