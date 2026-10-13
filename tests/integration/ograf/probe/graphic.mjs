// SPDX-License-Identifier: MIT
// Integration-test graphic: posts each call to the function's event stream with its layer.
const post = (o) => window.top.mxlBrowserSource && window.top.mxlBrowserSource.post({ probe: o });

export default class Probe extends HTMLElement {
  async load({ data, renderType, renderCharacteristics }) {
    this.data = Object.assign({}, data);
    this.step = undefined;
    this.box = document.createElement('div');
    Object.assign(this.box.style, { position: 'absolute', inset: '0', display: 'none', background: this.data.color || '#ff0000' });
    this.appendChild(this.box);
    post({ call: 'load', data: this.data, renderType, renderCharacteristics });
    return { statusCode: 200 };
  }
  async dispose() {
    post({ call: 'dispose', text: this.data && this.data.text });
    this.innerHTML = '';
  }
  async playAction({ goto, delta, skipAnimation }) {
    const target = goto !== undefined && goto >= 0 ? goto : (this.step === undefined ? -1 : this.step) + (delta === undefined ? 1 : delta);
    this.step = target >= 2 ? undefined : target;
    this.box.style.display = this.step === undefined ? 'none' : 'block';
    post({ call: 'playAction', step: this.step === undefined ? null : this.step, skipAnimation: !!skipAnimation, text: this.data.text });
    return { statusCode: 200, currentStep: this.step };
  }
  async stopAction() {
    this.step = undefined;
    this.box.style.display = 'none';
    post({ call: 'stopAction', text: this.data.text });
    return { statusCode: 200 };
  }
  async updateAction({ data }) {
    Object.assign(this.data, data);
    if (data.color) this.box.style.background = data.color;
    post({ call: 'updateAction', data: this.data });
    return { statusCode: 200 };
  }
  async customAction({ id, payload }) {
    if (id !== 'fetch') return { statusCode: 404, statusMessage: 'unknown action' };
    try {
      await fetch(payload.url, { mode: 'no-cors' });
      post({ call: 'fetch', ok: true });
      return { statusCode: 200, result: 'fetched' };
    } catch (e) {
      post({ call: 'fetch', ok: false });
      return { statusCode: 502, statusMessage: 'blocked: ' + e };
    }
  }
}
