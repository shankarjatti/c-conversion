# Terrestrial module — verification status

The terrestrial module (DMR, NXDN96, P25, TETRA, NXDN48, D-STAR, X2-TDMA, ProVoice) in C++: Ingest, bridge, decoder, console.

Every number was measured on the development machine; ⬜ = not done / not verified. Entries were written while the module still lived in a combined development tree; names of tools were adjusted to this repository's layout.

### 2026-10-29 — single IQ subscriber (Ingest) and the ZeroMQ voice regression

| Item | State |
|---|---|
| `ingest/ingest.py`: ONE SUB for all modules, routes by topic or header `centre_hz` via reloadable `ingest/routes.json`, counts unknown/bad/rate-mismatch/upstream loss | ✅ written, run with shuffled one-by-one arrival from the RTOS-style replay: 8 bands routed evenly, 0 unknown, 0 lost |
| Replay in RTOS style (`SECURECOM_REPLAY_STYLE=rtos`: one topic, no band name, `centre_hz` jittered ±2 kHz, random order) | ✅ used by `run_ui_dev.sh` |
| Voice regression with ZeroMQ IQ (nxdn 0.2 s, p25 1.5 s per 20 s) | ✅ root cause: SUB prefix match, `iq/nxdn` also received `iq/nxdn48`; fixed with trailing `/` topics |
| Voice per 20 s after fix (dmr/nxdn/p25/tetra/nxdn48/dstar) | ✅ 3.1 / 17.5 / 11.1 / 13.2 / 16.6 / 18.1 = TCP baseline |
| `zmq_sub_check` against the new chain | ✅ 15/15 |
| Air & maritime module on the Ingest, its ZeroMQ results interface | ⬜ not started (routes for ais/adsb are already in routes.json) |
| Ingest digital tuning (offset/filter/resample), single results endpoint + command routing | ⬜ not started |
| Real RTOS header fields (names, seq meaning, wideband vs channelised) | ⬜ unknown: needs the RTOS team |

#### Step 2 — C++ terrestrial bridge (2026-10-29)

| Item | State |
|---|---|
| `terrestrial/backend/terr_bridge/` (IQ SUB from the Ingest, 512-point FFT, activity with learned noise floor + cross-band cap, channel power, audio taps, per-transmission WAV recorder, TETRA PMT events, decoder process control, heartbeat, REP commands) | ✅ builds clean, no warnings |
| `zmq_sub_check --decoder-cycle` on the C++ bridge | ✅ 18/18 (B6 test itself was fixed: it assumed a file next to the recordings folder) |
| Side by side with the Python bridge, same replay, 60 s each: voice s/20 s dmr/nxdn/p25/tetra/nxdn48/dstar | ✅ C++ 3.98/18.20/12.21/11.52/17.23/18.36 vs Python 3.84/18.16/12.51/12.82/17.24/18.17 |
| Spectrum frames, active %, rms dB, floor dB, best channel, occupied channels per band | ✅ 10.0 frames/s; active % within 4 points (dmr 60.5 vs 64.3, rest ≤1.3); rms within 0.7 dB; floors within 1.3 dB; same best channel |
| Recordings / TETRA events in 60 s | ✅ 19 (136 s) / 27 vs Python 17 (124 s) / 25; TETRA event content has identical keys and values |
| Folder-escape refusals (download, delete) | ✅ |
| Not done in C++ | ⬜ the decoder itself is still `allinone.py` (step 5); WebSocket and Module 1 control are dropped on purpose |

#### Step 5 — C++ terrestrial decoders: 7 DSD chains + TETRA (2026-10-29/30)

| Item | State |
|---|---|
| `terrestrial/backend/terr_decoder/`: GNU Radio C++ flowgraph, ZeroMQ IQ sources, per band pfb channelizer -> channel sum -> resampler -> low-pass -> squelch -> FM demod -> gr-dsd -> x6 resampler -> volume -> audio tap; TETRA chain (xlating FIR, MMSE resampler, AGC, FLL, PFB clock sync, CMA equaliser, diff phasor, DQPSK decoder) + tetra-kit C API + ACELP codec + call-log events; no audio sinks | ✅ builds clean; runs real time at ~95 % of one core (Python decoder 120-165 %) |
| Offline, same IQ file, Python reference flowgraph vs C++ (`dsd_chain_ref.py`), 150 s incl. loop seams | ✅ NXDN 138.31 s vs 138.31 s voice, DMR 23.49 vs 23.50 s; also with real-time throttling, with the live capture, and under 8 parallel runs. Audio is not byte-identical (DSD output timing shifts by a few dozen samples) but the decoded amount is equal within 0.02 % |
| Live, both decoders on the same IQ, voice s per 20 s (Python / C++) | ✅ P25 11.0 / 11.0, NXDN48 16.9 / 16.9, D-STAR 17.9 / 17.9, DMR 3.1-3.4 / 2.9-3.4, TETRA 12.2-13.2 / 11.6-13.8 |
| ⚠️ Live NXDN | C++ 16.0-17.2 vs Python 17.6-17.9 per 20 s (about 5 % less voice, more short dropouts); both are below their own offline result (92 % voiced) so live arrival affects gr-dsd's yield. Not reproduced offline (chunk size, throttle, CPU load, buffer limits, DSD output multiple all tested: no effect). Cause not isolated |
| TETRA through the C++ decoder | ✅ voice 11.6-13.8 s per 20 s, call-log events (call_setup, broadcast ...) with talkgroup, slot, usage marker; 50 events in 100 s (Python: 25 in 60 s) |
| Interface check with the all-C++ terrestrial chain incl. decoder stop/start | ✅ 18/18 |
| Not covered | ⬜ X2-TDMA / ProVoice still have no real samples (silence only); no over-the-air run of the C++ decoder |

#### Step 6a — C++ terrestrial console `terrestrial/ui/terr_ui` (2026-10-30)

| Item | State |
|---|---|
| Qt5 Widgets console, ZeroMQ only (SUB `terr/...` + REQ commands); layout and behaviour of `securecom_gui.html` | ✅ builds clean; run offscreen against the live all-C++ backend |
| Data path vs the backend (40 s): messages by topic seen by the UI vs `bridge_profile.py` | ✅ iq 3200 = 8 bands x 10/s x 40 s, 12 recording events = 12, 20 TETRA events = 20, 0 sequence gaps, IQ arriving 8/8 |
| Plots: composite spectrum (carrier markers), waterfall, time; drag / wheel / double-click zoom | ✅ zoom test: drag 30-60 % of 419-454 MHz -> 429.507-440.013, wheel narrows around the cursor, double-click -> 419-454 |
| Signals tables, channel activity, decoded field (TETRA call log), audio meters, log, status line | ✅ screenshots checked (states DECODING/ACTIVE/idle per band as in the HTML page) |
| Recordings: list (newest 200), fetch in 256 KB chunks, WAV parse, delete | ✅ fetched a 542,926-byte recording = listing size, valid WAV (5.66 s); throw-away file deleted through the UI path |
| Reconnect: bridge killed and restarted while the UI ran | ✅ LIVE -> NOT CONNECTED after 5 s -> LIVE again at restart, no restart of the UI needed |
| Sound (PulseAudio mixer for Listen, recording playback) | ⚠️ written, compiled, never played: nothing was switched on during tests (no sound on your headphones). Needs a listening check by you |
| Differences from the HTML page (on purpose) | Start decoder does not switch Listen on; time plot autoscales (raw IQ is -45 dBFS, a flat line at full scale); frequency labels thin out with width; startup asks `get_state` instead of waiting for the 10 s re-publish; only the newest 200 recordings are listed |
| CPU | ~27 % of one core offscreen with all audio and spectrum frames |
| Not covered | ⬜ air & maritime console (step 6b); visible window checked only through offscreen screenshots; mouse zoom on a real display |

#### Live B210 DMR test (2026-11-02)
First run of the C++ chain on a real radio: `tools/run_terrestrial.sh start --b210 --test` (B210 serial 3273AC6 on USB 3, 446.118528 MHz, 200 kSps, gain 24 dB, antenna RX2, a DMR handheld keyed by the user).

| Item | State |
|---|---|
| B210 stream into the Ingest | ✅ level -77 dBFS idle, -27 ... -40 dBFS keyed, 0 clipped, 0 overflows, 0 send drops, 0 upstream loss |
| C++ terrestrial decoder (DMR chain) on the live signal | ✅ DMR446 DECODING, real TDMA bursts in the time plot, carrier at 446.12 MHz in spectrum/waterfall, DSD slot voice frames in the log |
| 60 s window | 12.1 s decoded voice per 20 s, 3 recordings saved (69 s) |
| Console on screen | ✅ opened on the user's desktop (Listen off) |
| Same keying on the Python decoder for comparison | ⬜ not done yet |
| Listening to a recording (sound) | ⬜ not done yet |

### Re-verification in this repository layout (2026-11-03)
The module was built from this repository (`tools/run_terrestrial.sh build`) and run on its own with the stand-in RTOS (`start --replay --test`).

| Item | State |
|---|---|
| Build of backend (Ingest, bridge, decoder) and console from a clean tree | ✅ |
| `tools/bench/interface_check.py` against the running module | ✅ 15/15 |
| IQ arriving on 8/8 bands, Ingest: 0 unknown, 0 bad, 0 upstream loss, 0 dropped | ✅ |
| Voice per 20 s on the replay (dmr / nxdn / p25 / tetra / nxdn48 / dstar) | ✅ 2.5 / 15.6 / 11.0 / 12.0 / 17.2 / 17.8 |
| TETRA from the vendored runtime in `third_party/tetra_runtime` | ✅ voice and call-log events |
| Runs next to the air & maritime module (own ports; that module's frequencies are ignored by this Ingest) | ✅ |
