/* ui/practice/score.js — scoring a repetition against its notes.
 *
 * The code scores, the model coaches (practice/README.md). Input is what the
 * view recorded — frames {t, freq|null, rms}, t in seconds from the start of
 * the repetition — and the repetition the server rendered (notes with midi,
 * start, seconds). Output is numbers a person can read and a coach can
 * compare across days. Pure functions; tests/ui/test_practice.js runs them.
 */
(function (root) {
  'use strict';

  const P = root.PracticePitch || (typeof require !== 'undefined' ? require('./pitch.js') : null);

  const IN_TUNE_CENTS = 30;    // within this, a note counts as on pitch
  const LANDED_CENTS = 50;     // the first frame this close ends the onset
  const GLIDE_CENTS = 100;     // a slide is followed within a semitone
  const LATENCY = 0.08;        // mic + processing delay, seconds
  const SETTLE = 0.1;          // after landing, before "holding" starts
  const RELEASE = 0.12;        // before the note's end: the move to the next one
  const SOUND_RMS = 0.02;      // unpitched: louder than this is sound
  const GAP = 0.15;            // unpitched: a shorter silence is not a stop

  function median(xs) {
    if (!xs.length) return null;
    const s = [...xs].sort((a, b) => a - b);
    const m = s.length >> 1;
    return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
  }
  function mean(xs) { return xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : null; }
  // Spread that a few stray frames cannot dominate: 1.4826 × median absolute
  // deviation, which equals the standard deviation for a normal wobble.
  function robustSpread(xs) {
    if (xs.length < 3) return null;
    const m = median(xs);
    return 1.4826 * median(xs.map((x) => Math.abs(x - m)));
  }
  function stdev(xs) {
    if (xs.length < 2) return null;
    const m = mean(xs);
    return Math.sqrt(xs.reduce((a, x) => a + (x - m) * (x - m), 0) / (xs.length - 1));
  }
  function round(x, d) {
    if (x === null || x === undefined || Number.isNaN(x)) return null;
    const k = Math.pow(10, d || 0);
    return Math.round(x * k) / k;
  }

  // Cents from the target, with an octave mistake folded back and counted:
  // YIN sometimes reports half or double the frequency of a breathy note, and
  // a person singing the right note an octave off is told so separately.
  function offTarget(freq, target) {
    let c = P.cents(freq, target);
    let octave = false;
    while (Math.abs(c) > 1200 - 150) {
      const folded = c - Math.sign(c) * 1200;
      if (Math.abs(folded) >= Math.abs(c)) break;
      c = folded;
      octave = true;
    }
    return { c, octave };
  }

  function targetAt(note, t) {
    if (note.kind !== 'glide') return note.midi;
    const x = Math.min(1, Math.max(0, (t - note.start) / note.seconds));
    return note.midi + (note.to - note.midi) * x;
  }

  // Loudness following sin(pi x) — soft, loud, soft — as a correlation.
  function dynamicsScore(frames) {
    if (frames.length < 8) return null;
    const t0 = frames[0].t, t1 = frames[frames.length - 1].t;
    const xs = [], ys = [];
    for (const f of frames) {
      if (f.rms <= 0) continue;
      xs.push(Math.sin(Math.PI * (f.t - t0) / (t1 - t0 || 1)));
      ys.push(20 * Math.log10(f.rms));
    }
    const mx = mean(xs), my = mean(ys);
    let sxy = 0, sxx = 0, syy = 0;
    for (let i = 0; i < xs.length; i++) {
      sxy += (xs[i] - mx) * (ys[i] - my);
      sxx += (xs[i] - mx) * (xs[i] - mx);
      syy += (ys[i] - my) * (ys[i] - my);
    }
    if (!sxx || !syy) return 0;
    return Math.max(0, sxy / Math.sqrt(sxx * syy));
  }

  function scoreNote(note, frames, opts) {
    const lat = opts.latency;
    const from = note.start + lat, to = note.start + note.seconds + lat;
    const win = frames.filter((f) => f.t >= from && f.t < to);
    const out = { kind: note.kind, midi: note.midi, frames: win.length };
    if (note.kind === 'glide') out.to = note.to;
    if (!win.length) return { ...out, voiced: 0, in_tune: 0, glide: note.kind === 'glide' ? 0 : undefined };

    const marked = win.map((f) => {
      if (!f.freq) return { t: f.t, rms: f.rms, c: null };
      const o = offTarget(f.freq, targetAt(note, f.t - lat));
      return { t: f.t, rms: f.rms, c: o.c, octave: o.octave };
    });
    const voiced = marked.filter((m) => m.c !== null);
    out.voiced = voiced.length / marked.length;
    out.octave = voiced.filter((m) => m.octave).length > voiced.length / 2;

    if (note.kind === 'glide') {
      out.glide = marked.filter((m) => m.c !== null && Math.abs(m.c) <= GLIDE_CENTS).length / marked.length;
      out.cents = round(median(voiced.map((m) => m.c)));
      return out;
    }

    const landed = marked.find((m) => m.c !== null && Math.abs(m.c) <= LANDED_CENTS);
    out.onset_ms = landed ? Math.round((landed.t - from) * 1000) : null;
    const holdFrom = landed ? landed.t + SETTLE : from + note.seconds * 0.25;
    // A held note is judged between landing and letting go: the transition
    // into the next note is the next note's onset, not this one's wobble.
    const holdTo = Math.max(holdFrom + 0.05, to - Math.min(RELEASE, note.seconds * 0.25));
    const held = marked.filter((m) => m.t >= holdFrom && m.t < holdTo);
    const heldVoiced = held.filter((m) => m.c !== null).map((m) => m.c);
    out.cents = round(median(heldVoiced.length ? heldVoiced : voiced.map((m) => m.c)));
    // Silence counts against: a note not sung is not a note sung in tune.
    out.in_tune = held.length
      ? held.filter((m) => m.c !== null && Math.abs(m.c) <= IN_TUNE_CENTS).length / held.length
      : 0;
    out.stability = round(robustSpread(heldVoiced), 1);
    if (opts.dynamics === 'messa_di_voce') out.dynamics = round(dynamicsScore(win), 2);
    return out;
  }

  // One repetition: per-note detail and the repetition's scores (0–100,
  // except stability in cents of wobble and onset in milliseconds).
  function scoreRep(rep, frames, options) {
    const opts = { latency: LATENCY, ...(options || {}) };
    const notes = rep.notes.map((n) => scoreNote(n, frames, opts));
    const held = notes.filter((n) => n.kind === 'note');
    const glides = notes.filter((n) => n.kind === 'glide');
    const scores = {};
    if (held.length) {
      scores.pitch = Math.round(100 * mean(held.map((n) => n.in_tune)));
      const st = held.map((n) => n.stability).filter((x) => x !== null);
      scores.stability = st.length ? round(mean(st), 1) : null;
      const on = held.map((n) => n.onset_ms).filter((x) => x !== null);
      scores.onset_ms = on.length ? Math.round(median(on)) : null;
      const dyn = held.map((n) => n.dynamics).filter((x) => x !== undefined && x !== null);
      if (dyn.length) scores.dynamics = Math.round(100 * mean(dyn));
    }
    if (glides.length) scores.glide = Math.round(100 * mean(glides.map((n) => n.glide)));
    return { root: rep.root, notes, scores };
  }

  // A breathing exercise: the longest stretch of sound and how even it was.
  function scoreUnpitched(frames, target) {
    let best = null, cur = null, lastSound = -Infinity;
    for (const f of frames) {
      if (f.rms >= SOUND_RMS) {
        if (!cur || f.t - lastSound > GAP) cur = { from: f.t, to: f.t, levels: [] };
        cur.to = f.t;
        cur.levels.push(f.rms);
        lastSound = f.t;
        if (!best || cur.to - cur.from > best.to - best.from) best = cur;
      }
    }
    if (!best) return { scores: { duration_s: 0, duration: 0 } };
    const secs = best.to - best.from;
    const cv = stdev(best.levels) / (mean(best.levels) || 1);
    return {
      scores: {
        duration_s: round(secs, 1),
        duration: Math.round(100 * Math.min(1, secs / (target || secs || 1))),
        stability: best.levels.length > 2 ? Math.round(100 * Math.max(0, 1 - cv)) : null,
      },
    };
  }

  // The lowest and highest notes held steadily (≥ minSeconds within half a
  // semitone) — the range check. A note brushed on the way past is not range.
  function rangeFromFrames(frames, minSeconds) {
    const hold = minSeconds || 0.3;
    const held = [];
    let seg = [];
    const close = () => {
      if (seg.length > 1 && seg[seg.length - 1].t - seg[0].t >= hold)
        held.push(median(seg.map((s) => s.m)));
      seg = [];
    };
    for (const f of frames) {
      if (!f.freq) { close(); continue; }
      const m = P.freqToMidi(f.freq);
      if (seg.length && (Math.abs(m - seg[0].m) > 0.5 || f.t - seg[seg.length - 1].t > 0.1)) close();
      seg.push({ t: f.t, m });
    }
    close();
    if (!held.length) return null;
    return { low: Math.round(Math.min(...held)), high: Math.round(Math.max(...held)) };
  }

  // Many repetitions → the session's scores, averaged.
  function summarize(reps) {
    const keys = ['pitch', 'stability', 'onset_ms', 'glide', 'dynamics', 'duration', 'duration_s'];
    const out = {};
    for (const k of keys) {
      const xs = reps.map((r) => r.scores[k]).filter((x) => x !== undefined && x !== null);
      if (xs.length) out[k] = k === 'stability' || k === 'duration_s' ? round(mean(xs), 1) : Math.round(mean(xs));
    }
    return out;
  }

  const api = { scoreRep, scoreUnpitched, rangeFromFrames, summarize,
                IN_TUNE_CENTS, LATENCY };
  root.PracticeScore = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
