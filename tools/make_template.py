"""Analyze PCM WAV and generate the firmware's spectral reference (numpy only)."""
from pathlib import Path
import argparse
import hashlib
import json
import wave
import numpy as np

RATE = 16000
FFT = 512
BLOCK = 1024
BANDS = 24
EDGES = np.unique(np.rint(np.geomspace(160, 7000, BANDS + 1) / RATE * FFT).astype(int))
assert len(EDGES) == BANDS + 1


def read_pcm(path):
    with wave.open(str(path), 'rb') as w:
        fmt = dict(channels=w.getnchannels(), sample_rate=w.getframerate(),
                   sample_width=w.getsampwidth(), frames=w.getnframes())
        if fmt['sample_width'] != 2 or w.getcomptype() != 'NONE':
            raise ValueError('Expected uncompressed 16-bit PCM WAV')
        a = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float64)
    a = a.reshape(-1, fmt['channels']) / 32768.0
    return a, fmt


def resample(a, source_rate):
    if source_rate == RATE:
        return a.copy()
    if source_rate != 48000:
        raise ValueError('This verified converter supports 48000 or 16000 Hz')
    # Symmetric anti-aliasing FIR; centered convolution compensates group delay.
    n = np.arange(127) - 63
    cutoff = 7300 / source_rate
    h = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(127)
    h /= h.sum()
    return np.convolve(a, h, mode='same')[::3]


def features(a):
    out = []
    for p in range(0, len(a) - BLOCK + 1, BLOCK):
        power = np.zeros(FFT // 2 + 1)
        for off in (0, 256, 512):
            s = np.fft.rfft(a[p + off:p + off + FFT] * np.hanning(FFT))
            power += abs(s) ** 2
        energy = np.array([power[EDGES[b]:EDGES[b+1]].mean() for b in range(BANDS)])
        log = np.log(energy + max(1e-12, energy.max() * 1e-5))
        log -= log.mean()
        norm = np.linalg.norm(log)
        out.append(log / norm if norm > 1e-8 and np.mean(a[p:p+BLOCK]**2) > 1e-10 else np.zeros(BANDS))
    return np.array(out, dtype=np.float32)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('--project', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.project
    assets = root / 'assets'
    assets.mkdir(parents=True, exist_ok=True)
    original = args.source.read_bytes()
    (assets / 'TRACK01.WAV').write_bytes(original)
    data, fmt = read_pcm(args.source)
    mono = data.mean(axis=1)
    if np.sqrt(np.mean(mono**2)) < .2 * np.sqrt(np.mean(data**2)):
        raise ValueError('Stereo cancellation: select a channel explicitly before converting')
    converted = resample(mono, fmt['sample_rate'])
    pcm = np.clip(np.rint(converted * 32768), -32768, 32767).astype('<i2')
    with wave.open(str(assets / 'reference-16k.wav'), 'wb') as w:
        w.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
        w.writeframes(pcm.tobytes())
    ref = features(pcm.astype(np.float64) / 32768)
    np.save(assets / 'template.npy', ref)
    (root / 'include').mkdir(exist_ok=True)
    rows = ',\n'.join('  {' + ','.join(f'{v:.9f}f' for v in row) + '}' for row in ref)
    text = ('#pragma once\n#include <cstddef>\nnamespace soundalert {\n'
            f'constexpr size_t kTemplateFrames = {len(ref)};\n'
            f'constexpr float kReferenceWeight = {float((ref*ref).sum()):.6f}f;\n'
            f'constexpr unsigned kBandEdges[25] = {{{",".join(map(str, EDGES))}}};\n'
            f'constexpr float kReference[kTemplateFrames][24] = {{\n{rows}\n}};\n}}\n')
    (root / 'include' / 'reference_template.h').write_text(text, encoding='utf-8')
    seconds = fmt['frames'] / fmt['sample_rate']
    report = dict(source=str(args.source), sha256=hashlib.sha256(original).hexdigest(),
        **fmt, seconds=seconds, peak=float(abs(data).max()),
        rms_dbfs=float(20*np.log10(np.sqrt(np.mean(data**2))+1e-15)),
        clipping_fraction=float(np.mean(abs(data)>=32767/32768)),
        reference_rate=RATE, reference_samples=len(pcm), template_frames=len(ref),
        template_seconds=len(ref)*BLOCK/RATE, block_ms=64,
        note='Reference-file analysis only; no room-recording or hardware validation.')
    (root / 'audio-analysis.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
