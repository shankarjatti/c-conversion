# Air & maritime module — interface (contract 1.0)

This module's slice of the shared contract `../../docs/INTERFACES_V1.md`. This module talks to nothing but its own Ingest, bridge, decoder and console; its Ingest output is `tcp://127.0.0.1:5571`.
Checked by `tools/bench/interface_check.py`.

## Rules for every PUB socket
* Each message = 3 frames: **topic** (ASCII) · **header** (UTF-8 JSON object) · **payload** (raw bytes, little endian, may be empty).
* The header always has `seq` (integer, per topic, starts at 1) and `t` (unix seconds, float). A gap in `seq` = lost messages.
* Subscribers **compare topics exactly**: ZeroMQ filters by prefix, so `terr/audio/nxdn` also matches `terr/audio/nxdn48`.
  IQ topics end in `/` for that reason (`iq/nxdn/`).
* PUB/SUB gives no delivery guarantee and a late joiner misses history: state topics are re-published, and every REP
  socket answers `get_state` so a console can start with the full picture.
* Nothing in any topic describes a radio or a scheduler. The source of IQ belongs to the main project.

## Versioning
Version **1.0**. Both `get_state` replies carry `interface_version`. Allowed without a version change: new topics, new
header fields, new commands, new optional command arguments. Consumers must ignore fields and topics they do not know.
A change that removes or renames something, changes a type or a payload layout, or changes an address needs a new major version.

## [A] RTOS -> Ingest (input; the RTOS publishes, the Ingest subscribes)
`SECURECOM_RTOS_ADDR`, default `tcp://127.0.0.1:5580`. Messages arrive one by one, in any order, from any band; nothing assumes a schedule.
* topic: anything (used only if `by_topic` in the routing table names it)
* header: `centre_hz` (number, required unless routed by topic), `rate_sps` (number, checked against the route), `seq` (integer, ONE counter of the sender over all messages: a gap = lost upstream), `t` (optional)
* payload: `complex64`, interleaved I,Q float32, length a non-zero multiple of 8
Anything else is counted (`bad`, `unknown`) and dropped, never guessed.

## [B] Ingest -> module (PUB, `SECURECOM_IQ_PUB_ADDR`; each module runs its OWN Ingest instance: terrestrial `tcp://127.0.0.1:5570`, air & maritime `tcp://127.0.0.1:5571`)
| topic | header | payload |
|---|---|---|
| `iq/<band>/` | `seq` (per band), `t`, `band`, `module`, `centre_hz`, `rate_sps`, `n`, `src_seq` | the same complex64 bytes, untouched; `n` = samples |
| `ingest/status` (every 2 s) | `seq`, `t`, `in`, `routed` {band: n}, `unknown`, `ignored`, `bad`, `rate_mismatch`, `src_lost`, `out_dropped`, `band_seq` {band: n} | — |

## [C] Routing table `ingest/routes.json` (re-read every 2 s while running)
`{"by_topic": {topic: band}, "routes": [{"band", "module", "centre_hz", "tol_hz", "rate_sps"}], "ignore": [{"centre_hz", "tol_hz"}]}`. `ignore` lists the frequencies of the OTHER module: they are dropped silently and counted as `ignored` (not `unknown`). A message goes to the route whose
`centre_hz` is nearest within `tol_hz`; `rate_sps` is only checked. Terrestrial routes: dmr, nxdn, p25, tetra, nxdn48, dstar, x2tdma, provoice (200 kSps). Air & maritime routes: ais (250 kSps), adsb (6 MSps). Both modules subscribe to the same RTOS stream.

## [E] Air & maritime bridge -> consoles (PUB `tcp://127.0.0.1:5565`, REP `tcp://127.0.0.1:5566`)
| topic | rate | header (besides seq, t) | payload |
|---|---|---|---|
| `air/ais/target`, `air/adsb/target` | on update | `target` {vessel: mmsi, lat, lon, speed, course, heading, nav_status, callsign, name, msg_type, last_seen; aircraft: icao, callsign, alt, speed, heading (compass, 0-360), vertical_rate, msgs, lat, lon, last_seen} — only fields heard so far | — |
| `air/ais/remove`, `air/adsb/remove` | on expiry (AIS 10 min, ADS-B 60 s) | `mmsi` / `icao` | — |
| `air/ais/targets`, `air/adsb/targets` | 10 s | `targets` [all current records] | — |
| `air/spectrum/<ais\|adsb>` | 10/s per band | `band`, `active`, `rms_db`, `ffts`, `n_spec`, `n_peak` | float32: mean spectrum[n_spec] dB, peak[n_peak] dB (512 bins each); empty when idle |
| `air/status` | 2 s | `decoder_running`, `iq_transport`, `iq_connected` {ais, adsb}, `ais_targets`, `adsb_targets` | — |
| `air/bands` | 10 s | `bands`: [{key, name, freq_mhz, samp_rate_hz, n_spec, spectrum_topic}] | — |

Commands: `get_state` (-> `interface_version`, `status`, `bands`, `ais_vessels`, `adsb_aircraft`) · `start_decoder` (-> `decoder_running`, `note`) · `stop_decoder`. Unknown command: `ok:false` + `known`.


## [F] Internal links (not part of the contract; may change)
Decoder messages `tcp://127.0.0.1:9201` (AIS NMEA sentences, PMT), `:9202` (ADS-B decoded fields, PMT), `:9203` (raw CRC-valid Mode-S frames, PMT).
