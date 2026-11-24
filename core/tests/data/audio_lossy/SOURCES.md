# audio_lossy fixtures

All files are synthetic and were produced by independent, widely used
encoders; none was written by Convertible. The per-area listings with the
exact commands are in the sub-directories:

- `mpeg/SOURCES.md`: MP3 (lame) and MP2 (ffmpeg, twolame)
- `aac/SOURCES.md`: AAC from Apple Core Audio (`afconvert`), Fraunhofer FDK (`fdkaac`) and ffmpeg
- `xiph/SOURCES.md`: Opus (`opusenc`), Vorbis (`oggenc`, ffmpeg/libvorbis), Speex (`speexenc`)
- `speech/SOURCES.md`: AMR-NB (opencore-amr's example encoder), AMR-WB (vo-amrwbenc's example encoder), GSM 06.10 (sox/libgsm)
- `synth/SOURCES.md`: MIDI files (Python `mido`) and tracker modules (written byte by byte by a script)

## Source signals

Every audio fixture encodes one of these signals, generated with Python 3 +
NumPy (`gen_sources.py`, reproduced at the end of this file), 16-bit WAV,
0.6 s long:

- `music_<rate>_st` / `music_<rate>_mono`: four decaying harmonic notes
  (220, 277.18, 329.63, 440 Hz, six harmonics, every 0.15 s) on the left; the
  same notes a fifth higher plus a linear sweep from 200 Hz to
  min(12 kHz, 0.45 x rate) on the right; a 30 ms noise burst at 0.33 s; a
  white-noise floor at -40 dBFS. Mono versions are the mean of both sides.
  Rates 8000, 16000, 22050, 24000, 32000, 44100, 48000.
- `speech_<rate>_mono`: a pulse train at 120 +/- 30 Hz through three formant
  resonators (700, 1200, 2600 Hz), voiced/unvoiced at 3 Hz. Rates 8000,
  16000, 48000.

## References

`<name>_ref.flac` next to a fixture is the output of an independent decoder
(ffmpeg 6.1's native decoders unless the area's list says otherwise),
16-bit, cut to the first 0.2 s to keep the files small:

    ffmpeg -i <fixture> -map_metadata -1 -c:a flac -sample_fmt s16 -compression_level 12 -fflags +bitexact -bitexact full.flac
    ffmpeg -i full.flac -t 0.2 -map_metadata -1 -c:a flac -compression_level 12 -fflags +bitexact -bitexact <name>_ref.flac

(`lame_cbr64_mono_22050_ref.flac` uses `-compression_level 8` in the second
step.) Tools ran in a throwaway `ubuntu:24.04` container (ffmpeg 6.1.1,
LAME 3.100, TwoLAME 0.4.0, opus-tools 0.2 with libopus 1.4, vorbis-tools 1.4.2,
speexenc 1.2.1, SoX 14.4.2, fdkaac 1.0.0, mpg123 1.32.5, openmpt123 0.7.3,
Python 3 with mido 1.2.10 and NumPy 1.26.4) except the
`af_*` files, written by `afconvert` on macOS.

## gen_sources.py

```python
import math, sys, wave
import numpy as np
out = sys.argv[1]
def write(name, rate, data):
    pcm = np.round(np.clip(data, -1.0, 1.0) * 32767.0).astype('<i2')
    with wave.open(f'{out}/{name}.wav', 'wb') as w:
        w.setnchannels(pcm.shape[1]); w.setsampwidth(2); w.setframerate(rate); w.writeframes(pcm.tobytes())
rng = np.random.default_rng(1234)
def music(rate, seconds, stereo=True):
    n = int(rate * seconds); t = np.arange(n) / rate
    left = np.zeros(n); right = np.zeros(n)
    for start, f in [(0.00, 220.0), (0.15, 277.18), (0.30, 329.63), (0.45, 440.0)]:
        env = np.where(t >= start, np.exp(-(t - start) * 4.0), 0.0) * (t >= start)
        tone = sum(np.sin(2 * math.pi * f * k * t) / k for k in range(1, 7) if f * k < rate / 2)
        left += 0.18 * env * tone
        right += 0.14 * env * np.sin(2 * math.pi * f * 1.5 * t)
    right += 0.12 * np.sin(2 * math.pi * (200 * t + (min(12000, rate * 0.45) - 200) / (2 * seconds) * t * t))
    burst = (t >= seconds * 0.55) & (t < seconds * 0.55 + 0.03)
    noise = rng.standard_normal(n) * 0.25
    left += np.where(burst, noise, 0.0); right += np.where(burst, np.roll(noise, 7), 0.0)
    left += 0.01 * rng.standard_normal(n); right += 0.01 * rng.standard_normal(n)
    return ((left + right) * 0.5)[:, None] if not stereo else np.stack([left, right], axis=1)
def speech(rate, seconds):
    n = int(rate * seconds); t = np.arange(n) / rate
    phase = 2 * math.pi * np.cumsum(120 + 30 * np.sin(2 * math.pi * 2.0 * t)) / rate
    src = np.zeros(n); src[np.diff(np.floor(phase / (2 * math.pi)), prepend=0) > 0] = 1.0
    sig = np.zeros(n)
    for formant, bw, gain in ((700, 80, 1.0), (1200, 90, 0.6), (2600, 120, 0.3)):
        if formant >= rate / 2: continue
        r = math.exp(-math.pi * bw / rate); a1 = 2 * r * math.cos(2 * math.pi * formant / rate); a2 = -r * r
        y = np.zeros(n); y1 = y2 = 0.0
        for i in range(n):
            v = src[i] + a1 * y1 + a2 * y2; y[i] = v; y2, y1 = y1, v
        sig += gain * y
    sig /= np.max(np.abs(sig)) + 1e-9
    voiced = np.sin(2 * math.pi * 3.0 * t) > -0.3
    return (sig * 0.5 * voiced + 0.03 * rng.standard_normal(n) * (~voiced))[:, None]
# called in this order (the random generator is shared):
write('music_44100_st', 44100, music(44100, 0.6)); write('music_48000_st', 48000, music(48000, 0.6))
write('music_32000_st', 32000, music(32000, 0.6)); write('music_22050_mono', 22050, music(22050, 0.6, False))
write('music_24000_mono', 24000, music(24000, 0.6, False)); write('music_8000_mono', 8000, music(8000, 0.6, False))
write('music_16000_mono', 16000, music(16000, 0.6, False)); write('speech_8000_mono', 8000, speech(8000, 0.6))
write('speech_16000_mono', 16000, speech(16000, 0.6)); write('speech_48000_mono', 48000, speech(48000, 0.6))
```

## Interoperability of Convertible's own output (checked during development)

Files written by Convertible from `music_44100_st.wav` were decoded with
ffmpeg 6.1.1 and the reference tools:

| Output | Independent decoder | Result |
| --- | --- | --- |
| MP3 q90 (VBR, 227 kbit/s), CBR 128 | ffmpeg, mpg123 | decode; gapless: exactly 26460 frames (Info/LAME tag honoured); SNR vs source 13.7 / 9.8 dB (LAME V2 / CBR 128 on the same input: 13.4 / 10.9 dB) |
| MP2 q90 | ffmpeg | decodes; 481-sample filterbank delay (no gapless signalling in MP2) |
| M4A (AAC-LC) q90 | ffmpeg, Apple `afinfo` | decodes; edit list honoured (start aligned), SNR 16.7 dB |
| AAC ADTS q75 | ffmpeg | decodes; 1600 samples of encoder priming, SNR 16.4 dB |
| Ogg Vorbis q90 | ffmpeg, oggdec | decodes; exactly 26460 frames; SNR 27.2 dB |
| Ogg Opus q90 | ffmpeg, opusdec | decodes (48 kHz) |
| Ogg Speex q90 | ffmpeg, speexdec | decodes (32 kHz ultra-wideband) |
| AMR-NB / AMR-WB | ffmpeg | decode (8 / 16 kHz) |
