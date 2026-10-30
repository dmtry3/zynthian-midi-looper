# Changelog

## 0.8.0 (2026-10-30)

First public version.

- LV2 MIDI-tool looper locked to the Zynthian (zynseq) transport; launch on beat/bar/loop, fixed length 1-16 bars.
- Rec / Overdub / Play / Stop, up to 8 layers (mute, delete, undo/redo); empty overdub layers are discarded.
- Pre-roll for notes played before the downbeat; notes pressed just before the loop end in overdub go to the loop start.
- Shared controller lane (CC, pitch bend, aftertouch) with touch-overwrite; played CC drive Zynthian MIDI-learn bindings
  (synth and effect parameters, on-screen knobs); the looper's own button CC are never recorded.
- Quantize (1/4-1/32, strength, starts/ends/both) on layers selected on screen; Half/Double; per-layer transposition.
- 16 slots on disk, auto-load on snapshot restore; export to .mid and into a zynseq pattern (4 steps per beat).
- Touch widget: state, progress, beats, coloured layers, piano roll with CC lane, tap-select for layers and slots.
- Stuck-note safety net.
