// SPDX-License-Identifier: MIT
// Frame-loop test elements (docs/performance.md, M5): one animated graphic per slot, in the
// animation kinds of the EBU OGraf examples. Every element changes on every frame, so any
// repeated grain while it runs is a held frame. index.html puts n of them into one page; the
// OGraf lab graphic (tests/integration/ograf/anim) puts one into each layer, at the same place,
// so both modes draw the same pixels.
//   kind 0: GSAP timeline, a lower third sliding and its accent bar scaling (l3rd-name)
//   kind 1: CSS @keyframes, a gradient shimmer and a pulsing dot (scoreboard)
//   kind 2: GSAP tween, an SVG logo rotating and scaling (ograf-logo)
//   kind 3: CSS transform ticker plus a GSAP-driven counter text (headline, bar chart)
const CSS = `
.fl-slot { position: absolute; width: 480px; height: 540px; overflow: hidden; font-family: 'DejaVu Sans', sans-serif; color: #fff; }
.fl-l3rd { position: absolute; left: 40px; top: 360px; width: 380px; padding: 16px 24px; background: rgba(10,40,120,0.85); border-radius: 8px; box-shadow: 0 6px 24px rgba(0,0,0,0.5); }
.fl-l3rd h1 { margin: 0; font-size: 40px; } .fl-l3rd p { margin: 4px 0 0; font-size: 24px; opacity: .85; }
.fl-bar { position: absolute; left: 0; bottom: 0; height: 8px; width: 100%; background: #ffb000; transform-origin: 0 50%; }
.fl-shimmer { position: absolute; left: 40px; top: 120px; width: 400px; height: 200px; border-radius: 12px;
  background: linear-gradient(110deg, #123 20%, #4a7bd0 40%, #123 60%) 0 0 / 300% 100%; animation: fl-shimmer 3s linear infinite; }
.fl-dot { position: absolute; left: 220px; top: 380px; width: 40px; height: 40px; border-radius: 50%; background: #e02020; animation: fl-pulse 1.4s ease-in-out infinite; }
@keyframes fl-shimmer { from { background-position: 100% 0; } to { background-position: 0 0; } }
@keyframes fl-pulse { 0%, 100% { opacity: 1; transform: scale(1); } 50% { opacity: .2; transform: scale(1.6); } }
.fl-logo { position: absolute; left: 140px; top: 170px; width: 200px; height: 200px; }
.fl-ticker { position: absolute; left: 0; top: 440px; width: 480px; height: 60px; background: rgba(0,0,0,.6); overflow: hidden; }
.fl-ticker div { position: absolute; white-space: nowrap; font-size: 36px; line-height: 60px; animation: fl-scroll 6s linear infinite; }
@keyframes fl-scroll { from { transform: translateX(480px); } to { transform: translateX(-100%); } }
.fl-count { position: absolute; left: 40px; top: 160px; font-size: 120px; font-weight: 700; text-shadow: 0 4px 12px rgba(0,0,0,.8); }
`;

let styled = new WeakSet();

/// Draws the element of `slot` (0-7) into `root` and starts its animation. Returns a stop function.
export function animate(root, slot, gsap) {
  const doc = root.ownerDocument;
  if (!styled.has(doc)) {
    const style = doc.createElement('style');
    style.textContent = CSS;
    doc.head.appendChild(style);
    styled.add(doc);
  }
  const box = doc.createElement('div');
  box.className = 'fl-slot';
  box.style.left = `${(slot % 4) * 480}px`;
  box.style.top = `${Math.floor(slot / 4) * 540}px`;
  root.appendChild(box);
  const tweens = [];
  switch (slot % 4) {
  case 0: {
    box.innerHTML = `<div class="fl-l3rd"><h1>Slot ${slot + 1}</h1><p>GSAP timeline</p><div class="fl-bar"></div></div>`;
    const tl = gsap.timeline({ repeat: -1, yoyo: true });
    tl.fromTo(box.querySelector('.fl-l3rd'), { x: -40, opacity: 0.4 }, { x: 20, opacity: 1, duration: 1.6, ease: 'power2.inOut' })
      .fromTo(box.querySelector('.fl-bar'), { scaleX: 0 }, { scaleX: 1, duration: 1.6, ease: 'power1.inOut' }, 0);
    tweens.push(tl);
    break;
  }
  case 1:
    box.innerHTML = '<div class="fl-shimmer"></div><div class="fl-dot"></div>';
    break;
  case 2: {
    box.innerHTML = `<svg class="fl-logo" viewBox="0 0 100 100"><circle cx="50" cy="50" r="44" fill="none" stroke="#fff" stroke-width="8"/>
      <path d="M50 14 L60 50 L50 86 L40 50 Z" fill="#ffb000"/><text x="50" y="58" font-size="20" text-anchor="middle" fill="#fff">${slot + 1}</text></svg>`;
    tweens.push(gsap.to(box.querySelector('.fl-logo'), { rotation: 360, duration: 4, repeat: -1, ease: 'none' }));
    tweens.push(gsap.fromTo(box.querySelector('.fl-logo'), { scale: 0.8 }, { scale: 1.1, duration: 1.2, repeat: -1, yoyo: true, ease: 'sine.inOut' }));
    break;
  }
  default: {
    box.innerHTML = '<div class="fl-count">0</div><div class="fl-ticker"><div>+++ frame loop test +++ GSAP and CSS +++ OGraf and page mode +++</div></div>';
    const count = box.querySelector('.fl-count');
    const state = { v: 0 };
    tweens.push(gsap.to(state, { v: 9999, duration: 200, repeat: -1, ease: 'none', onUpdate: () => { count.textContent = state.v.toFixed(1); } }));
    break;
  }
  }
  return () => { tweens.forEach((t) => t.kill()); box.remove(); };
}
