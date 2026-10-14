// SPDX-License-Identifier: MIT
// Frame-loop lab graphic (docs/performance.md, M5): draws slot `data.slot` of
// templates/frameloop/anim.js. tests/integration/frameloop_bench.py packs it with anim.js and
// GSAP (templates/frameloop/lib) into one ZIP, so the layer runs the same code as the page.
export default class Anim extends HTMLElement {
  async load(params) {
    this.style.cssText = 'position:absolute;inset:0;display:none';
    const [{ gsap }, { CSSPlugin }, { animate }] = await Promise.all([
      import(import.meta.resolve('./lib/gsap-core.js')),
      import(import.meta.resolve('./lib/CSSPlugin.js')),
      import(import.meta.resolve('./anim.js')),
    ]);
    gsap.registerPlugin(CSSPlugin);
    this._stop = animate(this, Number(params?.data?.slot ?? 0), gsap);
    return { statusCode: 200 };
  }
  async dispose() { this._stop?.(); this.innerHTML = ''; }
  async playAction() { this.style.display = 'block'; return { statusCode: 200, currentStep: 0 }; }
  async stopAction() { this.style.display = 'none'; return { statusCode: 200 }; }
  async updateAction() { return { statusCode: 200 }; }
  async customAction() { return { statusCode: 404 }; }
}
