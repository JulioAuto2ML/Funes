/* ui/practice/view.js — the practice view.
 *
 * Plays an exercise's reference notes, listens through the microphone, draws
 * the target notes and the voice over them, and scores each repetition
 * (score.js). Only the scores are sent to Funes (/api/practice/sessions); the
 * audio stays in this browser. getUserMedia needs a secure origin: localhost,
 * or HTTPS (yoda is served on the tailnet by `tailscale serve`).
 *
 * Uses app.js's t() and LOCALE, so it loads after app.js.
 */
(function () {
  'use strict';

  const P = window.PracticePitch;
  const S = window.PracticeScore;
  const $ = (id) => document.getElementById(id);

  const FRAME_MS = 33;          // ~30 pitch readings a second
  const TAIL = 0.4;             // keep listening this long after the last note
  const MEASURE_SECONDS = 8;    // each half of the range check
  const CATEGORIES = ['breath', 'warmup', 'technique', 'agility', 'range', 'cooldown'];

  const state = {
    library: null,
    profile: null,
    current: null,      // exercise id
    plan: null,         // {exercise, range, reps}
    running: false,
    stop: false,
  };
  const audio = { ctx: null, stream: null, analyser: null, buf: null };

  const loc = () => (typeof LOCALE !== 'undefined' ? LOCALE : 'en');
  const tr = (obj) => (obj && (obj[loc()] || obj.en)) || '';
  const noteLabel = (name) => P.noteName(P.nameToMidi(name), loc());
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  function el(tag, attrs, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') e.className = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else if (v !== undefined && v !== null) e.setAttribute(k, v);
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      e.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return e;
  }

  async function api(path, opts) {
    const res = await fetch('/api/practice/' + path, {
      headers: { 'Content-Type': 'application/json' },
      ...(opts || {}),
    });
    const body = await res.json().catch(() => ({ ok: false, error: res.statusText }));
    return { status: res.status, ...body };
  }

  /* ── audio ──────────────────────────────────────────────────────────── */

  async function ensureContext() {
    if (!audio.ctx) audio.ctx = new (window.AudioContext || window.webkitAudioContext)();
    if (audio.ctx.state === 'suspended') await audio.ctx.resume();
    return audio.ctx;
  }

  async function ensureMic() {
    if (audio.analyser) return;
    if (!window.isSecureContext || !navigator.mediaDevices) throw new Error(t('practice.micInsecure'));
    const ctx = await ensureContext();
    // The browser's voice processing bends pitch and flattens loudness:
    // off, so what is scored is what was sung.
    audio.stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    });
    const src = ctx.createMediaStreamSource(audio.stream);
    audio.analyser = ctx.createAnalyser();
    audio.analyser.fftSize = 2048;
    src.connect(audio.analyser);
    audio.buf = new Float32Array(audio.analyser.fftSize);
  }

  function releaseMic() {
    if (audio.stream) audio.stream.getTracks().forEach((tr) => tr.stop());
    audio.stream = null;
    audio.analyser = null;
  }

  function readFrame() {
    audio.analyser.getFloatTimeDomainData(audio.buf);
    const r = P.yin(audio.buf, audio.ctx.sampleRate);
    return { freq: r ? r.freq : null, rms: P.rms(audio.buf) };
  }

  function tone(ctx, at, seconds, f0, f1, gain) {
    const osc = ctx.createOscillator();
    const g = ctx.createGain();
    osc.type = 'triangle';
    osc.frequency.setValueAtTime(f0, at);
    if (f1) osc.frequency.exponentialRampToValueAtTime(f1, at + seconds);
    const peak = gain || 0.25;
    g.gain.setValueAtTime(0, at);
    g.gain.linearRampToValueAtTime(peak, at + 0.02);
    g.gain.setValueAtTime(peak, Math.max(at + 0.02, at + seconds - 0.05));
    g.gain.linearRampToValueAtTime(0, at + seconds);
    osc.connect(g).connect(ctx.destination);
    osc.start(at);
    osc.stop(at + seconds + 0.02);
  }

  function playRep(ctx, rep, at) {
    for (const n of rep.notes) {
      tone(ctx, at + n.start, n.seconds, P.midiToFreq(n.midi),
           n.kind === 'glide' ? P.midiToFreq(n.to) : null);
    }
  }

  function countIn(ctx, at, beat, beats) {
    for (let i = 0; i < beats; i++) tone(ctx, at + i * beat, 0.05, 1500, null, 0.15);
  }

  // Frames from `at` (context time) for `seconds`; onFrame sees each one and
  // may end the recording early by returning true.
  async function record(at, seconds, onFrame) {
    const frames = [];
    while (audio.ctx.currentTime < at) await sleep(5);
    while (!state.stop) {
      const tt = audio.ctx.currentTime - at;
      if (tt > seconds) break;
      const f = { t: tt, ...readFrame() };
      frames.push(f);
      if (onFrame && onFrame(f, frames) === true) break;
      await sleep(FRAME_MS);
    }
    return frames;
  }

  /* ── drawing ────────────────────────────────────────────────────────── */

  function color(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  function draw(canvas, rep, frames, now) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== w * dpr) { canvas.width = w * dpr; canvas.height = h * dpr; }
    const g = canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    if (!rep) return;

    const midis = rep.notes.flatMap((n) => (n.kind === 'glide' ? [n.midi, n.to] : [n.midi]));
    const lo = Math.min(...midis) - 3, hi = Math.max(...midis) + 3;
    const span = rep.seconds + TAIL;
    const x = (tt) => 8 + (tt / span) * (w - 16);
    const y = (m) => h - 8 - ((m - lo) / (hi - lo)) * (h - 16);
    const lat = S.LATENCY;

    // Semitone grid, with C labelled.
    g.font = '11px ' + color('--sans');
    for (let m = Math.ceil(lo); m <= hi; m++) {
      g.strokeStyle = color('--border');
      g.globalAlpha = m % 12 === 0 ? 1 : 0.4;
      g.beginPath(); g.moveTo(0, y(m)); g.lineTo(w, y(m)); g.stroke();
      if (m % 12 === 0 || m === Math.ceil(lo)) {
        g.fillStyle = color('--text-dim');
        g.fillText(P.noteName(m, loc()), 2, y(m) - 2);
      }
    }
    g.globalAlpha = 1;

    // Target notes: a band ±IN_TUNE_CENTS around each, glides as a line.
    const band = S.IN_TUNE_CENTS / 100;
    g.fillStyle = color('--accent-soft');
    g.strokeStyle = color('--accent');
    for (const n of rep.notes) {
      if (n.kind === 'glide') {
        g.lineWidth = 10; g.globalAlpha = 0.35;
        g.beginPath(); g.moveTo(x(n.start + lat), y(n.midi)); g.lineTo(x(n.start + n.seconds + lat), y(n.to)); g.stroke();
        g.globalAlpha = 1; g.lineWidth = 1;
      } else {
        const top = y(n.midi + band), bottom = y(n.midi - band);
        g.fillRect(x(n.start + lat), top, x(n.start + n.seconds + lat) - x(n.start + lat) - 2, bottom - top);
        g.strokeRect(x(n.start + lat), top, x(n.start + n.seconds + lat) - x(n.start + lat) - 2, bottom - top);
      }
    }

    // The voice.
    for (const f of frames || []) {
      if (!f.freq) continue;
      const m = P.freqToMidi(f.freq);
      if (m < lo - 1 || m > hi + 1) continue;
      const target = rep.notes.find((n) => f.t >= n.start + lat && f.t < n.start + n.seconds + lat);
      let ok = false;
      if (target) {
        const want = target.kind === 'glide'
          ? target.midi + (target.to - target.midi) * (f.t - lat - target.start) / target.seconds
          : target.midi;
        ok = Math.abs(m - want) * 100 <= (target.kind === 'glide' ? 100 : S.IN_TUNE_CENTS);
      }
      g.fillStyle = ok ? color('--ok') : color('--error');
      g.beginPath(); g.arc(x(f.t), y(m), 2.5, 0, 2 * Math.PI); g.fill();
    }

    if (now !== undefined) {
      g.strokeStyle = color('--text-dim');
      g.beginPath(); g.moveTo(x(now), 0); g.lineTo(x(now), h); g.stroke();
    }
  }

  /* ── views ──────────────────────────────────────────────────────────── */

  function rangeText() {
    const p = state.profile;
    if (!p) return t('practice.noRange');
    const range = t('practice.range', { low: noteLabel(p.full[0]), high: noteLabel(p.full[1]) });
    return p.source === 'measured'
      ? range + ' · ' + t('practice.measured', { date: (p.measured_at || '').slice(0, 10) })
      : range + ' · ' + t('practice.voice.' + p.voice);
  }

  function renderHead() {
    $('practice-range').textContent = rangeText();
    const sel = $('practice-voice');
    sel.replaceChildren(el('option', { value: '' }, t('practice.chooseVoice')),
      ...state.library.voices.map((v) => el('option', { value: v.voice }, t('practice.voice.' + v.voice))));
    sel.value = state.profile && state.profile.source === 'default' ? state.profile.voice : '';
  }

  function renderList() {
    const nav = $('practice-list');
    nav.replaceChildren();
    for (const cat of CATEGORIES) {
      const items = state.library.exercises.filter((e) => e.category === cat);
      if (!items.length) continue;
      nav.append(el('h4', {}, t('practice.category.' + cat)));
      for (const e of items) {
        nav.append(el('button', {
          class: 'practice-item' + (e.id === state.current ? ' active' : ''),
          onclick: () => open(e.id),
        }, tr(e.title), el('span', { class: 'practice-level' }, '·'.repeat(e.level))));
      }
    }
  }

  function exerciseHeader(ex) {
    return [
      el('h3', {}, tr(ex.title)),
      el('p', {}, tr(ex.description)),
      el('p', { class: 'practice-sound' }, t('practice.sound', { sound: ex.sound })),
      el('ol', {}, (ex.steps[loc()] || ex.steps.en).map((s) => el('li', {}, s))),
      el('details', {}, el('summary', {}, t('practice.mistakes')),
        el('ul', {}, (ex.mistakes[loc()] || ex.mistakes.en).map((s) => el('li', {}, s)))),
    ];
  }

  function showError(where, message) {
    where.replaceChildren(el('p', { class: 'practice-error' }, message));
  }

  async function open(id) {
    if (state.running) return;
    state.current = id;
    renderList();
    const main = $('practice-main');
    const ex = state.library.exercises.find((e) => e.id === id);
    if (ex.category === 'range' && !ex.pitched) return renderMeasure();
    if (!ex.pitched) return renderUnpitched(ex);
    if (!state.profile) {
      main.replaceChildren(...exerciseHeader(ex), el('p', { class: 'practice-error' }, t('practice.noRange')));
      return;
    }
    const r = await api('exercises/' + id);
    if (!r.ok) return showError(main, r.error);
    state.plan = r;
    renderPitched(ex);
  }

  function renderPitched(ex) {
    const main = $('practice-main');
    const reps = state.plan.reps;
    const status = el('p', { class: 'practice-status' });
    const live = el('span', { class: 'practice-live' });
    const canvas = el('canvas', { class: 'practice-canvas' });
    const results = el('div', { class: 'practice-results' });
    const mode = el('select', {},
      el('option', { value: 'response' }, t('practice.mode.response')),
      el('option', { value: 'together' }, t('practice.mode.together')));
    const count = el('select', {},
      ...[3, 5, 10].filter((n) => n < reps.length).map((n) =>
        el('option', { value: n }, t('practice.repsN', { n }))),
      el('option', { value: reps.length }, t('practice.repsAll', { n: reps.length })));
    // A long walk (an up-and-down over a whole range is 20-odd) starts shorter.
    count.value = String(reps.length > 12 ? 10 : reps.length);
    const listen = el('button', { class: 'ghost' }, t('practice.listen'));
    const start = el('button', {}, t('practice.start'));
    const stop = el('button', { class: 'ghost', hidden: 'hidden' }, t('practice.stop'));

    if (!reps.length) {
      main.replaceChildren(...exerciseHeader(ex), el('p', { class: 'practice-error' }, t('practice.noReps')));
      return;
    }

    listen.onclick = async () => {
      const ctx = await ensureContext();
      playRep(ctx, reps[0], ctx.currentTime + 0.05);
    };
    stop.onclick = () => { state.stop = true; };
    start.onclick = async () => {
      try {
        await ensureMic();
      } catch (e) {
        status.textContent = e.name === 'NotAllowedError' ? t('practice.micDenied') : e.message;
        return;
      }
      state.running = true; state.stop = false;
      start.hidden = true; listen.hidden = true; stop.hidden = false;
      try {
        await runPitched(ex, reps.slice(0, Number(count.value)), mode.value, { status, live, canvas, results });
      } finally {
        state.running = false;
        start.hidden = false; listen.hidden = false; stop.hidden = true;
        live.textContent = '';
        releaseMic();
      }
    };

    main.replaceChildren(
      ...exerciseHeader(ex),
      el('p', { class: 'practice-dim' }, t('practice.plan', {
        n: reps.length, low: noteLabel(reps[0].root), high: noteLabel(reps.reduce((a, r) => (r.root_midi > a.root_midi ? r : a)).root),
      })),
      el('div', { class: 'practice-controls' }, mode, count, listen, start, stop, live),
      status, canvas, results);
    draw(canvas, reps[0], [], undefined);
  }

  async function runPitched(ex, reps, mode, ui) {
    const ctx = await ensureContext();
    const beat = 60 / (ex.tempo || 60);
    const done = [];
    for (let i = 0; i < reps.length && !state.stop; i++) {
      const rep = reps[i];
      ui.status.textContent = t('practice.rep', { n: i + 1, total: reps.length, root: noteLabel(rep.root) });
      let at = ctx.currentTime + 0.15;
      if (mode === 'response') {
        ui.live.textContent = t('practice.listening');
        playRep(ctx, rep, at);
        draw(ui.canvas, rep, [], undefined);
        at += rep.seconds + beat * 0.5;
      }
      countIn(ctx, at, beat, 2);
      at += 2 * beat;
      if (mode === 'together') playRep(ctx, rep, at);
      const frames = await record(at, rep.seconds + TAIL, (f, all) => {
        ui.live.textContent = f.freq ? P.noteName(P.freqToMidi(f.freq), loc()) : '·';
        draw(ui.canvas, rep, all, f.t);
      });
      if (state.stop) break;
      const scored = S.scoreRep(rep, frames, { dynamics: ex.dynamics });
      done.push(scored);
      draw(ui.canvas, rep, frames, undefined);
      renderResults(ui.results, ex, done);
    }
    if (done.length) await save(ex, done, ui.status, { mode });
    else ui.status.textContent = '';
  }

  const COLUMNS = {
    pitch: (s) => (s.pitch !== undefined ? s.pitch + '%' : ''),
    stability: (s) => (s.stability !== null && s.stability !== undefined ? '±' + s.stability + '¢' : ''),
    onset: (s) => (s.onset_ms !== null && s.onset_ms !== undefined ? s.onset_ms + ' ms' : ''),
    glide: (s) => (s.glide !== undefined ? s.glide + '%' : ''),
    dynamics: (s) => (s.dynamics !== undefined ? s.dynamics + '%' : ''),
    duration: (s) => (s.duration_s !== undefined ? s.duration_s + ' s' : ''),
  };

  function renderResults(where, ex, reps) {
    const cols = Object.keys(COLUMNS).filter((k) => ex.measure.includes(k) ||
      (k === 'stability' && ex.measure.includes('pitch')));
    const sum = S.summarize(reps);
    const row = (label, s) => el('tr', {}, el('td', {}, label), ...cols.map((k) => el('td', {}, COLUMNS[k](s))));
    const worst = (rep) => {
      const off = rep.notes.filter((n) => n.kind === 'note' && n.cents !== null && Math.abs(n.cents) > S.IN_TUNE_CENTS);
      if (!off.length) return '';
      const n = off.reduce((a, b) => (Math.abs(b.cents) > Math.abs(a.cents) ? b : a));
      return P.noteName(n.midi, loc()) + ' ' + (n.cents > 0 ? '+' : '') + n.cents + '¢';
    };
    where.replaceChildren(el('table', {},
      el('thead', {}, el('tr', {}, el('th', {}, t('practice.col.root')),
        ...cols.map((k) => el('th', {}, t('practice.col.' + k))), el('th', {}, t('practice.col.worst')))),
      el('tbody', {},
        reps.map((r, i) => {
          const tr_ = row(r.root ? noteLabel(r.root) : '#' + (i + 1), r.scores);
          tr_.append(el('td', {}, r.notes ? worst(r) : ''));
          return tr_;
        }),
        (() => { const s = row(t('practice.col.total'), sum); s.className = 'practice-total'; s.append(el('td')); return s; })())));
  }

  async function save(ex, reps, status, extra) {
    const body = {
      exercise: ex.id,
      ...extra,
      range: state.plan ? state.plan.range : null,
      scores: S.summarize(reps),
      reps: reps.map((r) => ({
        root: r.root,
        scores: r.scores,
        notes: (r.notes || []).map((n) => ({ midi: n.midi, kind: n.kind, cents: n.cents,
          in_tune: n.in_tune !== undefined ? Math.round(n.in_tune * 100) : undefined,
          octave: n.octave || undefined })),
      })),
    };
    const r = await api('sessions', { method: 'POST', body: JSON.stringify(body) });
    status.textContent = r.ok ? t('practice.saved') : t('practice.saveFailed', { error: r.error || r.status });
  }

  function renderUnpitched(ex) {
    const main = $('practice-main');
    const status = el('p', { class: 'practice-status' });
    const meter = el('div', { class: 'practice-meter' }, el('div'));
    const results = el('div', { class: 'practice-results' });
    const start = el('button', {}, t('practice.start'));
    const stop = el('button', { class: 'ghost', hidden: 'hidden' }, t('practice.stop'));
    stop.onclick = () => { state.stop = true; };
    start.onclick = async () => {
      try { await ensureMic(); } catch (e) {
        status.textContent = e.name === 'NotAllowedError' ? t('practice.micDenied') : e.message;
        return;
      }
      state.running = true; state.stop = false; start.hidden = true; stop.hidden = false;
      const done = [];
      try {
        for (let i = 0; i < ex.repeats && !state.stop; i++) {
          status.textContent = t('practice.hiss', { n: i + 1, total: ex.repeats, seconds: ex.seconds, sound: ex.sound });
          const frames = await record(audio.ctx.currentTime + 0.1, ex.seconds + 10, (f, all) => {
            meter.firstChild.style.width = Math.min(100, f.rms * 400) + '%';
            const s = S.scoreUnpitched(all, ex.seconds).scores;
            meter.title = s.duration_s + ' s';
            // Stop listening a second after a long sound has ended.
            const last = all.slice(-30);
            return s.duration_s > 2 && last.length === 30 && last.every((x) => x.rms < 0.02);
          });
          if (state.stop) break;
          done.push(S.scoreUnpitched(frames, ex.seconds));
          renderResults(results, ex, done);
          if (i + 1 < ex.repeats) { status.textContent = t('practice.rest'); await sleep(4000); }
        }
        if (done.length) await save(ex, done, status, {});
      } finally {
        state.running = false; start.hidden = false; stop.hidden = true; releaseMic();
        meter.firstChild.style.width = '0';
      }
    };
    main.replaceChildren(...exerciseHeader(ex),
      el('div', { class: 'practice-controls' }, start, stop), status, meter, results);
  }

  // The range check: slide down and hold, slide up and hold; the lowest and
  // highest steadily held notes are the range (score.js rangeFromFrames).
  function renderMeasure() {
    const ex = state.library.exercises.find((e) => e.id === 'range_check');
    const main = $('practice-main');
    const status = el('p', { class: 'practice-status' });
    const live = el('span', { class: 'practice-live' });
    const result = el('div');
    const found = { low: null, high: null };

    const step = (which) => async () => {
      try { await ensureMic(); } catch (e) {
        status.textContent = e.name === 'NotAllowedError' ? t('practice.micDenied') : e.message;
        return;
      }
      state.running = true; state.stop = false;
      status.textContent = t(which === 'low' ? 'practice.measureLow' : 'practice.measureHigh', { seconds: MEASURE_SECONDS });
      const frames = await record(audio.ctx.currentTime + 0.1, MEASURE_SECONDS, (f) => {
        live.textContent = f.freq ? P.noteName(P.freqToMidi(f.freq), loc()) : '·';
      });
      state.running = false; releaseMic(); live.textContent = '';
      const r = S.rangeFromFrames(frames);
      if (!r) { status.textContent = t('practice.measureNone'); return; }
      found[which] = which === 'low' ? r.low : r.high;
      status.textContent = '';
      showResult();
    };

    const showResult = () => {
      const parts = [];
      if (found.low !== null) parts.push(t('practice.lowest', { note: P.noteName(found.low, loc()) }));
      if (found.high !== null) parts.push(t('practice.highest', { note: P.noteName(found.high, loc()) }));
      const save = el('button', {
        onclick: async () => {
          const r = await api('profile', { method: 'PUT', body: JSON.stringify({
            full: [P.noteName(found.low), P.noteName(found.high)], source: 'measured' }) });
          if (!r.ok) { status.textContent = r.error; return; }
          state.profile = r.profile;
          renderHead();
          status.textContent = t('practice.measureSaved');
        },
      }, t('practice.measureSave'));
      result.replaceChildren(el('p', {}, parts.join(' · ')),
        found.low !== null && found.high !== null ? save : '');
    };

    main.replaceChildren(...exerciseHeader(ex),
      el('div', { class: 'practice-controls' },
        el('button', { onclick: step('low') }, t('practice.measureLowButton')),
        el('button', { onclick: step('high') }, t('practice.measureHighButton')),
        live),
      status, result);
  }

  /* ── open / close ───────────────────────────────────────────────────── */

  async function show() {
    const view = $('practice-view');
    view.hidden = false;
    view.parentElement.classList.add('practicing');
    const [lib, prof] = await Promise.all([api('library'), api('profile')]);
    if (!lib.ok) return showError($('practice-main'), lib.error);
    state.library = lib;
    state.profile = prof.ok ? prof.profile : null;
    renderHead();
    renderList();
    if (state.current) open(state.current);
    else $('practice-main').replaceChildren(el('p', { class: 'practice-dim' },
      state.profile ? t('practice.pick') : t('practice.noRange')));
  }

  function hide() {
    state.stop = true;
    const view = $('practice-view');
    view.hidden = true;
    view.parentElement.classList.remove('practicing');
  }

  $('toggle-practice').addEventListener('click', () => ($('practice-view').hidden ? show() : hide()));
  $('practice-close').addEventListener('click', hide);
  $('practice-measure').addEventListener('click', () => { if (!state.running) { state.current = 'range_check'; renderList(); renderMeasure(); } });
  $('practice-voice').addEventListener('change', async (e) => {
    if (!e.target.value) return;
    const r = await api('profile', { method: 'PUT', body: JSON.stringify({ voice: e.target.value }) });
    if (!r.ok) return;
    state.profile = r.profile;
    renderHead();
    if (state.current) open(state.current);
  });
})();
