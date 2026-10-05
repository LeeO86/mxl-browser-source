// Client for the /api/v1/interact WebSocket (SPEC.md §7.2, docs/api.md):
// JSON text frames both ways, binary frames are preview pictures
// (8-byte little-endian grain index, then a JPEG).
import { wsUrl } from "./api.js";

const LINE_PX = 40; // wheel deltaMode 1 (lines) → pixels
const ACK_TIMEOUT_MS = 10000; // forget a seq that was never acknowledged

/** Modifier list of a keyboard, mouse or wheel event. */
export function modifiersOf(e) {
  // AltGr reports Ctrl+Alt on Windows; the character it produced is in
  // `key`, so drop both or the page would see a shortcut instead of text.
  const altGr = e.getModifierState?.("AltGraph");
  const m = [];
  if (e.shiftKey) m.push("shift");
  if (e.ctrlKey && !altGr) m.push("ctrl");
  if (e.altKey && !altGr) m.push("alt");
  if (e.metaKey) m.push("meta");
  if (e.getModifierState?.("CapsLock")) m.push("capslock");
  return m;
}

/** Wheel deltas in pixels (DOM sign: positive dy scrolls the content down). */
export function wheelPixels(e, pageHeightPx) {
  const f = e.deltaMode === 1 ? LINE_PX : e.deltaMode === 2 ? pageHeightPx : 1;
  return { dx: e.deltaX * f, dy: e.deltaY * f };
}

/** DOM MouseEvent.button → protocol name; other buttons are not sent. */
export const BUTTONS = ["left", "middle", "right"];
/** Bit of each button in MouseEvent.buttons (left 1, middle 4, right 2). */
export const BUTTON_BITS = [1, 4, 2];

export class InteractClient {
  /**
   * handlers: open(bool), frame(grain, blob), state(msg),
   * ack(rttMs, grain), cursor(name), error(msg)
   */
  constructor(handlers) {
    this.h = handlers;
    this.ws = null;
    this.seq = 0;
    this.sent = new Map(); // seq → performance.now() at send
    this.retry = 0;
    this.timer = 0;
    this.closed = false;
  }

  connect() {
    if (this.closed) return;
    const ws = new WebSocket(wsUrl("/api/v1/interact"));
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      this.retry = 0;
      this.send({ type: "hello", client: "ui", protocol: 1 });
      this.h.open?.(true);
    };
    ws.onmessage = (ev) => this.onMessage(ev.data);
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.sent.clear();
      this.h.open?.(false);
      if (!this.closed) this.timer = setTimeout(() => this.connect(), Math.min(5000, 500 * 2 ** this.retry++));
    };
    ws.onerror = () => ws.close();
    this.ws = ws;
  }

  close() {
    this.closed = true;
    clearTimeout(this.timer);
    const ws = this.ws;
    this.ws = null;
    ws?.close();
  }

  get isOpen() {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  send(msg) {
    if (!this.isOpen) return false;
    this.ws.send(JSON.stringify(msg));
    return true;
  }

  /** Sends with a new `seq`; the matching `ack` reports the round trip. */
  sendTracked(msg) {
    const seq = ++this.seq;
    const t = performance.now();
    for (const [s, t0] of this.sent) {
      if (t - t0 < ACK_TIMEOUT_MS) break; // Map keeps insertion order
      this.sent.delete(s);
    }
    if (!this.send({ ...msg, seq })) return false;
    this.sent.set(seq, t);
    return true;
  }

  /** Text in chunks of at most 4096 characters (schema maxLength). */
  sendText(text) {
    const chars = Array.from(text);
    for (let i = 0; i < chars.length; i += 4096) {
      this.send({ type: "text", text: chars.slice(i, i + 4096).join("") });
    }
  }

  onMessage(data) {
    if (data instanceof ArrayBuffer) {
      if (data.byteLength <= 8) return;
      const v = new DataView(data);
      const grain = v.getUint32(0, true) + v.getUint32(4, true) * 2 ** 32;
      this.h.frame?.(grain, new Blob([new Uint8Array(data, 8)], { type: "image/jpeg" }));
      return;
    }
    let msg;
    try {
      msg = JSON.parse(data);
    } catch {
      return;
    }
    switch (msg.type) {
      case "state":
        this.h.state?.(msg);
        break;
      case "ack": {
        const t0 = this.sent.get(msg.seq);
        if (t0 === undefined) break;
        this.sent.delete(msg.seq);
        this.h.ack?.(performance.now() - t0, msg.grain);
        break;
      }
      case "cursor":
        this.h.cursor?.(msg.cursor);
        break;
      case "error":
        this.h.error?.(msg);
        break;
      // `dialog` and `console` also arrive on /api/v1/events; the UI logs
      // them from there only, so they are not shown twice.
    }
  }
}
