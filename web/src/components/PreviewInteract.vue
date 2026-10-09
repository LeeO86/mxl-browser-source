<script setup>
// Live preview and the "Interact" mode (SPEC.md §7). Pictures come from the
// /api/v1/interact WebSocket; while it is down, /api/v1/preview.jpg is polled
// at 1 fps. While this session controls the page, pointer, wheel, keyboard,
// text and IME input on the preview are forwarded to the page. Back, forward
// and reload act on the page's own history.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { api, live } from "../api.js";
import { BUTTON_BITS, BUTTONS, InteractClient, modifiersOf, wheelPixels } from "../interact.js";

const canvas = ref(null);
const stage = ref(null);
const input = ref(null); // hidden textarea that receives keys, text and IME

const socketOpen = ref(false);
const istate = ref({ enabled: false, controller: "none", expires_in_s: 0 });
const interactOn = ref(false); // the toggle; the server's state overrides it
const controlling = computed(() => !!istate.value.enabled && istate.value.controller === "self");
const remaining = ref(0);
const rtt = ref(null);
const fps = ref(0);
const cursor = ref("default");
const keyboard = ref(false); // textarea focused
const serverError = ref("");
const hasFrame = ref(false);
const renderSize = ref(null); // {width, height} from the state message
const page = computed(() => live.page || {});

const aspect = computed(() => {
  const f = renderSize.value || live.info?.format;
  return f?.width && f?.height ? f.width / f.height : 16 / 9;
});

// ---- socket --------------------------------------------------------------

let errorTimer = 0;
const client = new InteractClient({
  open(isOpen) {
    socketOpen.value = isOpen;
    if (!isOpen) {
      istate.value = { enabled: false, controller: "none", expires_in_s: 0 };
      interactOn.value = false;
      rtt.value = null;
    }
  },
  frame(grain, blob) {
    showFrame(blob);
  },
  state(msg) {
    if (msg.interact) {
      istate.value = msg.interact;
      interactOn.value = !!msg.interact.enabled;
      setExpiry(msg.interact.expires_in_s);
    }
    if (msg.render?.width && msg.render?.height) renderSize.value = msg.render;
  },
  ack(ms) {
    rtt.value = Math.round(ms);
  },
  cursor(name) {
    // Only plain CSS cursor names; never url(...) from the page.
    cursor.value = /^[a-z-]+$/.test(name || "") ? name : "default";
  },
  error(msg) {
    showError(`${msg.code}: ${msg.message}`);
  },
});

function showError(text) {
  serverError.value = text;
  clearTimeout(errorTimer);
  errorTimer = setTimeout(() => (serverError.value = ""), 4000);
}

/** back, forward or reload (POST /api/v1/source/<action>). */
function pageAction(action) {
  api.post(`/api/v1/source/${action}`).catch((e) => showError(e.message));
}

function setInteract(enable, take = false) {
  interactOn.value = enable;
  client.send({ type: "interact", enable, take });
  if (!enable) input.value?.blur();
}

watch(controlling, (on) => {
  if (!on) {
    input.value?.blur();
    cursor.value = "default";
  }
});

// ---- remaining control time ---------------------------------------------
// The server sends expires_in_s on change only; count down locally and
// restart the count with each input we send (the timeout is "without input").

let expiresAt = 0;
let timeoutS = 0;
function setExpiry(s) {
  if (typeof s !== "number") return;
  timeoutS = Math.max(timeoutS, s);
  expiresAt = performance.now() + s * 1000;
  remaining.value = Math.max(0, Math.ceil(s));
}
function touch() {
  if (!timeoutS) return;
  expiresAt = performance.now() + timeoutS * 1000;
  remaining.value = timeoutS;
}

// ---- drawing -------------------------------------------------------------

let frame = null; // ImageBitmap on screen
let pic = { x: 0, y: 0, w: 1, h: 1 }; // drawn picture, as fractions of the canvas
let frames = 0;
let decoding = false;
let nextBlob = null;

async function showFrame(blob) {
  frames++;
  if (decoding) {
    nextBlob = blob; // keep only the newest picture
    return;
  }
  decoding = true;
  for (let b = blob; b; b = nextBlob) {
    nextBlob = null;
    try {
      const bmp = await createImageBitmap(b);
      frame?.close();
      frame = bmp;
      hasFrame.value = true;
      draw();
    } catch {
      /* broken JPEG: wait for the next one */
    }
  }
  decoding = false;
}

function draw() {
  const c = canvas.value;
  if (!c) return;
  const a = frame ? frame.width / frame.height : aspect.value;
  let w = c.width;
  let h = Math.round(w / a);
  if (h > c.height) {
    h = c.height;
    w = Math.round(h * a);
  }
  const x = Math.floor((c.width - w) / 2);
  const y = Math.floor((c.height - h) / 2);
  pic = { x: x / c.width, y: y / c.height, w: w / c.width, h: h / c.height };
  const ctx = c.getContext("2d");
  ctx.clearRect(0, 0, c.width, c.height);
  if (frame) {
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(frame, x, y, w, h);
  }
}

const resizer = new ResizeObserver(() => {
  const c = canvas.value;
  if (!c) return;
  const dpr = window.devicePixelRatio || 1;
  c.width = Math.max(1, Math.round(c.clientWidth * dpr));
  c.height = Math.max(1, Math.round(c.clientHeight * dpr));
  draw();
});

// ---- 1 s ticker: fps, countdown, fallback polling ------------------------

let polling = false;
async function poll() {
  if (polling) return;
  polling = true;
  try {
    await showFrame(await api.blob("/api/v1/preview.jpg"));
  } catch {
    /* no picture yet */
  }
  polling = false;
}

let ticker = 0;
function tick() {
  fps.value = frames;
  frames = 0;
  if (expiresAt) remaining.value = Math.max(0, Math.ceil((expiresAt - performance.now()) / 1000));
  if (!socketOpen.value) poll();
}

// ---- pointer and wheel ---------------------------------------------------

const round = (v) => Math.round(Math.min(1, Math.max(0, v)) * 1e5) / 1e5;

/** Event position as 0…1 of the drawn picture (letterbox bars excluded). */
function pos(e) {
  const r = canvas.value.getBoundingClientRect();
  const x = ((e.clientX - r.left) / r.width - pic.x) / pic.w;
  const y = ((e.clientY - r.top) / r.height - pic.y) / pic.h;
  return { x: round(x), y: round(y), inside: x >= 0 && x <= 1 && y >= 0 && y <= 1 };
}

let pendingMove = null;
let pendingWheel = null;
let raf = 0;
let last = { x: 0, y: 0 };

function flush() {
  if (raf) cancelAnimationFrame(raf);
  raf = 0;
  if (pendingMove) client.send({ type: "pointer", action: "move", ...pendingMove });
  if (pendingWheel) client.send({ type: "wheel", ...pendingWheel });
  if (pendingMove || pendingWheel) touch();
  pendingMove = pendingWheel = null;
}
const schedule = () => {
  if (!raf) raf = requestAnimationFrame(flush);
};

const pressed = new Set();
let lastDown = { t: 0, cx: 0, cy: 0, button: "", clicks: 0 };

// PointerEvent.detail is 0 in Chromium, so double and triple clicks are counted here.
function clickCount(e, button) {
  const t = performance.now();
  const same =
    button === lastDown.button && t - lastDown.t < 500 &&
    Math.abs(e.clientX - lastDown.cx) < 5 && Math.abs(e.clientY - lastDown.cy) < 5;
  const clicks = same ? Math.min(3, lastDown.clicks + 1) : 1;
  lastDown = { t, cx: e.clientX, cy: e.clientY, button, clicks };
  return clicks;
}

function onButton(e, action) {
  if (!controlling.value) return;
  e.preventDefault();
  const button = BUTTONS[e.button];
  if (!button) return; // back/forward buttons are swallowed, not sent
  const p = pos(e);
  if (action === "down") {
    if (!p.inside) return;
    focusKeyboard(e);
    try {
      canvas.value.setPointerCapture(e.pointerId);
    } catch {
      /* capture is optional */
    }
    pressed.add(button);
  } else if (!pressed.delete(button)) {
    return; // no matching down was sent
  }
  flush();
  last = { x: p.x, y: p.y };
  const clicks = action === "down" ? clickCount(e, button) : lastDown.button === button ? lastDown.clicks : 1;
  client.sendTracked({ type: "pointer", action, x: p.x, y: p.y, button, clicks, modifiers: modifiersOf(e) });
  touch();
}

function onPointerMove(e) {
  if (!controlling.value) return;
  // A second button pressed or released while one is held arrives as a move.
  if (e.button >= 0) return onButton(e, e.buttons & BUTTON_BITS[e.button] ? "down" : "up");
  const p = pos(e);
  last = { x: p.x, y: p.y };
  pendingMove = { x: p.x, y: p.y, modifiers: modifiersOf(e) };
  schedule();
}

function onPointerLeave() {
  if (!controlling.value) return;
  pendingMove = null;
  flush();
  pressed.clear();
  client.send({ type: "pointer", action: "leave", x: last.x, y: last.y });
}

function onWheel(e) {
  if (!controlling.value) return;
  e.preventDefault(); // also keeps Ctrl+wheel from zooming this UI
  const d = wheelPixels(e, canvas.value.clientHeight);
  const p = pos(e);
  pendingWheel = {
    x: p.x,
    y: p.y,
    dx: (pendingWheel?.dx || 0) + d.dx,
    dy: (pendingWheel?.dy || 0) + d.dy,
    modifiers: modifiersOf(e),
  };
  schedule();
}

function onMouseDown(e) {
  // Keeps the focus in the hidden textarea (pointerdown alone does not).
  if (controlling.value) e.preventDefault();
}

function onContextMenu(e) {
  if (controlling.value) e.preventDefault();
}

// ---- keyboard, text, IME -------------------------------------------------

/** Moves the hidden textarea to the click (the IME window opens there) and focuses it. */
function focusKeyboard(e) {
  const ta = input.value;
  const r = stage.value.getBoundingClientRect();
  ta.style.left = `${Math.round(e.clientX - r.left)}px`;
  ta.style.top = `${Math.round(e.clientY - r.top)}px`;
  ta.focus({ preventScroll: true });
}

let composing = false;
let justCommitted = false;

const imeKey = (e) => e.isComposing || e.keyCode === 229;
// Ctrl+V / Shift+Insert: let the operator's browser paste and send the text
// (the page's clipboard in the container is empty, SPEC §7.1).
const isPaste = (e) =>
  ((e.ctrlKey || e.metaKey) && !e.altKey && (e.code === "KeyV" || e.key.toLowerCase() === "v")) ||
  (e.shiftKey && e.key === "Insert");

function onKeyDown(e) {
  // Dead keys and IME keys compose in the textarea; the result arrives
  // as a key with the composed `key`, as text or as an IME commit.
  if (!controlling.value || imeKey(e) || e.key === "Dead" || isPaste(e)) return;
  e.preventDefault(); // everything else goes to the page, Tab and Ctrl+A included
  client.sendTracked({
    type: "key", action: "down", code: e.code, key: e.key, modifiers: modifiersOf(e), repeat: e.repeat,
  });
  touch();
}

function onKeyUp(e) {
  if (!controlling.value || imeKey(e) || e.key === "Dead" || isPaste(e)) return;
  e.preventDefault();
  client.send({ type: "key", action: "up", code: e.code, key: e.key, modifiers: modifiersOf(e) });
}

function onPaste(e) {
  if (!controlling.value) return;
  e.preventDefault();
  const text = e.clipboardData?.getData("text/plain") || "";
  if (text) client.sendText(text);
  touch();
}

// Printable keys never get here (their keydown is prevented); this catches
// text from other sources such as the emoji picker or dictation.
function onInput(e) {
  if (composing || e.isComposing) return;
  if (controlling.value && !justCommitted && e.data) {
    client.sendText(e.data);
    touch();
  }
  input.value.value = "";
}

function onCompositionStart() {
  composing = true;
}

function onCompositionUpdate(e) {
  if (!controlling.value) return;
  const text = e.data || "";
  client.send({ type: "ime", action: "composition", text, selection: [text.length, text.length] });
  touch();
}

function onCompositionEnd(e) {
  composing = false;
  if (controlling.value) {
    const text = e.data || "";
    client.send(text ? { type: "ime", action: "commit", text } : { type: "ime", action: "cancel" });
    justCommitted = true; // some browsers fire one more `input` with the same text
    setTimeout(() => (justCommitted = false));
    touch();
  }
  input.value.value = "";
}

function onFocus() {
  keyboard.value = true;
  if (controlling.value) client.send({ type: "focus", focused: true });
}

function onBlur() {
  keyboard.value = false;
  if (!controlling.value) return;
  if (composing) client.send({ type: "ime", action: "cancel" });
  composing = false;
  client.send({ type: "focus", focused: false });
}

// ---- status line ---------------------------------------------------------

const statusLine = computed(() => {
  if (!socketOpen.value) return "Socket down · polling preview.jpg at 1 fps";
  const parts = [`Socket live · ${fps.value} fps`];
  if (rtt.value !== null) parts.push(`input RTT ${rtt.value} ms`);
  if (controlling.value) parts.push(keyboard.value ? "keyboard captured" : "click the preview for keys");
  return parts.join(" · ");
});

onMounted(() => {
  resizer.observe(canvas.value);
  client.connect();
  ticker = setInterval(tick, 1000);
  tick();
});

onUnmounted(() => {
  client.close();
  resizer.disconnect();
  clearInterval(ticker);
  clearTimeout(errorTimer);
  if (raf) cancelAnimationFrame(raf);
  frame?.close();
  frame = null;
});
</script>

<template>
  <div class="preview-bar">
    <button class="btn small secondary" type="button" title="Back" :disabled="!page.can_go_back"
            @click="pageAction('back')">&larr; Back</button>
    <button class="btn small secondary" type="button" title="Forward" :disabled="!page.can_go_forward"
            @click="pageAction('forward')">Forward &rarr;</button>
    <button class="btn small secondary" type="button" title="Reload" @click="pageAction('reload')">&#x21bb; Reload</button>
    <label class="inline">
      <input type="checkbox" :checked="interactOn" :disabled="!socketOpen"
             @change="setInteract($event.target.checked)" />
      Interact
    </label>
    <template v-if="istate.controller === 'other'">
      <span class="muted">Another session controls the page.</span>
      <button class="btn small danger" @click="setInteract(true, true)">Take control</button>
    </template>
    <span class="spacer"></span>
    <span class="statusline" :class="{ err: serverError }">{{ serverError || statusLine }}</span>
  </div>

  <div ref="stage" class="stage" :class="{ onair: controlling }" :style="{ aspectRatio: aspect }">
    <canvas ref="canvas" role="img" aria-label="Page preview"
            :style="{ cursor: controlling ? cursor : 'default' }"
            @pointerdown="onButton($event, 'down')" @pointerup="onButton($event, 'up')"
            @pointermove="onPointerMove" @pointerleave="onPointerLeave" @pointercancel="onPointerLeave"
            @mousedown="onMouseDown" @wheel="onWheel" @contextmenu="onContextMenu"></canvas>
    <textarea ref="input" class="ime-input" aria-label="Keyboard input to the page" tabindex="-1"
              autocomplete="off" autocapitalize="off" spellcheck="false"
              @keydown="onKeyDown" @keyup="onKeyUp" @paste="onPaste" @input="onInput"
              @compositionstart="onCompositionStart" @compositionupdate="onCompositionUpdate"
              @compositionend="onCompositionEnd" @focus="onFocus" @blur="onBlur"></textarea>
    <div v-if="controlling" class="onair-badge">INTERACT — ON AIR · {{ remaining }} s</div>
    <div v-if="!hasFrame" class="stage-empty">No preview yet</div>
  </div>

  <p v-if="interactOn" class="note">
    Click the preview to send mouse and keys to the page; click outside it to release the keyboard.
    Shortcuts your own browser reserves (Ctrl+W, Ctrl+T, Ctrl+N, …) cannot be captured.
    Ctrl+V pastes your local clipboard as text.
  </p>
</template>
