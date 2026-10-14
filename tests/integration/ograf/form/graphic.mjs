// SPDX-License-Identifier: MIT
// UI test graphic (docs/ograf.md §8): every GDD type in its schema; shows its data, step and the
// last custom action as text so the output picture shows what the form sent.
export default class Form extends HTMLElement {
  async load(params) {
    this.data = params?.data ?? {};
    this.step = 0;
    this.last = '';
    this.style.cssText = 'position:absolute;left:80px;top:80px;display:none;font:28px DejaVu Sans,sans-serif;color:#fff;padding:24px;border-radius:10px;max-width:1500px;white-space:pre-wrap';
    this.render();
    return { statusCode: 200 };
  }
  render() {
    const d = this.data;
    this.style.background = d.shade || '#000000cc';
    this.style.borderLeft = `16px solid ${d.color || '#1e64c8'}`;
    this.style.opacity = String(d.opacity ?? 1);
    this.textContent = `${d.headline ?? ''}  [step ${this.step + 1}]${this.last ? '  ' + this.last : ''}\n${JSON.stringify(d)}`;
  }
  async dispose() { this.textContent = ''; }
  async playAction(p) {
    this.step = Math.max(0, Math.min(2, (p?.goto ?? this.step + (p?.delta ?? 0))));
    this.style.display = 'block';
    this.render();
    return { statusCode: 200, currentStep: this.step };
  }
  async stopAction() { this.style.display = 'none'; return { statusCode: 200 }; }
  async updateAction(p) { this.data = p?.data ?? {}; this.render(); return { statusCode: 200 }; }
  async customAction(p) {
    this.last = `${p.id}: ${JSON.stringify(p.payload ?? {})}`;
    this.render();
    return { statusCode: 200 };
  }
}
