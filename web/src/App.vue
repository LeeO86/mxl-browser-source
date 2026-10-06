<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from "vue";
import { api, auth, live, startEvents, submitToken } from "./api.js";
import SourceTab from "./components/SourceTab.vue";
import TemplateTab from "./components/TemplateTab.vue";
import SettingsTab from "./components/SettingsTab.vue";
import StatusTab from "./components/StatusTab.vue";

const tabs = [
  { id: "source", label: "Source", component: SourceTab },
  { id: "template", label: "Template", component: TemplateTab },
  { id: "settings", label: "Settings", component: SettingsTab },
  { id: "status", label: "Status", component: StatusTab },
];

const currentTab = ref("source");
const current = computed(() => tabs.find((t) => t.id === currentTab.value).component);
const tokenInput = ref("");
const tokenField = ref(null);
const infoError = ref("");

const page = computed(() => live.page || {});
const pageKind = computed(() => ({ loaded: "on", loading: "init" })[page.value.state] || "failed");

function switchTab(id) {
  currentTab.value = id;
  // replaceState: no history entries, so the embedding page's back button
  // is not affected when the UI runs inside an iframe.
  try {
    history.replaceState(null, "", `#${id}`);
  } catch {
    /* sandboxed */
  }
}

function onHashChange() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) currentTab.value = id;
}

watch(() => auth.asking, async (asking) => {
  if (!asking) return;
  await nextTick();
  tokenField.value?.focus();
});

function sendToken(ok) {
  submitToken(ok ? tokenInput.value : "");
  tokenInput.value = "";
}

onMounted(async () => {
  onHashChange();
  window.addEventListener("hashchange", onHashChange);
  try {
    live.info = await api.get("/api/v1/info"); // a 401 here asks for the token
  } catch (e) {
    infoError.value = e.message;
  }
  startEvents();
});
onUnmounted(() => window.removeEventListener("hashchange", onHashChange));
</script>

<template>
  <header>
    <h1>mxl-browser-source</h1>
    <span v-if="live.info" class="muted small">
      {{ live.info.label }} · {{ live.info.format?.name }} · key {{ live.info.key_mode }}
    </span>
    <span class="spacer"></span>
    <span v-if="live.status?.interact?.controlled" class="pill failed" title="A UI session controls the page">INTERACT</span>
    <span v-if="live.page" class="pill" :class="pageKind" :title="page.url">page {{ page.state }}</span>
    <span class="pill" :class="live.connected ? 'on' : 'off'" title="/api/v1/events">{{ live.connected ? "live" : "offline" }}</span>
    <span v-if="live.info" class="muted small">v{{ live.info.version }}</span>
  </header>
  <div v-if="infoError" class="banner bad">API error: {{ infoError }}</div>
  <div v-if="live.status?.render?.degraded" class="banner warn">
    GPU mode was requested but Chromium composites in software (render degraded).
  </div>
  <div v-if="live.status?.devtools?.sessions" class="banner bad">
    A DevTools session is connected: it has full control of the page and its network.
  </div>
  <nav>
    <button v-for="t in tabs" :key="t.id" :class="{ active: currentTab === t.id }" @click="switchTab(t.id)">{{ t.label }}</button>
  </nav>
  <main>
    <component :is="current" />
  </main>

  <div v-if="auth.asking" class="overlay">
    <form class="panel tokenbox" @submit.prevent="sendToken(true)">
      <h3>API token</h3>
      <p class="note">This instance needs its API token (<code>BROWSER_API_TOKEN</code>). It is kept for this browser tab only.</p>
      <label for="api-token">Token</label>
      <input id="api-token" ref="tokenField" v-model="tokenInput" type="password" autocomplete="off" />
      <div class="actions">
        <button class="btn secondary" type="button" @click="sendToken(false)">Cancel</button>
        <button class="btn" type="submit" :disabled="!tokenInput.trim()">OK</button>
      </div>
    </form>
  </div>
</template>
