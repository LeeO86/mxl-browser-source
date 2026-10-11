<script setup>
// Asks for BROWSER_API_TOKEN when a request gets a 401 (api.js `auth.asking`); in the app and in a widget.
import { nextTick, ref, watch } from "vue";
import { auth, submitToken } from "../api.js";

const tokenInput = ref("");
const tokenField = ref(null);

watch(() => auth.asking, async (asking) => {
  if (!asking) return;
  await nextTick();
  tokenField.value?.focus();
});

function sendToken(ok) {
  submitToken(ok ? tokenInput.value : "");
  tokenInput.value = "";
}
</script>

<template>
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
