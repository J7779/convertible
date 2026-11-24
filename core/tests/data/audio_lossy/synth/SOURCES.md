# MIDI and tracker-module fixtures

Written by `gen_synth.py` (Python 3, mido 1.2.10) in the tools container:

- `mido_type1.mid`: type 1, 480 ticks per beat, three tracks: a tempo track
  (track name "Test Song", 120 bpm, 4/4, 100 bpm after four beats), a piano
  track (A4, C5, E5, A5 one beat each, then volume 90, pitch bend +4096 and
  A4 for two beats), a drum track (eight kick/snare hits on channel 10).
  mido reports 36 messages, 13 note_on, length 4.1 s.
- `mido_type0_a4.mid`: type 0, 96 ticks per beat, program 16 (organ),
  A4 (440 Hz) for four beats at the default 120 bpm (2.0 s).

The tracker modules were written byte by byte by the same script following
the format descriptions (there is no other tool at hand that saves them):

- `sine_4ch.mod`: ProTracker M.K., one 32-sample signed sine cycle looped,
  C-2 (period 428) on row 0, period 254 on row 8, pattern break on row 15.
- `sine.s3m`: Scream Tracker 3, one sample (C2Spd 8363), C-4 then A-4, break on row 15.
- `sine_env.xm`: FastTracker 2, linear frequencies, one instrument with a
  16-bit delta-coded 64-sample sine cycle and a sustained volume envelope,
  C-4 then key-off on row 8.
- `sine.it`: Impulse Tracker (sample mode), 8-bit signed sample, C5Speed
  8363, C-5 then A-5, break on row 15.

Comparison with an independent player: `openmpt123 --batch --render
--samplerate 44100` (libopenmpt 0.7.3) of the modules, measured with an
FFT peak search: MOD 258.8 / 436.3 Hz, S3M 261.5 / 440.6 Hz, XM 130.4 Hz
(sound ends at 1.56 s after the key-off), IT 261.5 / 439.8 Hz, last sound
at 2.02 s (openmpt renders a short tail; Convertible stops at the end of
row 15, 1.92 s). The tests check these values within 0.6 %.

## gen_synth.py (excerpt of the essential calls)

```python
mid = mido.MidiFile(type=1, ticks_per_beat=480)  # tracks as described above
mid.save('mido_type1.mid')
m0 = mido.MidiFile(type=0, ticks_per_beat=96)
t = mido.MidiTrack(); m0.tracks.append(t)
t.append(mido.Message('program_change', channel=0, program=16, time=0))
t.append(mido.Message('note_on', channel=0, note=69, velocity=100, time=0))
t.append(mido.Message('note_off', channel=0, note=69, velocity=0, time=384))
t.append(mido.MetaMessage('end_of_track', time=0))
m0.save('mido_type0_a4.mid')
```
