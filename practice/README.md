# practice/ — exercise libraries

Data, not code: what a practice session is made of. The first library is
`vocal/` (singing); the format is meant to carry others later (an instrument,
pronunciation, posture) — `kind:` says which, and each kind decides what the
browser measures.

The idea the whole feature rests on: **the browser measures, the code scores,
the model coaches.** Pitch is detected in the browser from the microphone; the
audio never leaves the machine you practise on. What reaches Funes is a list of
numbers per note (cents off, time on pitch, stability). A model is good at
choosing today's exercises and saying two useful things about the numbers; it
is not good at writing notes, so exercises are written here by hand and the
model only picks them and their parameters.

The practice view (`ui/practice/`, `/api/practice/*` in
`src/server/practice_api.h`) plays them and scores what is sung; a person's
range and session log live in their workspace (`practice/profile.json`,
`practice/sessions.jsonl`). `src/core/practice.{h,cpp}` loads and validates these files;
`tests/test_practice.cpp` checks every shipped exercise against every voice in
`voices.yaml` (ctest `practice`).

## voices.yaml

Default ranges per voice type, in scientific pitch (C4 = middle C, A4 = 440 Hz).
`full` is what the voice can reach; `comfortable` is its tessitura. They are
starting points: a range calibration in the practice view replaces them with
the singer's own.

## An exercise

```yaml
id: lip_trill_scale          # = the file name
kind: vocal
category: warmup             # breath | warmup | technique | agility | range | cooldown
level: 1                     # 1 beginner … 3 advanced
title:       {es: …, en: …}
description: {es: …, en: …}  # what it is and what it is for, two or three sentences
steps:       {es: [ … ], en: [ … ]}   # how to do it
mistakes:    {es: [ … ], en: [ … ]}   # what goes wrong; the coach's vocabulary
sound: "brr"                 # what is sung: a syllable, vowel, "mm", "ng", "brr"
tempo: 96                    # quarter notes per minute
pattern: "0 2 4 5 7 5 4 2 0:2"
range: comfortable           # comfortable | full — which part of the voice it walks
roots: {step: 1, direction: up_down}   # up | down | up_down
articulation: legato         # legato | staccato
dynamics: steady             # steady | messa_di_voce | soft
measure: [pitch, stability]  # what the session is scored on
```

### pattern

Semitones from the root, space separated, each optionally `:beats` (default 1).
`0 2 4 5 7` is the first five notes of a major scale; `0 4 7 12` an arpeggio to
the octave.

- `a~b:beats` — a glide from `a` to `b` (sirens, sighs).
- `r:beats` — a rest.

### roots

The pattern is sung on a root, then repeated a step higher (or lower). Which
roots are used is computed from the singer's range: the lowest root is the one
whose lowest note sits on the bottom of the range, the highest the one whose
top note sits on its top. `direction: down` starts high and descends (for
exercises that bring head voice down); `up_down` goes up and comes back.

### Unpitched exercises

Breathing exercises have no notes: `pitched: false`, `seconds:` (the target
length of one repetition) and `repeats:`, and are measured on `duration` and
`stability` of the sound's level.

### measure

`pitch` (cents off each note), `stability` (wobble while holding),
`glide` (a slide that is continuous, no jumps), `onset` (time to land on the
note), `duration` (how long a sound is held), `dynamics` (loudness follows the
plan), `range` (lowest and highest notes reached).

## Sources

The warm-up set follows the families most voice pedagogy agrees on — SOVT
(lip/tongue trills, humming, straw), glides across the range, five-note
scales, arpeggios, messa di voce and staccato arpeggios; Ingo Titze, "The five
best vocal warm-up exercises", *Journal of Singing* 57(3), 2001. Exercises
here are the standard forms, written down from scratch.
