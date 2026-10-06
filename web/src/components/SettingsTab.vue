<script setup>
// Settings (SPEC.md §11): environment > config file > defaults. Edits go to
// the file layer; keys set by the environment are read-only here.
import { computed, onMounted, reactive, ref } from "vue";
import { act, api } from "../api.js";

const config = ref(null);
const drafts = reactive({});
const msg = ref({ kind: "", text: "" });
const importer = ref(null);

async function load() {
  const res = await act(msg, () => api.get("/api/v1/config"));
  if (!res) return;
  config.value = res;
  for (const s of res.settings) drafts[s.key] = s.secret ? "" : s.value ?? "";
}

// A secret's value is never sent to the UI: its field starts empty and is
// only written when something was typed.
const isDirty = (s) => (s.secret ? drafts[s.key] !== "" : drafts[s.key] !== (s.value ?? ""));
const changed = computed(() => (config.value?.settings || []).filter((s) => s.source !== "env" && isDirty(s)));

function restartText(res, done) {
  const keys = res?.restart_required || [];
  return keys.length ? `${done} Restart required for: ${keys.join(", ")}.` : done;
}

async function write(body, done) {
  const res = await act(msg, () => api.put("/api/v1/config", body));
  if (res === undefined) return;
  msg.value = { kind: "ok", text: restartText(res, done) };
  await load();
}

const save = () => write(Object.fromEntries(changed.value.map((s) => [s.key, drafts[s.key]])), "Saved.");
const reset = (key) => write({ [key]: null }, `${key} removed from the file.`);

async function exportConfig() {
  const blob = await act(msg, () => api.blob("/api/v1/config/export"));
  if (!blob) return;
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "config.json";
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function importConfig(e) {
  const file = e.target.files?.[0];
  e.target.value = "";
  if (!file) return;
  const res = await act(msg, async () => api.post("/api/v1/config/import", JSON.parse(await file.text())));
  if (res === undefined) return;
  msg.value = { kind: "ok", text: restartText(res, `Imported ${file.name}.`) };
  await load();
}

onMounted(load);
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <div class="row tight center">
      <span class="muted small grow">File: <code>{{ config?.file || "–" }}</code> · the environment wins over the file; a hand edit of the file needs a restart.</span>
      <button class="btn secondary" @click="exportConfig">Export</button>
      <button class="btn secondary" @click="importer.click()">Import…</button>
      <input ref="importer" type="file" accept="application/json,.json" class="sr-only"
             aria-label="Import configuration file" @change="importConfig" />
    </div>
    <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
  </div>

  <div v-if="config" class="panel">
    <table class="settings">
      <thead>
        <tr><th>Key</th><th>Value</th><th>Default</th><th>Source</th><th></th><th>Description</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in config.settings" :key="s.key">
          <td><label :for="`cfg-${s.key}`" class="keylabel">{{ s.key }}</label></td>
          <td class="valuecell">
            <input :id="`cfg-${s.key}`" v-model="drafts[s.key]" :type="s.secret ? 'password' : 'text'"
                   :disabled="s.source === 'env'" autocomplete="off"
                   :placeholder="s.secret ? (s.set ? '(set — type to replace)' : '(not set)') : ''"
                   :title="s.source === 'env' ? 'Set by the environment; change it there' : ''"
                   :class="{ dirty: s.source !== 'env' && isDirty(s) }" />
          </td>
          <td class="muted small">{{ s.secret ? "" : s.default }}</td>
          <td class="nowrap">
            <span class="envbadge" :class="s.source">{{ s.source.toUpperCase() }}</span>
            <button v-if="s.source === 'file'" class="btn small secondary" title="Remove from the file (back to the default)"
                    @click="reset(s.key)">Reset</button>
          </td>
          <td><span v-if="s.restart" class="envbadge restart" title="Applies on the next start">RESTART</span></td>
          <td class="small">{{ s.description }}</td>
        </tr>
      </tbody>
    </table>
    <div class="actions">
      <span class="muted small grow">{{ changed.length }} change{{ changed.length === 1 ? "" : "s" }}</span>
      <button class="btn" :disabled="!changed.length" @click="save">Save</button>
    </div>
  </div>
</template>
