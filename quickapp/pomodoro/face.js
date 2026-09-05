'use strict';

/* mood-orb 表情名 → 色点能量。设备端不画假五官，只做呼吸色点。 */
var POMODORO_RECIPES = {
  content: { energy: 0.3,  anim: 'float', accent: 0xc8c2b8 },
  sleepy:  { energy: 0.12, anim: 'droop', accent: 0xb8b4c8 }
};

function pomodoroMood(state) {
  return {
    expression: state.phase === 'long' ? 'sleepy' : 'content',
    gesture: null
  };
}

function createPomodoroFace(opts) {
  opts = opts || {};
  const ox = opts.x || 0;
  const oy = opts.y || 0;
  const size = opts.size || 148;
  const barX = opts.barX;
  const barY = opts.barY;
  const barW = opts.barW;
  const body = ui.panel(ox, oy, size, size, 0x171d1f, 8, 255);
  const glow = ui.panel(ox + 18, oy + 18, size - 36, size - 36, 0xb0a8d9, 8, 70);
  const barBg = ui.rect(barX, barY, barW, 6, 0x1c2427);
  const bar = ui.rect(barX, barY, barW, 6, 0x8fc8d9);

  let t = 0;
  let gestureT = 0;
  let gestureName = null;
  let recipe = POMODORO_RECIPES.content;
  let name = 'content';
  let lastSeq = -1;
  let lastName = '';

  function phaseDuration(state) {
    if (state.phase === 'short') return state.shortMin * 60000;
    if (state.phase === 'long') return state.longMin * 60000;
    return state.workMin * 60000;
  }

  function playGesture(g) {
    if (!g) return;
    gestureName = g;
    gestureT = 1;
  }

  function poke() {
    playGesture('bounce');
    if (opts.onChange) {
      opts.onChange({ expression: name, gesture: 'bounce', duration: 1.2 });
    }
  }

  function setState(state) {
    const mood = pomodoroMood(state, lastSeq);
    lastSeq = state.eventSeq;
    if (mood.expression !== lastName || mood.gesture) {
      lastName = mood.expression;
      name = mood.expression;
      recipe = POMODORO_RECIPES[name] || POMODORO_RECIPES.content;
      if (mood.gesture) playGesture(mood.gesture);
      if (opts.onChange) {
        opts.onChange({
          expression: mood.expression,
          gesture: mood.gesture || undefined,
          duration: mood.gesture ? 2 : undefined
        });
      }
    }
    const total = phaseDuration(state) || 1;
    const ratio = Math.max(0, Math.min(1, state.remaining / total));
    ui.setSize(bar, Math.max(6, Math.floor(barW * ratio)), 6);
    ui.setColor(bar, recipe.accent);
    ui.setColor(glow, recipe.accent);
    return mood;
  }

  function frame() {
    const energy = recipe.energy;
    t += 0.07 + energy * 0.06;
    if (gestureT > 0) gestureT -= 0.07;
    if (gestureT < 0) gestureT = 0;

    let dy = 0;
    if (recipe.anim === 'bob') dy = Math.sin(t) * (3 + 3 * energy);
    else if (recipe.anim === 'float') dy = Math.sin(t) * (2 + energy);
    else if (recipe.anim === 'droop') dy = 4 + Math.sin(t) * 1;
    else if (recipe.anim === 'pulse') dy = Math.sin(t * 1.6) * (1 + energy);

    const p = 1 - gestureT;
    if (gestureT > 0 && gestureName === 'jump') {
      dy -= 14 * Math.sin(Math.min(1, p * 1.35) * Math.PI);
    } else if (gestureT > 0 && gestureName === 'bounce') {
      dy -= 9 * Math.sin(Math.min(1, p * 1.45) * Math.PI);
    } else if (gestureT > 0 && gestureName === 'nod') {
      dy += 5 * Math.sin(p * Math.PI * 2) * gestureT;
    }

    const pulse = recipe.anim === 'pulse' ?
                  56 + Math.floor((0.5 + 0.5 * Math.sin(t * 1.6)) * 50 * energy) :
                  64 + Math.floor(energy * 40);
    ui.setPos(body, ox, oy + dy);
    ui.setPos(glow, ox + 18, oy + 18 + dy);
    ui.setOpacity(glow, pulse);
    ui.setPos(barBg, barX, barY);
    ui.setPos(bar, barX, barY);
  }

  return {
    setState: setState,
    frame: frame,
    poke: poke,
    getExpression: function () { return name; },
    layout: { x: ox, y: oy, size: size }
  };
}

if (typeof globalThis !== 'undefined') {
  globalThis.POMODORO_RECIPES = POMODORO_RECIPES;
  globalThis.pomodoroMood = pomodoroMood;
  globalThis.createPomodoroFace = createPomodoroFace;
}
