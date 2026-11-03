# Terrestrial module — C++

DMR, NXDN96, P25 phase 1, TETRA, NXDN48, D-STAR, X2-TDMA and ProVoice. A complete, separate module: it has its own Ingest, bridge,
decoder, console, routing table, ports and launcher, and does not need the air & maritime module.

```
RTOS --ZeroMQ--> Ingest --iq/<band>/ (tcp://127.0.0.1:5570)--> bridge + decoder --ZeroMQ PUB :5560 / REP :5561--> console (Qt5)
```
| Folder | What |
|---|---|
| `backend/ingest` | the ONE ZeroMQ subscriber of the RTOS stream; routes each message by the centre frequency in its header (`config/routes.json`, reloadable) |
| `backend/terr_decoder` | GNU Radio C++ flowgraph: 7 DSD chains (channelizer, resamplers, squelch, FM demod, gr-dsd) + the TETRA chain (tetra-kit); audio taps 9101-9109, TETRA events 9105 |
| `backend/terr_bridge` | spectrum, band activity, per-channel power, audio taps, voice recordings (WAV), TETRA events, decoder control; publishes `terr/...` and serves commands |
| `ui/terr_ui` | Qt5 console: spectrum / waterfall / time plots with zoom, Signals tables, TETRA call log, audio meters, log, Recordings (list, play, save, delete), Start/Stop decoder, Listen |
| `config/routes.json` | the 8 terrestrial routes; the air & maritime frequencies are in `ignore` |
| `third_party/tetra_runtime` | prebuilt tetra-kit libraries in the layout the decoder expects (see `../THIRD_PARTY.md`) |
| `tools/` | `run_terrestrial.sh` (build / start / status / stop / restart / ui / logs), `b210_source.py` (lab RTOS stand-in with a real USRP B210), `bench/` (stand-in RTOS from recordings, interface check, voice measurements, offline DSD chain reference) |
| `docs/INTERFACE.md` | this module's slice of the frozen contract 1.0 |
| `STATUS.md` | verification record with the real numbers and what is still open |

## Build and run
(Without root: apt-get download qtbase5-dev libzmq3-dev nlohmann-json3-dev libpulse-dev libsqlite3-dev libgl-dev libegl-dev libglx-dev libopengl-dev (as needed) into a folder, then dpkg -x each .deb into DEVROOT; or install the packages normally and use -DDEVROOT=/.)

Needs GNU Radio 3.10 (C++ dev files), gr-dsd (`/usr/local/lib/x86_64-linux-gnu/libgnuradio-dsd.so`), libzmq, nlohmann-json, Qt5 and PulseAudio development headers.
Without root the headers can be unpacked into a private prefix (`~/Downloads/tutorials/cpp_ui_devroot/prefix`, see `backend/cmake/LocalDeps.cmake`); with
normal system packages use `-DDEVROOT=/`.
```bash
cd terrestrial
tools/run_terrestrial.sh build
tools/run_terrestrial.sh start --b210 --test        # a real USRP B210 on DMR 446.118528 MHz (a stand-in for the RTOS) ...
tools/run_terrestrial.sh start --replay --test      # ... or real recordings played like the RTOS (the recordings are not in this repository)
tools/run_terrestrial.sh status                     # processes, IQ per band, Ingest counters
tools/run_terrestrial.sh ui                         # open the console (Listen is off: no sound until you switch it on)
python3 tools/bench/interface_check.py              # check the running module against docs/INTERFACE.md
tools/run_terrestrial.sh stop
```
In the real system the RTOS publishes the IQ (`docs/INTERFACE.md` [A]); the B210 and the replay are lab stand-ins that publish the same way.
