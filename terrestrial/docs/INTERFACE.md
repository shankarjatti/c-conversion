# Terrestrial module — interface (contract 1.0)

This module's slice of the shared contract `../../docs/INTERFACES_V1.md`. This module talks to nothing but its own Ingest, bridge, decoder and console; its Ingest output is `tcp://127.0.0.1:5570`.
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

## [D] Terrestrial bridge -> consoles (PUB `tcp://127.0.0.1:5560`, REP `tcp://127.0.0.1:5561`)
| topic | rate | header (besides seq, t) | payload |
|---|---|---|---|
| `terr/audio/<band>` | 100-190/s per band (tetra ~20) | `band`, `rate` (Hz), `active`, `rms_db`, `n` | int16 mono, `n` samples (48 kHz bands sent at 24 kHz, TETRA 8 kHz) |
| `terr/iq/<band>` | 10/s per band | `band`, `active`, `rms_db`, `floor_db` (number or null), `n_spec`(256), `n_time`(64), `n_chan`, `chan_best`, `chan_best_db`, `chan_occupied` [int], `chan_bw_hz` | float32: spectrum[n_spec] dB, time[n_time], chan_db[n_chan] |
| `terr/status` | 2 s | `source`, `decoder_running`, `iq_connected` {band: bool}, `iq_transport`, `bands` | — |
| `terr/bands` | 10 s | `bands`: [{key, name, freq_mhz, iq_port, iq_rate_sps, channels, chan_bw_hz, audio_topic, audio_rate_hz, iq_topic}] | — |
| `terr/decoder` | on change | `running` | — |
| `terr/log` | ~45/s | `band` (or null), `text`, `t` | — |
| `terr/recording` | per saved clip | `band`, `file`, `duration`, `had_audio`, `peak_db`, `t` | — |
| `terr/recordings` | 10 s + on change | `total`, `items` (newest 200: file, band, bytes, mtime, duration) | — |
| `terr/tetra` | on event | `data` (call-log dict) | — |

Commands (request: one JSON frame `{"cmd": ...}`; reply: 2 frames `[JSON header, payload]`, header has `ok`, and `error` when not ok):
`get_state` (-> `interface_version`, `status`, `bands`, `recordings_total`) · `start_decoder` / `stop_decoder` (-> `decoder_running`) ·
`list_recordings` (`offset`, `limit` -> `total`, `offset`, `items`) · `get_recording` (`file`, `offset`, `length` <= 1 MiB -> `file`, `size`, `offset`, `length`, `eof` + bytes) ·
`delete_recording` (`file` -> `deleted`) · `delete_all_recordings` (-> `deleted`). File names are bare `.wav` names inside the recordings folder; anything else is refused. An unknown command gets `ok:false` and `known`.


## [F] Internal links (not part of the contract; may change)
Decoder audio taps `tcp://127.0.0.1:9101-9109` (float32), TETRA call-log events `:9105` (PMT), tetra-kit reports on UDP `127.0.0.1:42100` (one TETRA decoder per machine).
