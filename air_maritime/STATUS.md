# Air & maritime module — verification status

The air & maritime module (AIS 162 MHz, ADS-B 1090 MHz) in C++: Ingest, bridge, decoder, console.

Every number was measured on the development machine; ⬜ = not done / not verified. Entries were written while the module still lived in a combined development tree; names of tools were adjusted to this repository's layout.

### 2026-10-29 — air & maritime on the Ingest + ZeroMQ output

| Item | State |
|---|---|
| Air decoder IQ in from the Ingest (`iq/ais/`, `iq/adsb/`; `iq_source` factory, `SECURECOM_IQ_TRANSPORT` zmq default, tcp for Module 1 — `run_airmaritime.sh` exports tcp) | ✅ real AIS + ADS-B recordings, RTOS-style one-by-one random-order publish: decoder source 0 lost messages, 6 MSps ADS-B kept real time |
| Air bridge spectrum from the Ingest | ✅ both bands, 10 spectrum msg/s each, 0 sequence loss |
| Air bridge ZeroMQ output (`air/...`, REP commands `get_state/start_decoder/stop_decoder`) | ✅ `air_zmq_check.py` 10/10: 6 AIS vessels with positions (same as the cable tests), 22–25 ADS-B aircraft, 10 with position, in ~60 s |
| Ingest during the run (10 bands incl. 48 MB/s ADS-B) | ✅ 0 unknown, 0 dropped, 0 upstream loss, replay never behind real time |
| Air UI (HTML) still on WebSocket | ⚠️ unchanged; the C++ air view will use `air/...` |
| Single results endpoint for both modules + command routing | ⬜ still two PUB/REP pairs (5560/5561 terrestrial, 5565/5566 air) |

#### Step 3 — C++ air & maritime bridge (2026-10-29)

| Item | State |
|---|---|
| `air_maritime/backend/air_bridge/` (AIS AIVDM decode + 2-message confirmation, ADS-B confirmation, CPR global/local decode with the plausibility rules, expiry 60 s / 10 min, Welch spectrum over every sample, `air/...` topics, REP commands, decoder process) | ✅ builds clean, no warnings |
| `air_zmq_check.py` on the C++ bridge | ✅ 10/10 |
| Python bridge and C++ bridge side by side on the SAME decoder output (`air_compare.py`, decoder frozen before the state query) | ✅ AIS 6 vs 6 vessels, ADS-B 25 vs 25 aircraft, 9 vs 9 with position; **0 field differences** (positions, altitude, speed, heading, callsign); spectrum 10.0 frames/s, same level (AIS -38.3 dB, ADS-B -11.3 dB) |
| Decoder commands | ✅ start, second start ("already running"), stop, status |
| Known small difference | AIS 6-bit text uses the standard apostrophe (character 39); the Python table has a backtick there |
| Not done in C++ | ⬜ the decoders (`airmaritime_decoder_live.py`, step 4) |

#### Step 4 — C++ air decoders, ADS-B and AIS (2026-10-29)

| Item | State |
|---|---|
| `air_maritime/backend/air_decoder/`: GNU Radio C++ flowgraph; ZeroMQ IQ sources, 10 AIS branches (2 channels x base + 4 carrier-offset hypotheses: FIR xlating decimate 5, FM demod + M&M symbol sync = gmsk_demod rebuilt, NRZI, invert, HDLC), ADS-B demodulator, field decoder, PMT out on 9201-9203 (same interface the bridge reads) | ✅ builds clean; runs real time (ADS-B 6001 kSps, AIS 250 kSps, 0 lost IQ messages) |
| ADS-B demodulator, C++ vs the Python original on the SAME file, 10 s of the real recording, three chunk sizes | ✅ identical frame sets and times (157 / 159 / 158 frames, 0 only-C++, 0 only-Python), SNR identical, 5x faster |
| ADS-B fields vs gr-adsb on known frames | ✅ callsign, speed 159.2011306493 kt, vertical rate -832, altitude 38000 identical |
| Live, both decoders on the same IQ stream, 50 s | ✅ ADS-B: 135 of 136 Python frames also found by C++, C++ found 5 more (CRC-valid); 25 aircraft addresses in both. AIS: all 9 distinct Python sentences found, C++ 1 more |
| Targets through the C++ bridge (`air_compare.py --decoder-compare`) | ✅ AIS 6 vs 6 vessels, 0 field differences; ADS-B 25 vs 25 aircraft, callsigns identical; positions equal except the last-message timing of 2 aircraft; 10/10 interface check with the all-C++ air chain |
| **Finding in the Python chain: gr-adsb `heading` is the mathematical angle atan2(v_ns, v_ew), not a compass heading** (-92.88 for a true 182.88); the bridge's `% 360` showed 267.1. C++ decoder reports the compass heading (0..360). | ⚠️ UI headings from the Python chain were mirrored; the C++ chain is right (checked: 18 of 20 differing live values satisfy B = (90 - A) mod 360, the rest are different last messages) |
| Altitude | C++ reports it from every airborne-position frame; gr-adsb only after its own CPR pair check passed (aircraft showed no altitude for longer) |
| CPU (all-C++ air chain, 20 s of 6 MSps + 250 kSps): decoder ~52 %, bridge ~56 % of one core | ✅ (Python decoder alone ~127 % of a core) |
| Not covered | ⬜ real over-the-air test of the C++ decoder (cable/HackRF) — same IQ replay only; altitude with Q=0 (Gillham) frames and airborne-position GNSS height are not decoded |

#### Step 6b — C++ air & maritime console `air_maritime/ui/air_ui` (2026-10-30)

| Item | State |
|---|---|
| Qt5 console, ZeroMQ only (SUB `air/...` + REQ commands); own copy of the ZeroMQ link in `ui/common/link.*` (no code shared with the terrestrial module) | ✅ builds clean |
| Against the live all-C++ air backend, 30 s: targets in the UI vs `air_zmq_check.py` | ✅ 6 AIS vessels (5 with position) and 25 ADS-B aircraft (10 with position) = the backend's `get_state`; 0 sequence gaps; spectrum frames 304 / 301 for 30 s (about 10/s per band) |
| Map: offline MBTiles read directly with SQLite, only stored tiles drawn, framing on the first target at the deepest zoom that has tiles | ✅ real Denmark tiles at zoom 6 (15 tiles), the 5 ships at 57.05 N 10.05 E (same place as the cable tests) |
| Map interactions (simulated mouse events) | ✅ drag 120 px east moves the centre 0.66 deg west, wheel 8 -> 9, click on a marker opens that target's popup (3465D8), hiding ships and aircraft removes all markers (4 -> 0) |
| Spectrum panels for AIS and ADS-B (mean, peak per 0.1 s, max hold, Avg, Auto Scale once at start) | ✅ checked in screenshots (AIS +/-50 kHz hump, ADS-B 4 MHz-wide recording spectrum) |
| Reconnect: bridge killed and restarted while the UI ran | ✅ LIVE -> NOT CONNECTED after 5 s -> LIVE at restart; the vessel list then follows the new bridge (0 until AIS is confirmed again) |
| Differences from the HTML page (on purpose) | no Hardware / Schedule tabs; both bands always shown (no dwell logic, IQ is continuous); markers are drawn shapes (no emoji); the JSON feed shows ZeroMQ topics instead of port numbers; Auto Scale also runs once on the first data |
| Not covered | ⬜ no tiles at zoom 7-12 outside the stored regions (same as the HTML page); visible window only checked through offscreen screenshots |

### Re-verification in this repository layout (2026-11-03)
The module was built from this repository (`tools/run_air_maritime.sh build`) and run on its own with the stand-in RTOS (`start --replay`).

| Item | State |
|---|---|
| Build of backend (Ingest, bridge, decoder, `adsb_file_test`) and console from a clean tree | ✅ |
| `tools/bench/interface_check.py` against the running module | ✅ 11/11 |
| IQ arriving on both bands; 4 vessels and 25 aircraft decoded from the real recordings | ✅ |
| Ingest: 0 unknown, 0 bad, 0 upstream loss, 0 dropped; the terrestrial frequencies of a shared RTOS are ignored (7,204 messages counted as `ignored`, none routed) | ✅ |
| Runs next to the terrestrial module (own IQ port 5571) | ✅ |
| Map tiles `data/world_zoom12.mbtiles` found by the console through the launcher | ✅ |
