// SPDX-License-Identifier: MIT
// Lab test (docs/ograf.md §12): alpha ramps, anti-aliased text and soft shadows. page.html draws
// the same markup as a plain page; the lab compares both outputs grain for grain.
export function draw(root) {
  root.innerHTML = `
<style>
  .edges { position: absolute; inset: 0; font-family: 'DejaVu Sans', sans-serif; }
  .ramp { position: absolute; left: 0; right: 0; top: 0; height: 120px; background: linear-gradient(to right, rgba(255,0,0,0), rgba(255,0,0,1)); }
  .steps { position: absolute; left: 0; right: 0; top: 120px; height: 120px; display: flex; }
  .steps div { flex: 1; }
  .third { position: absolute; left: 120px; bottom: 140px; padding: 24px 48px; border-radius: 12px; background: rgba(10,40,120,0.85); box-shadow: 0 8px 40px rgba(0,0,0,0.6); color: #fff; }
  .third h1 { margin: 0; font-size: 64px; font-weight: 700; text-shadow: 0 2px 6px rgba(0,0,0,0.8); }
  .third p { margin: 6px 0 0; font-size: 34px; color: rgba(255,255,255,0.8); }
  .glow { position: absolute; right: 160px; top: 320px; font-size: 120px; color: transparent; -webkit-text-stroke: 2px #fff; text-shadow: 0 0 24px rgba(255,200,0,0.9); }
</style>
<div class="edges">
  <div class="ramp"></div>
  <div class="steps">
    <div style="background:rgba(255,255,255,0)"></div><div style="background:rgba(255,255,255,0.25)"></div>
    <div style="background:rgba(255,255,255,0.5)"></div><div style="background:rgba(255,255,255,0.75)"></div>
    <div style="background:rgba(255,255,255,1)"></div><div style="background:rgba(0,0,0,0.5)"></div>
    <div style="background:rgba(0,0,255,0.5)"></div><div style="background:rgba(0,255,0,1)"></div>
  </div>
  <div class="glow">Ag</div>
  <div class="third"><h1>Grüezi Zürich</h1><p>anti-aliased text, soft shadow</p></div>
</div>`;
}

export default class Edges extends HTMLElement {
  async load() {
    this.style.cssText = 'position:absolute;inset:0;display:none';
    draw(this);
    return { statusCode: 200 };
  }
  async dispose() { this.innerHTML = ''; }
  async playAction() { this.style.display = 'block'; return { statusCode: 200, currentStep: 0 }; }
  async stopAction() { this.style.display = 'none'; return { statusCode: 200 }; }
  async updateAction() { return { statusCode: 200 }; }
  async customAction() { return { statusCode: 404 }; }
}
