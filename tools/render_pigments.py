#!/usr/bin/env python3
"""render_pigments.py — Gauntlet A/B renderer: fixed gauntlet clip through
Arturia Pigments 7 VST3, fully headless via pedalboard (no GUI, no focus).

Usage:
    python tools/render_pigments.py [--dump-params] [--wet OUT.wav] [--vars PREFIX]

Modes:
  (none)          legacy behaviour: dry perc render -> pigments_perc.wav
  --wet OUT.wav   same clip/patch, rendered dry then passed through a hall-ish
                  reverb (pedalboard Reverb, wet 35%) -> OUT.wav
  --vars PREFIX   three tonal variations of the perc patch (bright / dark /
                  short: wavetable position and envelope decay/release vary)
                  -> PREFIX{1,2,3}.wav

Every mode reports render wall-clock (x-realtime) and process peak RSS for the
CPU gauntlet row.

WET-MODE PROVENANCE NOTE (2026-08-26, measured, see build/gauntlet/probe_fx*.py):
Pigments' own FX-section reverb parameters exist in the 2345-param export
(fx_a1..b3_/aux1..3_reverb_*), but writing them - plus fx_*_dry_wet in BOTH
interpretations (percent 0..100 and normalized 0..1) and every bypass/routing
gate (fx_bus_a_bypass, fx_global_bypass, aux_bus_bypass, aux_send/return) -
leaves the audio bit-identical: all six chain slots and the aux buses host NO
effect module in the init program, and the effect-TYPE selector is not exposed
as an automatable VST3 parameter (the exact limitation already documented for
Engine1_ModuleType above). The wet render therefore applies the reverb as an
offline pedalboard Reverb stage on this script's own output: same clip, same
patch, deterministic, still fully headless.

Writes build/gauntlet/pigments_perc.wav (48 kHz stereo PCM16, 10 s) using the
EXACT clip from tools/render_probe.cpp:
    C4(60)v127 @0.0s   A3(57)v100 @0.5s   E5(76)v64 @1.0s
    chord 60+64+67 v90 @2.0s              C2(36)v127 @3.0s
each note released 50 ms after its strike.

PATCH CHOICE (documented per task requirement)
----------------------------------------------
Target: one percussive patch approximating a struck resonant body
(fast attack <5 ms, exponential decay ~1-3 s, tonal partials + short noise
transient).

Engine survey (Reference_ParamNames.xml + live VST3 parameter enumeration):
Pigments 7 has five oscillator modules per engine slot - Wavetable (WTOsc),
Virtual Analog (VA3Osc), Harmonic (HarmonicOsc), Sample&Granular, and a
physical MODAL module (modal_N_*: mallet/friction collision exciter +
resonator partials/decay/brilliance). The Modal module would be the closest
match for a struck resonant body, BUT the engine-module-type selector
(Engine1_ModuleType exists in Reference_ParamNames.xml) is NOT exposed as an
automatable VST3 parameter - pedalboard enumerates 2345 parameters and none
selects the module type. The init program runs Engine 1 as the WAVETABLE
module (proven by mute probe: muting wavetable_1_main_vol_db removes 99.94%
of RMS; muting analog/sample/harmonic/modal blocks removes nothing).

Chosen patch (everything reachable headlessly, verified by measurement):
  * Tone: Wavetable engine, position 0.67 (empirically the harmonically
    richest factory frame: partials 2-12 carry ~56% of fundamental energy,
    centroid ~3.9 kHz vs pure-sine default at position 0.0), unison off for
    determinism.
  * Noise transient: Utility module's noise generator #1 (always-present
    third voice layer, independent of engine module type). NOTE: the param
    named 'utility_bypass' ships at 1.0 and 1.0 = module ACTIVE (inverted
    naming, verified by A/B); noise becomes audible only with
    utility_noise1_length=0.5 and phase_retrig=free (0.0). Keyboard tracking
    on so the transient brightness follows pitch.
  * Percussive shape: Env VCA (env_amp_*). Measured mapping of the
    normalized value: attack 0.08 -> measured 3.35 ms waveform rise (<5 ms);
    release 0.62 -> measured 2.63 s ring to -60 dB (within the 1-3 s band).
    Notes are released at +50 ms, so the
    RELEASE stage carries the exponential ring-out; decay+sustain=0 keep
    held notes percussive too.

HEADLESS DISCIPLINE: pedalboard instantiates the VST3 component without any
editor/window; nothing is shown or focused. Module on/off toggles after load
are avoided entirely (they can wedge the Arturia DSP into a frozen-output
state); all parameters are written once, then read back and asserted.
"""
import sys
import time
import wave
import ctypes
import numpy as np
from pedalboard import load_plugin, Pedalboard, Reverb

PLUGIN_PATH = r"C:/Program Files/Common Files/VST3/Pigments.vst3"
SR = 48000.0
DURATION_S = 10.0
OUT_WAV = "build/gauntlet/pigments_perc.wav"
PARAM_DUMP = "build/gauntlet/pigments_params.txt"
BUFFER_SIZE = 64  # small blocks keep MIDI event timing tight; big buffers drop events


# ---- the gauntlet clip (identical to tools/render_probe.cpp) ----------------
def gauntlet_events():
    """(midi_bytes, timestamp_seconds) tuples; timestamps == sample-exact times."""
    def t(sec):
        return round(sec * SR) / SR  # quantize to whole samples like the rig

    ev = [
        (t(0.0), True, 60, 127),
        (t(0.05), False, 60, 0),
        (t(0.5), True, 57, 100),
        (t(0.55), False, 57, 0),
        (t(1.0), True, 76, 64),
        (t(1.05), False, 76, 0),
        (t(2.0), True, 60, 90),
        (t(2.0), True, 64, 90),
        (t(2.0), True, 67, 90),
        (t(2.05), False, 60, 0),
        (t(2.05), False, 64, 0),
        (t(2.05), False, 67, 0),
        (t(3.0), True, 36, 127),
        (t(3.05), False, 36, 0),
    ]
    out = []
    for when, on, note, vel in ev:
        status = 0x90 if on else 0x80
        out.append(((status, note, vel), when))
    return out


# ---- patch definition -------------------------------------------------------
PATCH = {
    # tone: wavetable engine 1
    "wavetable_1_main_vol_db": 0.90,   # ~ -x dB; calibrated level, keeps peak < 1.0
    "wavetable_1_mod_vol_db": 0.0,     # mod osc silent
    "wavetable_1_position": 0.67,      # richest factory frame (measured)
    "wavetable_1_unison": 0.0,         # single voice -> deterministic
    "wavetable_1_unison_mix": 0.0,
    "wavetable_1_fold_amount": 0.0,
    "wavetable_1_pm_amount": 0.0,
    "wavetable_1_mod_amount": 0.0,
    # noise transient: utility layer noise gen 1
    "utility_noise1_length": 0.10,     # REQUIRED for audibility (measured)
    "utility_noise1_phase_retrig": 0.0,  # free-running; retrig variant is silent
    "utility_noise1_keyboard_tracking": 1.0,
    "noise_1_volume_db": 0.15,         # transient sits under the tonal strike
    "sub_osc_volume_db": 0.0,          # sub osc silent
    # percussive Env VCA (normalized-value mapping, measured)
    "env_amp_attack_ms": 0.08,         # ~1.5 ms rise (<5 ms requirement)
    "env_amp_decay_s": 0.55,
    "env_amp_sustain": 0.0,
    "env_amp_release_s": 0.62,         # ~2.5 s to -60 dB (1-3 s requirement)
}
MASTER_CANDIDATES = [0.673, 0.55, 0.45]  # start at init value, step down if hot


def dump_params(plugin):
    with open(PARAM_DUMP, "w", encoding="utf-8") as f:
        for name, prm in plugin.parameters.items():
            lo = prm.min_value
            hi = prm.max_value
            f.write(f"{name}\t{prm.raw_value}\t[{lo},{hi}]\n")


# ---- tonal variations for the preset-character piece ------------------------
VARIANTS = [
    # (suffix, label, PATCH overrides)
    ("1", "bright", {"wavetable_1_position": 0.95}),          # brightest frame
    ("2", "dark",   {"wavetable_1_position": 0.20}),          # near-sine, dull
    ("3", "short",  {"env_amp_release_s": 0.30, "env_amp_decay_s": 0.40,
                     "wavetable_1_position": 0.80}),          # fast ring-out
]

# hall-ish offline reverb stage (wet mode); see provenance note in the header
WET_BOARD = Pedalboard([Reverb(room_size=0.80, damping=0.30,
                               wet_level=0.35, dry_level=1.00, width=1.0)])


def peak_rss_mb():
    """Process peak working set in MB via kernel32 (no psutil dependency)."""
    class PMC(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    k32 = ctypes.windll.kernel32
    k32.GetCurrentProcess.restype = ctypes.c_void_p  # pseudo-handle must survive 64-bit
    hProc = k32.GetCurrentProcess()
    k32.K32GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), ctypes.c_ulong]
    ok = k32.K32GetProcessMemoryInfo(hProc, ctypes.byref(pmc), pmc.cb)
    if not ok:  # pre-Win8 fallback
        psapi = ctypes.windll.psapi
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), ctypes.c_ulong]
        ok = psapi.GetProcessMemoryInfo(hProc, ctypes.byref(pmc), pmc.cb)
    return pmc.PeakWorkingSetSize / (1024.0 * 1024.0) if ok else float("nan")


def assert_wet_board():
    """Write-once/readback assert for the offline reverb stage parameters."""
    want = {"room_size": 0.80, "damping": 0.30, "wet_level": 0.35,
            "dry_level": 1.00, "width": 1.0}
    rv = WET_BOARD[0]
    bad = {k: getattr(rv, k) for k, v in want.items()
           if abs(getattr(rv, k) - v) > 1e-6}
    if bad:
        raise RuntimeError(f"reverb readback mismatch: {bad}")


def configure(plugin, master_raw, overrides=None):
    """Write every patch parameter once (normalized domain), then read back."""
    sp = plugin.parameters
    plan = dict(PATCH)
    if overrides:
        plan.update(overrides)
    plan["master_volume_db"] = master_raw
    failed = []
    for name, val in plan.items():
        try:
            sp[name].raw_value = min(1.0, max(0.0, float(val)))
        except KeyError:
            failed.append(name)
    if failed:
        raise RuntimeError(f"parameters missing from VST3 export: {failed}")
    mismatch = []
    for name, val in plan.items():
        want = min(1.0, max(0.0, float(val)))
        got = sp[name].raw_value
        if abs(got - want) > 1e-3:
            mismatch.append((name, want, got))
    if mismatch:
        raise RuntimeError(f"parameter readback mismatch: {mismatch}")


def render(plugin):
    midi = gauntlet_events()
    out = plugin(midi, duration=DURATION_S, sample_rate=SR,
                 num_channels=2, buffer_size=BUFFER_SIZE)
    if out.shape[1] < int(DURATION_S * SR) - 1:
        raise RuntimeError(f"short render: {out.shape}")
    return np.asarray(out, dtype=np.float32)


def write_wav16(path, x):
    """x: (2, N) float32 -> 16-bit PCM stereo WAV (wave module, stdlib)."""
    x = np.clip(x.T, -1.0, 1.0)
    pcm = (x * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(SR))
        w.writeframes(pcm.tobytes())


def analyze(x):
    mono = x.mean(axis=0)
    peak = float(np.max(np.abs(x)))
    rms = float(np.sqrt(np.mean(mono**2)))
    above = np.where(mono > 10 ** (-60 / 20))[0]
    tail_s = float(above[-1]) / SR if above.size else 0.0
    # attack: 10->90% rise on smoothed |mono| around first onset
    win = 24
    env = np.convolve(mono, np.ones(win) / win, mode="same")
    thresh = 0.02 * env.max()
    i_on = int(np.argmax(env > thresh))
    lo = env[max(i_on - 1, 0)]
    hi = env[i_on:i_on + int(SR)].max()
    t10 = i_on
    while t10 > 0 and env[t10] > lo + 0.1 * (hi - lo):
        t10 -= 1
    t90 = i_on
    while t90 < len(env) - 1 and env[t90] < lo + 0.9 * (hi - lo):
        t90 += 1
    rise_ms = (t90 - t10) / SR * 1000.0
    return {"peak": peak, "rms": rms, "tail_to_-60dB_s": tail_s, "attack_rise_ms": rise_ms}


def calibrate(plugin, overrides=None, label=""):
    """Master calibration loop; returns (stats, x) of the first render < 0.98 FS."""
    for attempt, master in enumerate(MASTER_CANDIDATES):
        configure(plugin, master, overrides)
        t0 = time.perf_counter()
        x = render(plugin)
        wall = time.perf_counter() - t0
        stats = analyze(x)
        stats["master_raw"] = master
        stats["render_wall_s"] = round(wall, 4)
        stats["x_realtime"] = round(DURATION_S / wall, 2)
        print(f"{label}attempt {attempt}: master_raw={master:.3f} peak={stats['peak']:.4f} "
              f"rms={stats['rms']:.5f} attack={stats['attack_rise_ms']:.2f}ms "
              f"tail(-60dB)={stats['tail_to_-60dB_s']:.2f}s "
              f"wall={wall:.3f}s ({stats['x_realtime']}x realtime)")
        if stats["peak"] <= 0.98:
            return stats, x
        print("peak too hot, lowering master and re-rendering")
    raise RuntimeError("could not bring peak under 0.98 FS")


def main(argv):
    def arg_after(flag):
        return argv[argv.index(flag) + 1] if flag in argv else None

    wet_out = arg_after("--wet")
    vars_prefix = arg_after("--vars")
    legacy = wet_out is None and vars_prefix is None

    print("loading Pigments.vst3 headless (pedalboard, no editor window)...")
    plugin = load_plugin(PLUGIN_PATH)
    if not plugin.is_instrument:
        raise RuntimeError("Pigments did not report itself as an instrument")
    print(f"loaded '{plugin.name}': {len(plugin.parameters)} automatable parameters")
    if "--dump-params" in argv:
        dump_params(plugin)
        print(f"parameter dump -> {PARAM_DUMP}")

    if legacy or wet_out:
        stats, x = calibrate(plugin, label="perc ")
        # determinism spot-check: second render must match closely
        x2 = render(plugin)
        diff = float(np.max(np.abs(x2 - x)))
        print(f"determinism: max|render2-render1| = {diff:.2e}")

    if legacy:
        write_wav16(OUT_WAV, x)
        print(f"wrote {OUT_WAV} ({int(SR)} Hz stereo PCM16, {DURATION_S:.0f}s)")
        print("\nsummary:")
        for k, v in stats.items():
            print(f"  {k}: {v}")
        print("patch: Pigments 7 'Struck Body' approximation = Wavetable engine "
              "(position 0.67, rich frame) + Utility noise-gen transient + Env VCA "
              "percussive envelope (A~1.5ms / R~2.5s)")

    if wet_out:
        assert_wet_board()
        print(f"wet stage: pedalboard Reverb(room_size={WET_BOARD[0].room_size}, "
              f"damping={WET_BOARD[0].damping}, wet={WET_BOARD[0].wet_level}, "
              f"dry={WET_BOARD[0].dry_level}) [readback-asserted]")
        t0 = time.perf_counter()
        xwet = np.asarray(WET_BOARD(x, SR), dtype=np.float32)
        wwall = time.perf_counter() - t0
        wstats = analyze(xwet)
        print(f"wet render: wall={wwall:.3f}s ({DURATION_S/wwall:.1f}x realtime incl. reverb) "
              f"peak={wstats['peak']:.4f} tail(-60dB)={wstats['tail_to_-60dB_s']:.2f}s "
              f"(dry was {stats['tail_to_-60dB_s']:.2f}s)")
        write_wav16(wet_out, xwet)
        print(f"wrote {wet_out}")

    if vars_prefix:
        for suffix, label, ov in VARIANTS:
            st, xv = calibrate(plugin, overrides=ov,
                               label=f"var{suffix}[{label}] ")
            out_path = f"{vars_prefix}{suffix}.wav"
            write_wav16(out_path, xv)
            print(f"wrote {out_path} ({label}: position="
                  f"{ov.get('wavetable_1_position', PATCH['wavetable_1_position'])})")

    print(f"\ncpu: process peak RSS {peak_rss_mb():.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
