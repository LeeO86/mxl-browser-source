<script setup>
// Source page (SPEC.md §4.1, §10.2): URL and navigation, preview with
// interaction, look (background, zoom, scale, CSS, JS), presets, console.
import { computed, onMounted, reactive, ref, watch } from "vue";
import { act, api, clock, live } from "../api.js";
import PreviewInteract from "./PreviewInteract.vue";

const msg = ref({ kind: "", text: "" });
const url = ref("");
let urlFocused = false;
const cookies = ref(false);
const look = reactive({ transparent: true, color: "#000000", zoom: 1, dsf: 1, css: "", js: "" });
const presets = ref([]);
const presetName = ref("");
const armed = ref(""); // preset whose delete button waits for a second click

const page = computed(() => live.page || {});
const pageKind = computed(() => ({ loaded: "on", loading: "init" })[page.value.state] || (page.value.state ? "failed" : "init"));

function showSource(doc) {
  if (!doc) return;
  if (!urlFocused) url.value = doc.url || "";
  look.transparent = doc.background === "transparent";
  if (!look.transparent && /^#[0-9a-fA-F]{6}$/.test(doc.background || "")) look.color = doc.background;
  look.zoom = doc.zoom ?? 1;
  look.dsf = doc.device_scale_factor ?? 1;
  look.css = doc.css || "";
  look.js = doc.js || "";
}

// Follow navigation done inside the page, unless the operator is typing.
watch(() => page.value.url, (u) => {
  if (u && !urlFocused) url.value = u;
});

async function loadPresets() {
  presets.value = (await act(msg, () => api.get("/api/v1/presets"))) || [];
}

const go = () => act(msg, () => api.post("/api/v1/source/navigate", { url: url.value.trim() }), "Navigating.");
const reload = () => act(msg, () => api.post("/api/v1/source/reload", { ignore_cache: false }), "Reloading.");
const stop = () => act(msg, () => api.post("/api/v1/source/stop"), "Stopped.");
const clearCache = () =>
  act(msg, () => api.post("/api/v1/source/clear-cache", { cookies: cookies.value }),
    cookies.value ? "Cache and cookies cleared." : "Cache cleared.");

async function applyLook() {
  const doc = await act(msg, () => api.patch("/api/v1/source", {
    background: look.transparent ? "transparent" : look.color,
    zoom: Number(look.zoom),
    device_scale_factor: Number(look.dsf),
    css: look.css,
    js: look.js,
  }), "Applied.");
  showSource(doc);
}

async function applyPreset(name) {
  showSource(await act(msg, () => api.post(`/api/v1/presets/${encodeURIComponent(name)}/apply`), `Preset "${name}" applied.`));
}

async function savePreset() {
  const name = presetName.value.trim();
  if (!name) return;
  // "Current" is what is on air: the source document as the server has it.
  const ok = await act(msg, async () => {
    const current = await api.get("/api/v1/source");
    await api.post("/api/v1/presets", { ...current, name });
    return true;
  }, `Preset "${name}" saved.`);
  if (ok) {
    presetName.value = "";
    loadPresets();
  }
}

let armTimer = 0;
async function deletePreset(name) {
  if (armed.value !== name) {
    armed.value = name;
    clearTimeout(armTimer);
    armTimer = setTimeout(() => (armed.value = ""), 3000);
    return;
  }
  armed.value = "";
  await act(msg, () => api.del(`/api/v1/presets/${encodeURIComponent(name)}`), `Preset "${name}" deleted.`);
  loadPresets();
}

const consoleRows = computed(() => [...live.console].reverse());
const logRows = computed(() => [...live.log].reverse());

onMounted(async () => {
  showSource(await act(msg, () => api.get("/api/v1/source")));
  loadPresets();
});
</script>

<template>
  <div class="panel">
    <form class="urlbar" @submit.prevent="go">
      <label class="sr-only" for="src-url">Page URL</label>
      <input id="src-url" v-model="url" type="text" inputmode="url" placeholder="https://…" autocomplete="off"
             @focus="urlFocused = true" @blur="urlFocused = false" />
      <button class="btn" type="submit">Go</button>
      <button class="btn secondary" type="button" @click="reload">Reload</button>
      <button class="btn secondary" type="button" @click="stop">Stop</button>
      <button class="btn secondary" type="button" @click="clearCache">Clear cache</button>
      <label class="inline"><input v-model="cookies" type="checkbox" /> cookies too</label>
    </form>
    <div class="pagebar">
      <span class="pill" :class="pageKind">{{ page.state || "unknown" }}</span>
      <span class="pagetitle">{{ page.title || "untitled" }}</span>
      <span v-if="page.error" class="msg err inline-msg">{{ page.error }}</span>
    </div>
    <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
  </div>

  <div class="panel">
    <PreviewInteract />
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Look</h3>
      <fieldset class="bare">
        <legend>Background</legend>
        <div class="row tight">
          <label class="inline"><input v-model="look.transparent" type="radio" :value="true" /> transparent</label>
          <label class="inline"><input v-model="look.transparent" type="radio" :value="false" /> colour</label>
          <input v-model="look.color" type="color" class="swatch" aria-label="Background colour" :disabled="look.transparent" />
        </div>
      </fieldset>
      <div class="row">
        <div>
          <label for="src-zoom">Zoom</label>
          <input id="src-zoom" v-model.number="look.zoom" type="number" step="0.05" min="0.1" />
        </div>
        <div>
          <label for="src-dsf">Device scale</label>
          <input id="src-dsf" v-model.number="look.dsf" type="number" step="0.25" min="0.25" />
        </div>
      </div>
      <label for="src-css">CSS (injected after every load)</label>
      <textarea id="src-css" v-model="look.css" class="code" spellcheck="false"></textarea>
      <label for="src-js">JavaScript (run after every load)</label>
      <textarea id="src-js" v-model="look.js" class="code" spellcheck="false"></textarea>
      <div class="actions">
        <button class="btn" @click="applyLook">Apply</button>
      </div>
    </div>

    <div class="panel">
      <h3>Presets</h3>
      <table>
        <tbody>
          <tr v-if="!presets.length"><td class="muted">no presets</td></tr>
          <tr v-for="p in presets" :key="p.name">
            <td><strong>{{ p.name }}</strong><div class="muted small ellipsis" :title="p.url">{{ p.url }}</div></td>
            <td class="nowrap right">
              <button class="btn small" @click="applyPreset(p.name)">Apply</button>
              <button class="btn small danger" @click="deletePreset(p.name)">{{ armed === p.name ? "Sure?" : "Delete" }}</button>
            </td>
          </tr>
        </tbody>
      </table>
      <form class="row tight" style="margin-top:.7rem" @submit.prevent="savePreset">
        <div>
          <label for="preset-name">Save current source as preset</label>
          <input id="preset-name" v-model="presetName" placeholder="Name" />
        </div>
        <button class="btn secondary grow-0 bottom" type="submit" :disabled="!presetName.trim()">Save</button>
      </form>
      <p class="note">Saving under an existing name replaces that preset.</p>
    </div>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Console</h3>
      <div class="log" role="log" aria-label="Page console">
        <div v-if="!consoleRows.length" class="muted">no messages</div>
        <div v-for="(m, i) in consoleRows" :key="i" class="logline">
          <span class="muted">{{ clock(m.time) }}</span>
          <span class="lvl" :class="m.level">{{ m.level }}</span>
          <span class="logtext">{{ m.message }}</span>
          <span v-if="m.source" class="muted small"> {{ m.source }}<template v-if="m.line">:{{ m.line }}</template></span>
        </div>
      </div>
    </div>
    <div class="panel">
      <h3>Dialogs and page events</h3>
      <div class="log" role="log" aria-label="Dialogs and page events">
        <div v-if="!logRows.length" class="muted">nothing yet</div>
        <div v-for="(m, i) in logRows" :key="i" class="logline">
          <span class="muted">{{ clock(m.time) }}</span>
          <span class="lvl" :class="m.kind">{{ m.kind }}</span>
          <span class="logtext">{{ m.text }}</span>
        </div>
      </div>
    </div>
  </div>
</template>
