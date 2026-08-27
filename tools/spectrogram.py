#!/usr/bin/env python3
"""spectrogram.py — log-frequency spectrogram PNG + peak/decay summary for a WAV.

Usage: python tools/spectrogram.py <in.wav> <out.png>

Prints a text summary to stdout:
  peak dBFS              time-domain true peak of either channel
  tail(-30dB)            seconds after t=4.0s (end of gauntlet excitation)
                         until the 10 ms-block envelope drops below peak-30dB
  spectral centroid      energy-weighted mean frequency over active frames

PNG: matplotlib if importable (labeled plot); otherwise a self-contained
zlib/struct PNG writer with an inferno-like colormap (numpy only).
Color scale is fixed (-100..0 dB rel full-scale) so renders from different
presets/builds are directly comparable.
"""

import struct
import sys
import wave
import zlib

import numpy as np

# ---- analysis constants -----------------------------------------------------
FRAME = 4096          # STFT frame size
HOP = 1024            # STFT hop
FMIN, FMAX = 20.0, 20000.0
ROWS = 128            # log-frequency rows
VMIN_DB, VMAX_DB = -100.0, 0.0
GAUNTLET_EXCITE_END_S = 4.0   # last strike release ends here; tails follow


def read_wav(path):
    w = wave.open(path, "rb")
    nch, sw, sr, nf = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(nf)
    w.close()
    if sw != 2:
        raise SystemExit(f"error: {path}: expected 16-bit PCM, got sampwidth={sw}")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if nch < 1:
        raise SystemExit(f"error: {path}: no channels")
    peak = float(np.abs(x).max()) if x.size else 0.0     # true peak across channels
    mono = x.reshape(-1, nch).mean(axis=1) if nch > 1 else x
    return mono, sr, peak, nch


def stft_mag_db(mono, sr):
    """Frames x ROWS log-frequency magnitude in dB (full-scale referenced)."""
    win = np.hanning(FRAME)
    scale = 2.0 / win.sum()  # sine-amplitude referencing for a hann window
    n_frames = 1 + max(0, (len(mono) - FRAME)) // HOP
    if n_frames < 1:
        raise SystemExit("error: file shorter than one STFT frame")
    idx = np.arange(FRAME)[None, :] + HOP * np.arange(n_frames)[:, None]
    frames = mono[idx] * win
    spec = np.abs(np.fft.rfft(frames, axis=1)) * scale  # n_frames x (FRAME/2+1)
    freqs = np.fft.rfftfreq(FRAME, 1.0 / sr)

    fmax = min(FMAX, sr / 2.0)
    edges = FMIN * (fmax / FMIN) ** (np.arange(ROWS + 1) / ROWS)
    S = np.zeros((n_frames, ROWS))
    lo = np.searchsorted(freqs, edges[:-1], side="left")
    hi = np.searchsorted(freqs, edges[1:], side="left")
    for r in range(ROWS):
        a, b = lo[r], max(hi[r], lo[r] + 1)  # never empty: reuse nearest bin
        S[:, r] = spec[:, a:b].max(axis=1)
    return 20.0 * np.log10(S + 1e-12), edges, n_frames


def summary_metrics(mono, sr, peak):
    out = {}
    out["peak_dbfs"] = 20.0 * np.log10(peak) if peak > 0 else -999.0

    # 10 ms block-max envelope -> robust last-time-above-threshold
    W = int(0.010 * sr)
    pad = (-len(mono)) % W
    blocks = np.abs(np.concatenate([mono, np.zeros(pad)])).reshape(-1, W).max(axis=1)
    thr = peak * 10.0 ** (-30.0 / 20.0)
    above = np.nonzero(blocks >= thr)[0]
    if above.size:
        t_last = float(above[-1] + 1) * W / sr
    else:
        t_last = 0.0
    out["t_last_above"] = t_last
    out["tail_s"] = max(0.0, t_last - GAUNTLET_EXCITE_END_S)

    # spectral centroid over frames within 60 dB of the loudest frame
    win = np.hanning(FRAME)
    n_frames = 1 + max(0, (len(mono) - FRAME)) // HOP
    if n_frames > 0:
        idx = np.arange(FRAME)[None, :] + HOP * np.arange(n_frames)[:, None]
        spec = np.abs(np.fft.rfft(mono[idx] * win, axis=1))
        freqs = np.fft.rfftfreq(FRAME, 1.0 / sr)
        energy = spec.sum(axis=1)
        active = energy >= energy.max() * 1e-3  # -60 dB
        if active.any():
            cen = (spec[active] * freqs[None, :]).sum(axis=1) / np.maximum(
                spec[active].sum(axis=1), 1e-30)
            out["centroid_hz"] = float(np.average(cen, weights=energy[active]))
        else:
            out["centroid_hz"] = 0.0
    else:
        out["centroid_hz"] = 0.0
    return out


# ---- fallback PNG writer (no matplotlib) ------------------------------------
_STOPS = [  # inferno-like: position -> RGB
    (0.00, (0, 0, 4)),
    (0.25, (87, 16, 110)),
    (0.50, (188, 55, 84)),
    (0.75, (249, 142, 9)),
    (1.00, (252, 255, 164)),
]


def _colormap_lut():
    pos = np.array([p for p, _ in _STOPS])
    cols = np.array([c for _, c in _STOPS], dtype=np.float64)
    t = np.linspace(0.0, 1.0, 256)
    lut = np.stack([np.interp(t, pos, cols[:, k]) for k in range(3)], axis=1)
    return np.clip(lut, 0, 255).astype(np.uint8)


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png_fallback(path, img_rgb):
    h, w, _ = img_rgb.shape
    stride = w * 3
    # one filter-type byte (0=none) per scanline, assembled in numpy:
    # bytearray slice assignment rejects numpy uint8 buffers on newer stacks
    raw = np.zeros((h, stride + 1), dtype=np.uint8)
    raw[:, 1:] = np.ascontiguousarray(img_rgb).reshape(h, stride)
    comp = zlib.compress(raw.tobytes(), 9)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_png_chunk(b"IHDR", ihdr))
        f.write(_png_chunk(b"IDAT", comp))
        f.write(_png_chunk(b"IEND", b""))


def db_to_u8(S_db):
    frac = np.clip((S_db - VMIN_DB) / (VMAX_DB - VMIN_DB), 0.0, 1.0)
    lut = _colormap_lut()
    return lut[(frac * 255).astype(np.uint8)]


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <in.wav> <out.png>", file=sys.stderr)
        return 2
    wav_path, png_path = argv[1], argv[2]
    mono, sr, peak, nch = read_wav(wav_path)
    dur = len(mono) / sr

    S_db, edges, n_frames = stft_mag_db(mono, sr)
    m = summary_metrics(mono, sr, peak)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        t_edges = np.arange(n_frames + 1) * HOP / sr
        fig, ax = plt.subplots(figsize=(12, 5), dpi=130)
        pc = ax.pcolormesh(t_edges, edges, S_db.T, cmap="inferno",
                           vmin=VMIN_DB, vmax=VMAX_DB, shading="flat")
        ax.set_yscale("log")
        ax.set_ylim(FMIN, min(FMAX, sr / 2))
        ax.set_xlabel("time (s)")
        ax.set_ylabel("frequency (Hz)")
        ax.set_title(wav_path)
        fig.colorbar(pc, label="dBFS")
        fig.tight_layout()
        fig.savefig(png_path)
        plt.close(fig)
        backend = "matplotlib"
    except ImportError:
        img = db_to_u8(S_db)  # rows=freq(log), cols=time
        write_png_fallback(png_path, img[::-1])  # flip so low freq is at bottom
        backend = "fallback-png"

    print(f"file: {wav_path}  ({dur:.2f} s @ {sr} Hz, {nch} ch)")
    print(f"peak: {m['peak_dbfs']:.2f} dBFS")
    print(f"tail(-30dB): {m['tail_s']:.2f} s after {GAUNTLET_EXCITE_END_S:.1f} s "
          f"(last above threshold at {m['t_last_above']:.2f} s)")
    print(f"spectral centroid: {m['centroid_hz']:.1f} Hz")
    print(f"png: {png_path} ({backend})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
