# Air & maritime module — C++

AIS (162 MHz, 250 kSps) and ADS-B (1090 MHz, 6 MSps). A complete, separate module: it has its own Ingest, bridge, decoder, console, routing table,
ports and launcher, and does not need the terrestrial module.

```
RTOS --ZeroMQ--> Ingest --iq/<band>/ (tcp://127.0.0.1:5571)--> bridge + decoder --ZeroMQ PUB :5565 / REP :5566--> console (Qt5)
```
| Folder | What |
|---|---|
| `backend/ingest` | the ONE ZeroMQ subscriber of the RTOS stream; routes by centre frequency (`config/routes.json`, reloadable) |
| `backend/air_decoder` | GNU Radio C++ flowgraph: ADS-B Mode-S demodulator (CRC-24) + field decoder (compass heading), AIS chain (2 channels x 5 carrier-offset branches: FM demod, symbol sync, NRZI, HDLC, NMEA) |
| `backend/air_bridge` | AIS decoding, ADS-B target tracking with validated CPR positions, expiry (AIS 10 min / ADS-B 60 s), RF spectrum, decoder control; publishes `air/...` |
| `backend/tools/adsb_file_test` | runs the ADS-B demodulator over a complex64 file |
| `ui/air_ui` | Qt5 console: two-band RF spectrum (mean / peak / max hold), unified map over the offline tiles (pan, zoom, markers, popups, filters), target tables, ZMQ JSON feed, log |
| `data/world_zoom12.mbtiles` | offline map tiles (see `../THIRD_PARTY.md`); only stored tiles are drawn |
| `config/routes.json` | the 2 air & maritime routes; the terrestrial frequencies are in `ignore` |
| `tools/` | `run_air_maritime.sh` (build / start / status / stop / restart / ui / logs), `b210_source.py` (lab RTOS stand-in with a real B210, one band at a time), `bench/` (stand-in RTOS from recordings, interface and output checks) |
| `docs/INTERFACE.md` | this module's slice of the frozen contract 1.0 |
| `STATUS.md` | verification record with the real numbers and what is still open |

## Build and run
(Without root: apt-get download qtbase5-dev libzmq3-dev nlohmann-json3-dev libpulse-dev libsqlite3-dev libgl-dev libegl-dev libglx-dev libopengl-dev (as needed) into a folder, then dpkg -x each .deb into DEVROOT; or install the packages normally and use -DDEVROOT=/.)

Needs GNU Radio 3.10 (C++ dev files), libzmq, nlohmann-json, Qt5 and SQLite development headers (see `backend/cmake/LocalDeps.cmake` and `ui/cmake/LocalDeps.cmake`; `-DDEVROOT=/` for normal system packages).
```bash
cd air_maritime
tools/run_air_maritime.sh build
tools/run_air_maritime.sh start --replay            # real AIS + ADS-B recordings played like the RTOS (not in this repository: set SECURECOM_AIS_IQ / SECURECOM_ADSB_IQ)
tools/run_air_maritime.sh start --b210 ais          # or a real B210 on one band (ais | adsb)
tools/run_air_maritime.sh status
tools/run_air_maritime.sh ui
python3 tools/bench/interface_check.py              # check the running module against docs/INTERFACE.md
python3 tools/bench/air_zmq_check.py                # content check: targets with real fields, spectrum sizes, commands
tools/run_air_maritime.sh stop
```
To run it next to the terrestrial module on one machine both just subscribe to the same RTOS address (`SECURECOM_RTOS_ADDR`); the ports are different.
