// tests/ui/test_practice.js — pitch detection and scoring of the practice view
// (ui/practice/pitch.js, ui/practice/score.js), under node, no browser.
// Synthetic sines stand in for a voice; synthetic frames for a recording.
'use strict';

const path = require('path');
const assert = require('assert');
const P = require(path.join(__dirname, '../../ui/practice/pitch.js'));
const S = require(path.join(__dirname, '../../ui/practice/score.js'));

const SR = 48000;
let failures = 0;
function test(name, fn) {
  try { fn(); console.log('  ok: ' + name); }
  catch (e) { failures++; console.log('  FAIL: ' + name + ' — ' + e.message); }
}

function sine(freq, n, amp, harmonics) {
  const b = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let x = Math.sin(2 * Math.PI * freq * i / SR);
    // A voice is not a sine: give it overtones, the case YIN's octave errors come from.
    if (harmonics) x += 0.5 * Math.sin(4 * Math.PI * freq * i / SR) + 0.3 * Math.sin(6 * Math.PI * freq * i / SR);
    b[i] = (amp || 0.3) * x;
  }
  return b;
}

test('note names round-trip', () => {
  assert.strictEqual(P.nameToMidi('A4'), 69);
  assert.strictEqual(P.nameToMidi('G2'), 43);
  assert.strictEqual(P.nameToMidi('Bb3'), 58);
  assert.ok(Number.isNaN(P.nameToMidi('H2')));
  assert.strictEqual(P.noteName(60), 'C4');
  assert.strictEqual(P.noteName(67, 'es'), 'Sol4');
  assert.strictEqual(P.noteName(61, 'es'), 'Do#4');
});

test('yin finds a baritone range of pitches within 5 cents', () => {
  for (const midi of [43, 45, 52, 57, 62, 67]) {          // G2 … G4
    const f = P.midiToFreq(midi);
    const r = P.yin(sine(f, 2048, 0.3, true), SR);
    assert.ok(r, 'no pitch for ' + midi);
    assert.ok(Math.abs(P.cents(r.freq, midi)) < 5, midi + ': ' + P.cents(r.freq, midi).toFixed(1) + ' cents');
  }
});

test('yin hears silence and noise as no pitch', () => {
  assert.strictEqual(P.yin(new Float32Array(2048), SR), null);
  const noise = new Float32Array(2048).map(() => (Math.random() - 0.5) * 0.6);
  assert.strictEqual(P.yin(noise, SR), null);
});

// Frames as the view records them: ~30 per second.
function frames(seconds, fn) {
  const out = [];
  for (let t = 0; t < seconds; t += 1 / 30) out.push({ t, ...fn(t) });
  return out;
}
const rep = {
  root: 'A2',
  notes: [
    { kind: 'note', midi: 45, start: 0, seconds: 1 },
    { kind: 'note', midi: 47, start: 1, seconds: 1 },
  ],
};
const at = (midi, cents) => ({ freq: P.midiToFreq(midi + (cents || 0) / 100), rms: 0.1 });

test('a rep sung in tune scores high', () => {
  const lat = S.LATENCY;
  const r = S.scoreRep(rep, frames(2.2, (t) => at(t - lat < 1 ? 45 : 47, 5)));
  assert.ok(r.scores.pitch >= 85, 'pitch ' + r.scores.pitch);
  assert.ok(r.scores.stability < 5, 'stability ' + r.scores.stability);
  assert.strictEqual(r.notes[0].cents, 5);
});

test('a clean rep with less latency than assumed has no wobble', () => {
  // The browser's real delay is not LATENCY: the next note arrives while the
  // window still says "this one". That is a transition, not an unsteady voice.
  const r = S.scoreRep(rep, frames(2.2, (t) => at(t < 1 ? 45 : 47)));
  assert.ok(r.scores.stability < 5, 'stability ' + r.scores.stability);
  assert.ok(r.scores.pitch >= 85, 'pitch ' + r.scores.pitch);
});

test('a flat rep scores low and says how flat', () => {
  const lat = S.LATENCY;
  const r = S.scoreRep(rep, frames(2.2, (t) => at(t - lat < 1 ? 45 : 47, -60)));
  assert.ok(r.scores.pitch <= 10, 'pitch ' + r.scores.pitch);
  assert.strictEqual(r.notes[1].cents, -60);
});

test('silence is not singing in tune', () => {
  const r = S.scoreRep(rep, frames(2.2, () => ({ freq: null, rms: 0 })));
  assert.strictEqual(r.scores.pitch, 0);
});

test('an octave off is folded and flagged', () => {
  const lat = S.LATENCY;
  const r = S.scoreRep(rep, frames(2.2, (t) => at((t - lat < 1 ? 45 : 47) + 12, 0)));
  assert.ok(r.notes[0].octave, 'octave flag');
  assert.strictEqual(r.notes[0].cents, 0);
});

test('a glide followed scores high, steps do not', () => {
  const g = { root: 'A2', notes: [{ kind: 'glide', midi: 45, to: 57, start: 0, seconds: 2 }] };
  const lat = S.LATENCY;
  const smooth = S.scoreRep(g, frames(2.2, (t) => at(45 + 12 * Math.min(1, Math.max(0, t - lat) / 2))));
  assert.ok(smooth.scores.glide >= 90, 'glide ' + smooth.scores.glide);
  const stepped = S.scoreRep(g, frames(2.2, (t) => at(t - lat < 1 ? 45 : 57)));
  assert.ok(stepped.scores.glide < 60, 'stepped ' + stepped.scores.glide);
});

test('messa di voce follows the swell', () => {
  const m = { root: 'A2', notes: [{ kind: 'note', midi: 45, start: 0, seconds: 4 }] };
  const lat = S.LATENCY;
  const swell = frames(4.2, (t) => ({ ...at(45), rms: 0.02 + 0.2 * Math.sin(Math.PI * Math.min(1, Math.max(0, t - lat) / 4)) }));
  const flat = frames(4.2, (t) => ({ ...at(45), rms: 0.1 + 0.01 * Math.sin(40 * t) }));
  assert.ok(S.scoreRep(m, swell, { dynamics: 'messa_di_voce' }).scores.dynamics >= 80);
  assert.ok(S.scoreRep(m, flat, { dynamics: 'messa_di_voce' }).scores.dynamics < 50);
});

test('unpitched: the longest even sound', () => {
  const r = S.scoreUnpitched(frames(15, (t) => ({ freq: null, rms: t > 2 && t < 14 ? 0.1 : 0 })), 20);
  assert.ok(Math.abs(r.scores.duration_s - 12) < 0.2, String(r.scores.duration_s));
  assert.strictEqual(r.scores.duration, 60);
  assert.ok(r.scores.stability >= 95);
});

test('range: held notes count, a passing slide does not', () => {
  // Hold G2 half a second, slide fast up to E4, hold E4 half a second.
  const f = frames(2, (t) => {
    if (t < 0.5) return at(43);
    if (t < 1.5) return at(43 + (64 - 43) * (t - 0.5));
    return at(64);
  });
  const r = S.rangeFromFrames(f);
  assert.deepStrictEqual(r, { low: 43, high: 64 });
  assert.strictEqual(S.rangeFromFrames(frames(1, () => ({ freq: null, rms: 0 }))), null);
});

test('summarize averages reps', () => {
  const s = S.summarize([{ scores: { pitch: 80, stability: 10 } }, { scores: { pitch: 60, stability: 20 } }]);
  assert.deepStrictEqual(s, { pitch: 70, stability: 15 });
});

if (failures) { console.log(failures + ' failed'); process.exit(1); }
console.log('test_practice.js: all passed');
