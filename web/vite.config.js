import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

// The build inlines everything into dist/index.html so the C++ binary can
// embed one file (cmake/EmbedFile.cmake).
const target = "http://127.0.0.1:8160";

export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  server: {
    // `npm run dev` against a locally running mxl-browser-source instance.
    proxy: {
      "/api": { target, ws: true },
      "/devtools": { target, ws: true },
      "/livez": target,
      "/readyz": target,
      "/metrics": target,
    },
  },
});
