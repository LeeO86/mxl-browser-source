<script setup>
// Template control, CasparCG style (SPEC.md §4.2, §4.4).
import { computed, onMounted, ref } from "vue";
import { act, api, clock, live } from "../api.js";

const msg = ref({ kind: "", text: "" });
const data = ref("");
const fn = ref("");
const args = ref("[]");
const files = ref([]);

const FN_RE = /^[A-Za-z_$][A-Za-z0-9_$.]*$/;

// `update` sends an object when the editor holds valid JSON (object or
// array), the text itself otherwise.
const parsedData = computed(() => {
  try {
    const v = JSON.parse(data.value);
    if (v !== null && typeof v === "object") return { kind: "JSON", value: v };
  } catch {
    /* plain text */
  }
  return { kind: "text", value: data.value };
});

const argsError = computed(() => {
  try {
    return Array.isArray(JSON.parse(args.value)) ? "" : "Arguments must be a JSON array.";
  } catch {
    return "Arguments must be a JSON array.";
  }
});
const fnError = computed(() => (fn.value && !FN_RE.test(fn.value) ? "Not a valid function name." : ""));

const verb = (v, body) => act(msg, () => api.post(`/api/v1/template/${v}`, body), `${v} queued.`);
const sendUpdate = () => verb("update", { data: parsedData.value.value });
const invoke = () => verb("invoke", { function: fn.value, args: JSON.parse(args.value) });

const templateUrl = (file) => `https://templates.local/${file.split("/").map(encodeURIComponent).join("/")}`;
const open = (file) => act(msg, () => api.post("/api/v1/source/navigate", { url: templateUrl(file) }), `Navigating to ${file}.`);

async function loadFiles() {
  files.value = (await act(msg, () => api.get("/api/v1/templates")))?.files || [];
}

const events = computed(() => live.log.filter((e) => e.kind === "event").reverse());

onMounted(loadFiles);
</script>

<template>
  <div class="panel">
    <h3>Control</h3>
    <div class="row tight">
      <button class="btn" @click="verb('play')">Play</button>
      <button class="btn secondary" @click="verb('next')">Next</button>
      <button class="btn secondary" @click="verb('stop')">Stop</button>
      <button class="btn danger" @click="verb('remove')">Remove</button>
    </div>
    <p class="note">Calls <code>play()</code>, <code>next()</code>, <code>stop()</code> or <code>remove()</code> in the page. Calls before the page has loaded are queued.</p>
    <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Update</h3>
      <label for="tpl-data">Data for <code>update(data)</code> — JSON or plain text</label>
      <textarea id="tpl-data" v-model="data" class="code" spellcheck="false" placeholder='{"f0": "Name", "f1": "Title"}'></textarea>
      <div class="actions">
        <span class="muted small grow">sent as {{ parsedData.kind === "JSON" ? "JSON object" : "string" }}</span>
        <button class="btn" @click="sendUpdate">Update</button>
      </div>
    </div>

    <div class="panel">
      <h3>Invoke</h3>
      <label for="tpl-fn">Function</label>
      <input id="tpl-fn" v-model="fn" placeholder="e.g. setScore" autocomplete="off" />
      <div v-if="fnError" class="msg err">{{ fnError }}</div>
      <label for="tpl-args">Arguments (JSON array)</label>
      <textarea id="tpl-args" v-model="args" class="code short" spellcheck="false"></textarea>
      <div v-if="argsError" class="msg err">{{ argsError }}</div>
      <div class="actions">
        <button class="btn" :disabled="!fn || !!fnError || !!argsError" @click="invoke">Invoke</button>
      </div>
    </div>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>
        Local templates
        <button class="btn secondary small right-btn" @click="loadFiles">Refresh</button>
      </h3>
      <table>
        <tbody>
          <tr v-if="!files.length"><td class="muted">no files</td></tr>
          <tr v-for="f in files" :key="f">
            <td><button class="linkbtn" :title="templateUrl(f)" @click="open(f)">{{ f }}</button></td>
          </tr>
        </tbody>
      </table>
      <p class="note">Click a file to show it (<code>https://templates.local/…</code>).</p>
    </div>

    <div class="panel">
      <h3>Page events</h3>
      <div class="log" role="log" aria-label="Page events">
        <div v-if="!events.length" class="muted">nothing yet — pages report with <code>window.mxlBrowserSource.post(obj)</code></div>
        <div v-for="(e, i) in events" :key="i" class="logline">
          <span class="muted">{{ clock(e.time) }}</span>
          <span class="logtext">{{ e.text }}</span>
        </div>
      </div>
    </div>
  </div>
</template>
