# 169 — The sound mixer, voice by voice: a cheaper game thread on the Switch

Status: current
Checked: 2026-11-06

Phase 4, NX3 (finding 167). The console profile (`DHD_SWITCH_PROFILE`, STATE.md "NX3, the CPU
threads measured on the console") put about 9% of the Switch's game thread, the thread that
limits a race at scale 2, in the native 989snd mixer (`port/hle/sound.c`, finding 022): `render`,
`reverb_sample` and `snd_adsr_tick`. This is how that cost was cut without changing a sample.

## Where the mixer runs and what it shares

`sound_tick` (once a vsync, from the game's own frame) renders the 800.8 samples the vsync is
due (4004/5 a field at 48 kHz, kept exact by counting), hands them to the host
(`host_sound_write`, `port/host/sound_out_sdl.c`: SDL's stream, which only queues them) and to
the state trace. Everything else the driver does reads what the mixer moved on: 989snd's handler
tick polls each voice's envelope (ENVX, four zero polls free it, `handlers_tick` every 200
samples), the RPCs ask whether a sound still plays, streams end and queue the next one at a sample
position. So the mixer's state is the game's state: every position is a function of the vsync
count, and a run is the same however slow the host is (finding 022).

That rules out the two "move it" designs:

- **Another host thread, or SDL's audio callback.** The mixer is core code (wasm2c), and the core
  is one instance with one shadow stack pointer and one call-depth counter
  (`WASM_RT_THREAD_LOCAL` is empty, build_host64.py): two host threads cannot run in it at once.
  Running the mixer outside the core means a second, native copy of the voice code fed with a
  snapshot of every voice each 200-sample period, and the envelope, endpoint and block stepping
  must still run on the game thread, since the driver reads them at the next tick. About half of
  the work stays, the rest needs the copy kept exact by hand. Not the cheapest; left as the next
  step if the game thread needs more (below).
- **Run it later or ahead.** What the driver sees would then depend on the host's timing.

## What changed: one voice at a time

The old loop advanced every voice by one output sample, then the next sample: 48 voice slots
visited 800 times a field, and every field of every sounding voice loaded and stored again for
each sample. Within one 200-sample driver period, though, nothing outside a voice changes it:
voices start, stop, release, change volume or pitch only in a tick (`handlers_tick`,
`stream_parameters_tick`, `duckers_tick`, all at multiples of 200 samples) or in an RPC (between
`sound_tick` calls), and the mix is an integer sum, the same in any order. So `render` now cuts
its samples at the next multiple of 200 (`SEGMENT`), and `mix_segment`:

1. runs the debug trace and the driver ticks exactly where the per-sample loop ran them (the
   segment's first sample);
2. runs each sounding voice through the whole segment (`voice_run`): the same steps in the same
   order as before for that voice, adding into a dry and a wet (reverb input) buffer per sample;
   a voice that falls silent stays silent to the segment's end, as it did, since phase 4 and
   `used = 0` change only in a tick or an RPC;
3. then, sample by sample, the streams (as before: they read `grain_samples`, which counts up
   here as it did), both cores' reverb, and the gain trace.

`snd_adsr_tick` and `snd_gaussian_sample` are inlined into the loops (`adsr_tick`, `gaussian`;
the exported names stay for the tests). The `DHD_SND_GAIN_TRACE` counts (voices sounding, the
largest voice volume, per sample) are kept in per-sample arrays, so the trace is the same too.

Tried and dropped (no gain on the bench below): a register copy of the voice for the segment,
whole or field by field (the stores to the mix buffers can alias it as far as the compiler
knows), and a scalar Gaussian in place of the SSE one (SSE becomes wasm SIMD, then SIMDe on the
host; the four scalar multiplies were 5% slower on x64).

## Checked the same

A harness built `test_sound.c`'s fixtures with the old and the new `sound.c` through the core's
own path (wasm32 with build_wasm.py's flags, wasm2c, guard pages, the host compiler at -O3; inside
`dhd-switch-dev`): the unit tests pass, 40 seeded random sequences of 3,000 steps each (plays of a
looping and a one-shot sound with random volume, pan and pitch fields, stops, reverb presets and
depths on both cores, master and group volumes, pauses, streams of 2-300 blocks looping or not,
"still playing" polls, renders of 1-1,024 samples) hash to the same PCM and polled state, and so
does a timed load. The harness is not in the tree (it needs the old file); it is
`test_sound.c` plus a `main` of 150 lines.

The timed load, 24 looping voices on both reverb cores, 600 fields at 800/801 samples, x64 host,
best of five, three alternating rounds:

| mixer | 24 voices + reverb | 24 voices, no reverb | reverb only | idle |
|---|---|---|---|---|
| per sample (before) | 0.415-0.439 ms a field | 0.351-0.357 | 0.107-0.109 | 0.040 |
| per voice and segment | 0.333-0.353 | 0.273-0.279 | 0.077-0.082 | 0.018-0.019 |

About a fifth of the mixer, and half of its fixed cost (48 slots visited 800 times).

## On the Switch

The same track at scale 2, clocks at `max`, against the merged build's run (STATE.md, "NX3, the
CPU threads measured on the console"), both without the profiler:

| | race fields/s | low1% | game busy, ms a field | VU busy | underruns |
|---|---|---|---|---|---|
| before (264 s of race) | 43.8 | 24.5 | 22.06 | 19.37 | 4 |
| after (137 s of race) | 44.6 | 22.7 | 20.87 | 19.10 | 4 |

The underruns are the same four in both runs, at start-up and the race's load (the queue runs
empty while the game loads); none came in the race. The player heard no difference. The 30-second
windows of one run spread from 20 to 24.6 ms of game busy time, so the 1.2 ms the averages differ
by is not all this change: the bench's fifth of a ~2 ms mixer predicts about 0.4 ms. The limit
is still `cpu:game` in most race windows, with the VU thread close behind.

## What is left

Getting the remaining ~1.6 ms off the game thread needs the mixer out of the core: a native
copy of `voice_run` and the reverb on a host thread, fed at each 200-sample period with a copy of
the sounding voices (and the reverb, master and movie changes in order), while the game thread
keeps the control half (envelopes, endpoints, block flags) that the driver reads. That copy must
stay bit-exact with the core's by hand, so it is worth doing only if the game thread is still
the limit once the VU and renderer work lands.
