# SecureCOM — C++ conversion

The SecureCOM edge-board software rewritten in C++ (backend and consoles), organised as **two completely separate modules**:

| Module | Signals | Results / commands | Folder |
|---|---|---|---|
| **Terrestrial** | DMR, NXDN96, P25, TETRA, NXDN48, D-STAR, X2-TDMA, ProVoice | PUB `:5560` / REP `:5561` | [`terrestrial/`](terrestrial/) |
| **Air & maritime** | AIS, ADS-B | PUB `:5565` / REP `:5566` | [`air_maritime/`](air_maritime/) |

Each module holds its own Ingest, bridge, decoder, console (Qt5), routing table, launcher, tools, interface document and status log, and shares no code or
files with the other; either one runs alone. Both are fed by the same RTOS stream:

```
USRP 2945 -> RTOS --ZeroMQ--> terrestrial Ingest --> bridge + decoder --ZeroMQ--> terrestrial console
                      \------> air & maritime Ingest --> bridge + decoder --ZeroMQ--> air & maritime console
```
Every link is ZeroMQ (publish/subscribe for data, request/reply for commands); there is no WebSocket. The Ingest of each module is the single subscriber of the
RTOS stream and routes each IQ message by the centre frequency in its header — nothing assumes a schedule or an order. The interface is frozen as
**version 1.0**: the shared contract is [`docs/INTERFACES_V1.md`](docs/INTERFACES_V1.md), each module has its slice in `docs/INTERFACE.md` and an automatic checker
(`tools/bench/interface_check.py`).

## Verified (details and numbers in each module's `STATUS.md`)
* Each C++ part was compared with the Python original it replaces on the same input: Ingest output byte-identical; bridges identical; ADS-B demodulator finds the
  same frames; DSD chains decode the same amount of voice offline (within 0.02 %); live, the terrestrial decoder matches on most bands.
* Both consoles were checked against the live backend (counts, zoom/pan, recordings, reconnect, map).
* First live run with a real USRP B210 (DMR): real voice decoded.
* Each module was built and run on its own from this repository layout; both run side by side.

## Not verified / open
* Live NXDN yields about 5 % less voice than the Python decoder (cause not found; identical offline).
* The terrestrial console's sound output (Listen, recording playback) has never been played.
* No over-the-air test of the air & maritime decoders with the C++ code; X2-TDMA and ProVoice have no real sample.
* The real RTOS header field names and the meaning of its sequence number are still to be agreed with the RTOS team (only stand-ins were used).

## Dependencies and licences
GNU Radio 3.10 (C++), gr-dsd, libzmq, nlohmann-json, Qt5, PulseAudio (terrestrial console), SQLite (air console) — see each module's README. Third-party items shipped
here (prebuilt tetra-kit libraries, map tiles) and the licence situation: [`THIRD_PARTY.md`](THIRD_PARTY.md).
