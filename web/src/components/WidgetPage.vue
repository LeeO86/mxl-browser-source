<script setup>
// Operator-screen widget (SPEC §10.6): /widget/interaction[?theme=dark|light|transparent] is the Source
// tab's preview with back, forward, reload and Interact (mouse, wheel, keys, text) without the app
// around it. Everything goes to this instance's own origin: REST, /api/v1/events and the
// /api/v1/interact WebSocket on WEB_PORT. With BROWSER_API_TOKEN set it asks for the token like the
// app. It posts {type: "widget-ready"} and {type: "widget-size", w, h} to the page that frames it.
import { onMounted, onUnmounted, ref, watch } from "vue";
import PreviewInteract from "./PreviewInteract.vue";
import GraphicsTab from "./GraphicsTab.vue";
import TokenDialog from "./TokenDialog.vue";
import { api, live, startEvents } from "../api.js";

const query = new URLSearchParams(location.search);
const theme = query.get("theme");
// /widget/ograf[?layers=1,2,3]: the Graphics tab's layers and form, without upload or delete.
const widgetId = location.pathname.slice("/widget/".length);
const layerFilter = (query.get("layers") || "")
  .split(",")
  .map((x) => Number(x))
  .filter((n) => Number.isInteger(n) && n >= 1 && n <= 8);
if (theme) document.documentElement.dataset.theme = theme;

const root = ref(null);
const error = ref("");
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
watch(
  () => live.info,
  (info) => {
    if (!info || ready) return;
    ready = true;
    post({ type: "widget-ready" });
    postSize();
  },
  { flush: "post" },
);
onMounted(async () => {
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
  try {
    live.info = await api.get("/api/v1/info"); // a 401 here asks for the token
  } catch (e) {
    error.value = e.message;
  }
  startEvents();
});
onUnmounted(() => observer?.disconnect());
</script>

<template>
  <div ref="root" class="widget-root">
    <GraphicsTab v-if="live.info && widgetId === 'ograf'" widget :layers="layerFilter" />
    <PreviewInteract v-else-if="live.info" fill />
    <div v-else class="empty">{{ error || "Waiting for the browser source…" }}</div>
    <TokenDialog />
  </div>
</template>
