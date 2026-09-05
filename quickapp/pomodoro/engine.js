'use strict';

function createPomodoro(options) {
  options = options || {};
  const WORK_MIN = 25;
  const SHORT_MIN = 5;
  const LONG_MIN = 15;
  const ROUNDS = 4;
  let workMin = WORK_MIN;
  let shortMin = SHORT_MIN;
  let longMin = LONG_MIN;
  let phase = 'work';
  let remaining = workMin * 60000;
  let running = false;
  let completed = 0;
  let eventSeq = 0;
  let toast = '';
  let dialog = '';

  function clamp(value, min, max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
  }

  function pad(value) {
    return value < 10 ? '0' + value : '' + value;
  }

  function formatClock(ms) {
    const total = Math.max(0, Math.ceil(ms / 1000));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor(total / 60) % 60;
    const seconds = total % 60;
    if (hours > 0) {
      return pad(hours) + ':' + pad(minutes) + ':' + pad(seconds);
    }
    return pad(Math.floor(total / 60)) + ':' + pad(seconds);
  }

  function phaseName() {
    if (phase === 'short') return '短休息';
    if (phase === 'long') return '长休息';
    return '工作';
  }

  function phaseDuration() {
    if (phase === 'short') return shortMin * 60000;
    if (phase === 'long') return longMin * 60000;
    return workMin * 60000;
  }

  function persist() {
    if (!options.storage) return;
    options.storage.set('pomodoro', JSON.stringify({
      workMin: workMin,
      shortMin: shortMin,
      longMin: longMin,
      completed: completed,
      phase: phase,
      remaining: remaining
    }));
  }

  function emit(nextToast, nextDialog) {
    eventSeq += 1;
    toast = nextToast;
    dialog = nextDialog;
    if (options.toast) options.toast(nextToast);
    if (options.dialog) options.dialog(nextDialog);
  }

  function completePhase() {
    running = false;
    if (phase === 'work') {
      completed += 1;
      if (completed % ROUNDS === 0) {
        phase = 'long';
        remaining = longMin * 60000;
        emit('四个番茄完成，进入长休息', '工作完成，开始长休息。');
      } else {
        phase = 'short';
        remaining = shortMin * 60000;
        emit('工作完成，开始短休息', '本轮工作结束，休息一下。');
      }
    } else {
      phase = 'work';
      remaining = workMin * 60000;
      emit('休息结束，开始工作', '休息结束，开始下一个番茄。');
    }
    persist();
  }

  function getState() {
    return {
      phase: phase,
      running: running,
      remaining: remaining,
      workMin: workMin,
      shortMin: shortMin,
      longMin: longMin,
      completed: completed,
      label: phaseName(),
      clock: formatClock(remaining),
      eventSeq: eventSeq,
      toast: toast,
      dialog: dialog
    };
  }

  function tick() {
    if (!running) return getState();
    remaining -= 1000;
    if (remaining <= 0) {
      remaining = 0;
      completePhase();
    }
    return getState();
  }

  function start() {
    running = true;
    persist();
    return getState();
  }

  function pause() {
    running = false;
    persist();
    return getState();
  }

  function toggle() {
    running = !running;
    persist();
    return getState();
  }

  function skip() {
    remaining = 0;
    completePhase();
    return getState();
  }

  function reset() {
    running = false;
    remaining = phaseDuration();
    persist();
    return getState();
  }

  function addWork(delta) {
    if (running) {
      emit('请先暂停再调整时长', '');
      return getState();
    }
    workMin = clamp(workMin + delta, 1, 60);
    if (phase === 'work') remaining = workMin * 60000;
    persist();
    return getState();
  }

  function addBreak(delta) {
    if (running) {
      emit('请先暂停再调整时长', '');
      return getState();
    }
    shortMin = clamp(shortMin + delta, 1, 30);
    longMin = clamp(shortMin * 3, 3, 45);
    if (phase === 'short') remaining = shortMin * 60000;
    if (phase === 'long') remaining = longMin * 60000;
    persist();
    return getState();
  }

  function sceneWork() {
    phase = 'work';
    remaining = workMin * 60000;
    running = true;
    persist();
    return getState();
  }

  function nudge(ms) {
    if (running) return getState();
    remaining = clamp(remaining + ms, 1000, 99 * 3600000 + 59 * 60000 + 59000);
    persist();
    return getState();
  }

  if (options.storage) {
    const raw = options.storage.get('pomodoro');
    if (raw) {
      try {
        const data = JSON.parse(raw);
        workMin = clamp(data.workMin || WORK_MIN, 1, 60);
        shortMin = clamp(data.shortMin || SHORT_MIN, 1, 30);
        longMin = clamp(data.longMin || LONG_MIN, 1, 45);
        completed = clamp(data.completed || 0, 0, 999);
        if (data.phase === 'short' || data.phase === 'long' ||
            data.phase === 'work') {
          phase = data.phase;
        }
        remaining = clamp(data.remaining || phaseDuration(), 0, phaseDuration());
        running = false;
      } catch (e) {
        workMin = WORK_MIN;
        shortMin = SHORT_MIN;
        longMin = LONG_MIN;
      }
    }
  }

  return {
    start: start,
    pause: pause,
    toggle: toggle,
    skip: skip,
    reset: reset,
    tick: tick,
    addWork: addWork,
    addBreak: addBreak,
    sceneWork: sceneWork,
    nudge: nudge,
    getState: getState
  };
}

if (typeof globalThis !== 'undefined') {
  globalThis.createPomodoro = createPomodoro;
}
