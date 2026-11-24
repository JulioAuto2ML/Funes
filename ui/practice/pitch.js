/* ui/practice/pitch.js — pitch detection for the practice view.
 *
 * YIN (de Cheveigné & Kawahara, 2002) over one window of microphone samples.
 * Pure functions, no DOM: the view feeds it an AnalyserNode's time-domain
 * buffer, and tests/ui/test_practice.js feeds it synthetic sines. The audio
 * never leaves the browser; only the scores computed from these numbers do.
 */
(function (root) {
  'use strict';

  // A voice's fundamental: 60 Hz (below a bass's B1) to 1100 Hz (above C6).
  const MIN_FREQ = 60;
  const MAX_FREQ = 1100;
  // YIN's absolute threshold: lower is stricter. 0.15 is the paper's value
  // for clean monophonic input; breathy singing sits a little above it.
  const THRESHOLD = 0.15;
  // Below this RMS the frame is silence, whatever YIN says about it.
  const SILENCE_RMS = 0.01;

  function rms(buf) {
    let s = 0;
    for (let i = 0; i < buf.length; i++) s += buf[i] * buf[i];
    return Math.sqrt(s / buf.length);
  }

  // {freq, clarity} or null for silence / no clear pitch.
  function yin(buf, sampleRate, opts) {
    const o = opts || {};
    const minFreq = o.minFreq || MIN_FREQ;
    const maxFreq = o.maxFreq || MAX_FREQ;
    if (rms(buf) < (o.silence || SILENCE_RMS)) return null;

    const tauMin = Math.max(2, Math.floor(sampleRate / maxFreq));
    const tauMax = Math.min(Math.floor(sampleRate / minFreq), Math.floor(buf.length / 2));
    const w = buf.length - tauMax;
    if (w < tauMax / 2) return null;

    // Difference function and its cumulative mean normalization, together.
    const d = new Float32Array(tauMax + 1);
    d[0] = 1;
    let running = 0;
    for (let tau = 1; tau <= tauMax; tau++) {
      let s = 0;
      for (let i = 0; i < w; i++) {
        const x = buf[i] - buf[i + tau];
        s += x * x;
      }
      running += s;
      d[tau] = running > 0 ? (s * tau) / running : 1;
    }

    // First dip under the threshold, then down to its local minimum.
    let tau = -1;
    for (let t = tauMin; t <= tauMax; t++) {
      if (d[t] < (o.threshold || THRESHOLD)) {
        while (t + 1 <= tauMax && d[t + 1] < d[t]) t++;
        tau = t;
        break;
      }
    }
    if (tau < 0) return null;

    // Parabolic interpolation around the minimum for sub-sample precision.
    let better = tau;
    if (tau > 1 && tau < tauMax) {
      const a = d[tau - 1], b = d[tau], c = d[tau + 1];
      const den = a + c - 2 * b;
      if (den !== 0) better = tau + (a - c) / (2 * den);
    }
    return { freq: sampleRate / better, clarity: 1 - d[tau] };
  }

  function freqToMidi(f) { return 69 + 12 * Math.log2(f / 440); }
  function midiToFreq(m) { return 440 * Math.pow(2, (m - 69) / 12); }
  // Cents from `midi` (fractional allowed) to the frequency `f`.
  function cents(f, midi) { return 100 * (freqToMidi(f) - midi); }

  const SHARP = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  const SOLFEGE = ['Do', 'Do#', 'Re', 'Re#', 'Mi', 'Fa', 'Fa#', 'Sol', 'Sol#', 'La', 'La#', 'Si'];
  // 60 → "C4" (en) or "Do4" (es): Spanish speakers read solfège.
  function noteName(midi, locale) {
    const m = Math.round(midi);
    const names = locale === 'es' ? SOLFEGE : SHARP;
    return names[((m % 12) + 12) % 12] + (Math.floor(m / 12) - 1);
  }
  // "C4", "Bb3", "F#2" → MIDI, or NaN.
  function nameToMidi(name) {
    const m = /^([A-Ga-g])([#b]?)(-?\d)$/.exec(name || '');
    if (!m) return NaN;
    const base = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[m[1].toUpperCase()];
    const acc = m[2] === '#' ? 1 : m[2] === 'b' ? -1 : 0;
    return (Number(m[3]) + 1) * 12 + base + acc;
  }

  const api = { rms, yin, freqToMidi, midiToFreq, cents, noteName, nameToMidi,
                MIN_FREQ, MAX_FREQ, SILENCE_RMS };
  root.PracticePitch = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
