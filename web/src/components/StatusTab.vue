<script setup>
// Status (SPEC.md §10.2): render mode, grains, timing, audio, NMOS,
// interaction and DevTools. Values come from the 4 Hz `status` events.
import { computed, onMounted, ref, watch } from "vue";
import { act, api, fmt, live, withToken } from "../api.js";

const s = computed(() => live.status);
const info = computed(() => live.info);
const nmos = ref(null); // device and domain ids are not in the status
const targets = ref([]);
const msg = ref({ kind: "", text: "" });

const METER_FLOOR = -60; // dBFS at the left end of a meter

function meter(db) {
  const v = typeof db === "number" && Number.isFinite(db) ? db : -Infinity;
  const pct = Math.max(0, Math.min(100, ((v - METER_FLOOR) / -METER_FLOOR) * 100));
  return { pct, text: Number.isFinite(v) ? v.toFixed(1) : "-inf", kind: v >= -6 ? "hot" : v >= -18 ? "warm" : "" };
}

const senders = computed(() => Object.entries(s.value?.nmos?.senders || {}));
const yesNo = (v) => (v ? "yes" : "no");

async function loadTargets() {
  const list = await act(msg, () => api.get("/devtools/json/list"));
  targets.value = (list || []).filter((t) => t.type === "page" && t.devtoolsFrontendUrl);
}

// `info` may arrive after this tab is opened.
watch(() => info.value?.devtools, (on) => on && loadTargets(), { immediate: true });

onMounted(async () => {
  nmos.value = (await act(msg, () => api.get("/api/v1/nmos"))) || null;
});
</script>

<template>
  <div v-if="msg.text && msg.kind === 'err'" class="panel msg err">{{ msg.text }}</div>
  <div v-if="!s" class="panel muted">Waiting for status…</div>
  <template v-else>
    <div class="grid cards">
      <div class="panel">
        <h3>Render</h3>
        <dl class="kv">
          <dt>Mode</dt><dd>{{ s.render?.mode }}<span class="muted"> (requested {{ info?.render?.requested || "–" }})</span></dd>
          <dt>Degraded</dt>
          <dd><span class="pill" :class="s.render?.degraded ? 'failed' : 'on'">{{ yesNo(s.render?.degraded) }}</span></dd>
          <dt>Paint p50</dt><dd>{{ fmt(s.render?.paint_ms?.p50) }} ms</dd>
          <dt>Paint p95</dt><dd>{{ fmt(s.render?.paint_ms?.p95) }} ms</dd>
          <dt>Convert</dt><dd>{{ fmt(s.render?.convert_ms) }} ms</dd>
        </dl>
      </div>
      <div class="panel">
        <h3>Grains</h3>
        <dl class="kv">
          <dt>Video</dt><dd>{{ s.grains?.video ?? "–" }}</dd>
          <dt>Key</dt><dd>{{ s.grains?.key ?? "–" }}</dd>
          <dt>Repeated</dt><dd :class="{ warn: s.grains?.repeated }">{{ s.grains?.repeated ?? "–" }}</dd>
          <dt>Missed</dt><dd :class="{ bad: s.grains?.missed }">{{ s.grains?.missed ?? "–" }}</dd>
          <dt>Late paints</dt><dd :class="{ warn: s.grains?.late_paints }">{{ s.grains?.late_paints ?? "–" }}</dd>
          <dt>BeginFrames</dt><dd>{{ s.grains?.begin_frames ?? "–" }}</dd>
        </dl>
      </div>
      <div class="panel">
        <h3>Interaction</h3>
        <dl class="kv">
          <dt>Sessions</dt><dd>{{ s.interact?.sessions ?? 0 }}</dd>
          <dt>Controlled</dt>
          <dd><span class="pill" :class="s.interact?.controlled ? 'failed' : 'init'">{{ yesNo(s.interact?.controlled) }}</span></dd>
          <dt>Ready</dt>
          <dd><span class="pill" :class="s.ready ? 'on' : 'failed'">{{ yesNo(s.ready) }}</span></dd>
        </dl>
      </div>
    </div>

    <div class="panel">
      <h3>Audio</h3>
      <div v-if="!s.audio || !s.audio.channels" class="muted">no audio flow</div>
      <template v-else>
        <dl class="kv">
          <dt>Stream</dt>
          <dd><span class="pill" :class="s.audio.stream ? 'on' : 'init'">{{ yesNo(s.audio.stream) }}</span></dd>
          <dt>Drift</dt><dd>{{ fmt(s.audio.drift_ppm) }} ppm</dd>
          <dt>Buffer</dt><dd>{{ fmt(s.audio.buffer_ms) }} ms</dd>
          <dt>Under / overruns</dt><dd>{{ s.audio.underruns ?? 0 }} / {{ s.audio.overruns ?? 0 }}</dd>
        </dl>
        <div class="meters">
          <div v-for="(db, i) in s.audio.peaks_dbfs || []" :key="i" class="meter-row">
            <span class="muted small">{{ i + 1 }}</span>
            <div class="meter" role="meter" :aria-label="`Channel ${i + 1} peak`" :aria-valuenow="meter(db).text"
                 aria-valuemin="-60" aria-valuemax="0">
              <div class="meter-fill" :class="meter(db).kind" :style="{ width: meter(db).pct + '%' }"></div>
            </div>
            <span class="small num">{{ meter(db).text }} dBFS</span>
          </div>
        </div>
      </template>
    </div>

    <div class="panel">
      <h3>NMOS</h3>
      <dl class="kv">
        <dt>Registered</dt>
        <dd><span class="pill" :class="s.nmos?.registered ? 'on' : 'failed'">{{ yesNo(s.nmos?.registered) }}</span></dd>
        <dt>Node</dt><dd><code>{{ s.nmos?.node_id || "–" }}</code></dd>
        <dt>Device</dt><dd><code>{{ nmos?.device_id || "–" }}</code></dd>
        <dt>Domain</dt><dd><code>{{ nmos?.domain_id || "–" }}</code></dd>
      </dl>
      <table style="margin-top:.6rem">
        <thead><tr><th>Sender</th><th>Id</th><th>Flow</th><th>Enabled</th></tr></thead>
        <tbody>
          <tr v-if="!senders.length"><td colspan="4" class="muted">no senders</td></tr>
          <tr v-for="[name, snd] in senders" :key="name">
            <td>{{ name }}</td>
            <td><code>{{ snd.id }}</code></td>
            <td><code>{{ snd.flow_id }}</code></td>
            <td><span class="pill" :class="snd.enabled ? 'on' : 'off'">{{ yesNo(snd.enabled) }}</span></td>
          </tr>
        </tbody>
      </table>
    </div>

    <div v-if="info?.devtools" class="panel">
      <h3>
        DevTools
        <button class="btn secondary small right-btn" @click="loadTargets">Refresh</button>
      </h3>
      <div class="warnbox">DevTools gives full control of the page and its network. Connected sessions: {{ s.devtools?.sessions ?? 0 }}.</div>
      <table>
        <tbody>
          <tr v-if="!targets.length"><td class="muted">no page targets</td></tr>
          <tr v-for="t in targets" :key="t.id">
            <td>{{ t.title || t.url }}<div class="muted small ellipsis">{{ t.url }}</div></td>
            <td class="right nowrap">
              <a class="btn small" :href="withToken(t.devtoolsFrontendUrl)" target="_blank" rel="noopener">Open DevTools</a>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <div v-if="info" class="panel">
      <h3>Versions</h3>
      <dl class="kv">
        <dt>mxl-browser-source</dt><dd>{{ info.version }}</dd>
        <dt>CEF</dt><dd>{{ info.cef }}</dd>
        <dt>Chromium</dt><dd>{{ info.chromium }}</dd>
        <dt>MXL</dt><dd><code>{{ info.mxl_revision }}</code></dd>
        <dt>nmos-cpp</dt><dd><code>{{ info.nmos_cpp }}</code></dd>
        <dt>Format</dt><dd>{{ info.format?.name }} ({{ info.format?.width }}×{{ info.format?.height }}, {{ info.format?.rate }})</dd>
        <dt>Key mode</dt><dd>{{ info.key_mode }}</dd>
        <dt>Audio channels</dt><dd>{{ info.audio_channels }}</dd>
      </dl>
    </div>
  </template>
</template>
