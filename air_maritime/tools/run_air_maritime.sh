#!/usr/bin/env bash
# SecureCOM AIR & MARITIME module (AIS 162 MHz, ADS-B 1090 MHz) — C++ edge-board launcher.
#
#   RTOS --ZeroMQ--> Ingest --iq/<band>/--> bridge + decoder --ZeroMQ--> console        (results PUB :5565, commands REP :5566)
#
#   tools/run_air_maritime.sh build                    compile backend and console (cmake, no root)
#   tools/run_air_maritime.sh start [--replay | --b210 ais|adsb [--gain dB]] [--test]
#        --replay  stand-in RTOS from real recordings (tools/bench/replay_iq.py; the recordings are not in this repository)
#        --b210 ais|adsb   stand-in RTOS from a real USRP B210 (tools/b210_source.py): ONE band at a time (AIS 162 MHz @ 250 kSps or ADS-B 1090 MHz @ 6 MSps)
#   tools/run_air_maritime.sh status | stop | restart [...] | ui | logs [name]
#
# Environment: SECURECOM_RTOS_ADDR (where the RTOS publishes, default tcp://127.0.0.1:5580), SECURECOM_IQ_PUB_ADDR (the Ingest's output,
# default tcp://127.0.0.1:5571: this module has its own port, so it can run next to the terrestrial module), SECURECOM_EDGE_RUN (scratch dir).
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${SECURECOM_EDGE_RUN:-/tmp/securecom_air_maritime}"
BUILD="$HERE/backend/build"
UIB="$HERE/ui/build"
RTOS_ADDR="${SECURECOM_RTOS_ADDR:-tcp://127.0.0.1:5580}"
IQ_ADDR="${SECURECOM_IQ_PUB_ADDR:-tcp://127.0.0.1:5571}"
export PATH=/usr/local/bin:/usr/bin:/bin
BIN_INGEST="$BUILD/ingest/securecom_ingest"
BIN_TB="$BUILD/air_bridge/securecom_air_bridge"
BIN_TD="$BUILD/air_decoder/securecom_air_decoder"
BIN_UI="$UIB/air_ui/air_ui"
TILES="$HERE/data/world_zoom12.mbtiles"
TCP_PORTS="5565 5566 ${IQ_ADDR##*:}"

c_ok() { printf '\033[32m%s\033[0m' "$*"; }
c_bad() { printf '\033[31m%s\033[0m' "$*"; }
say() { echo "[air_maritime] $*"; }
alive() { [ -f "$RUN/$1.pid" ] && kill -0 "$(cat "$RUN/$1.pid")" 2>/dev/null; }
pids_of() { local b; b="$(readlink -f "$1")"; for d in /proc/[0-9]*; do [ "$(readlink "$d/exe" 2>/dev/null)" = "$b" ] && echo "${d#/proc/}"; done; }

cmd_build() {
  say "building the backend ..."
  cmake -S "$HERE/backend" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$BUILD" -j"$(nproc)" 2>&1 | grep -E "error|warning|Built target"
  say "building the console ..."
  cmake -S "$HERE/ui" -B "$UIB" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$UIB" -j"$(nproc)" 2>&1 | grep -E "error|warning|Built target"
}

preflight() {
  local miss=0 busy=0 b p
  for b in "$BIN_INGEST" "$BIN_TB" "$BIN_TD"; do [ -x "$b" ] || { say "missing $b"; miss=1; }; done
  [ $miss = 0 ] || { say "binaries missing: run  $0 build"; return 1; }
  for p in $TCP_PORTS; do ss -ltn 2>/dev/null | grep -q "[:.]$p " && { say "TCP port $p is already in use"; busy=1; }; done
  [ -f "$TILES" ] || say "warning: $TILES missing: the console shows no map"
  return $busy
}

start_bg() {   # start_bg NAME DIR ENV... -- COMMAND...   (detached; the pid file holds the REAL process)
  local name=$1 dir=$2; shift 2
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done
  shift
  setsid nohup bash -c 'echo $$ >"$0"; cd "$1"; shift; exec env "$@"' "$RUN/$name.pid" "$dir" "${envs[@]}" "$@" >"$RUN/$name.log" 2>&1 </dev/null &
}
wait_port() { for _ in $(seq 1 "${2:-30}"); do ss -ltn 2>/dev/null | grep -q "[:.]$1 " && return 0; sleep 0.5; done; return 1; }
cmd_ctl() {
  python3 - "$1" "$2" <<'PY'
import json, sys, zmq
c = zmq.Context(); r = c.socket(zmq.REQ); r.setsockopt(zmq.RCVTIMEO, 20000); r.setsockopt(zmq.LINGER, 0)
r.connect(f'tcp://127.0.0.1:{sys.argv[1]}'); r.send(sys.argv[2].encode())
print(json.dumps(json.loads(r.recv_multipart()[0])))
PY
}

cmd_start() {
  local replay=0 test=0 b210=0 band="" gain=40 freq=0 rate=0 n
  while [ $# -gt 0 ]; do case "$1" in --replay) replay=1;; --test) test=1;; --b210) b210=1; band=${2:-}; shift;; --gain) gain=$2; shift;; *) say "unknown option $1"; return 2;; esac; shift; done
  [ $replay = 1 ] && [ $b210 = 1 ] && { say "--replay and --b210 both publish on the RTOS address: choose one"; return 2; }
  [ $b210 = 1 ] && case "$band" in ais) freq=162.0; rate=250000;; adsb) freq=1090.0; rate=6000000;; *) say "--b210 needs the band: ais or adsb"; return 2;; esac
  mkdir -p "$RUN"
  for n in ingest air_bridge replay b210; do alive "$n" && { say "already running ($n); use stop or restart"; return 1; }; done
  preflight || return 1
  if [ $replay = 1 ]; then
    say "starting the stand-in RTOS (real AIS + ADS-B recordings, one topic, random order) ..."
    start_bg replay "$HERE" SECURECOM_RTOS_ADDR="$RTOS_ADDR" -- python3 -u tools/bench/replay_iq.py "$RUN/replay_stats.txt"
    sleep 1; for _ in $(seq 1 120); do grep -q 'publishing' "$RUN/replay.log" 2>/dev/null && break; sleep 1; done
    grep -q 'publishing' "$RUN/replay.log" 2>/dev/null || { say "replay did not come up:"; tail -3 "$RUN/replay.log"; cmd_stop >/dev/null; return 1; }
  fi
  if [ $b210 = 1 ]; then
    say "starting the B210 source: $band, $freq MHz, $rate Sps, gain $gain dB ..."
    start_bg b210 "$HERE" LD_LIBRARY_PATH=/usr/local/lib -- python3 -u tools/b210_source.py --addr "$RTOS_ADDR" --freq "$freq" --rate "$rate" --gain "$gain"
    sleep 1; for _ in $(seq 1 30); do grep -q 'publishing on' "$RUN/b210.log" 2>/dev/null && break; [ -f "$RUN/b210.pid" ] && ! alive b210 && break; sleep 1; done
    grep -q 'publishing on' "$RUN/b210.log" 2>/dev/null || { say "the B210 did not start:"; tail -3 "$RUN/b210.log"; cmd_stop >/dev/null; return 1; }
    tail -2 "$RUN/b210.log"
  fi
  say "starting the Ingest ($RTOS_ADDR -> $IQ_ADDR) ..."
  start_bg ingest "$HERE" SECURECOM_RTOS_ADDR="$RTOS_ADDR" SECURECOM_IQ_PUB_ADDR="$IQ_ADDR" -- "$BIN_INGEST" "$HERE/config/routes.json"
  wait_port "${IQ_ADDR##*:}" 20 || { say "Ingest did not come up (see $RUN/ingest.log)"; return 1; }
  say "starting the bridge ..."
  start_bg air_bridge "$HERE" SECURECOM_IQ_PUB_ADDR="$IQ_ADDR" SECURECOM_AIR_DECODER_BIN="$BIN_TD" SECURECOM_DECODER_DIR="$HERE" -- "$BIN_TB"
  wait_port 5566 20 || { say "bridge did not come up (see $RUN/air_bridge.log)"; return 1; }
  say "starting the decoder (through the bridge) ..."
  echo "  $(cmd_ctl 5566 '{"cmd":"start_decoder"}')"
  say "up.  Check:  $0 status     Console:  $0 ui     Stop:  $0 stop"
}

cmd_stop() {
  mkdir -p "$RUN"
  local n pid b
  alive ui && kill "$(cat "$RUN/ui.pid")" 2>/dev/null
  alive air_bridge && kill "$(cat "$RUN/air_bridge.pid")" 2>/dev/null     # the bridge stops the decoder it started
  for _ in $(seq 1 20); do alive air_bridge || break; sleep 0.5; done
  for n in ingest replay b210; do alive "$n" && kill "$(cat "$RUN/$n.pid")" 2>/dev/null; done
  sleep 1
  for b in "$BIN_TD" "$BIN_TB" "$BIN_INGEST"; do for pid in $(pids_of "$b"); do say "stopping leftover $(basename "$b") ($pid)"; kill "$pid" 2>/dev/null; done; done
  sleep 1
  for b in "$BIN_TD" "$BIN_TB" "$BIN_INGEST"; do for pid in $(pids_of "$b"); do kill -9 "$pid" 2>/dev/null; done; done
  rm -f "${RUN:?}"/*.pid
  say "stopped"
}

cmd_status() {
  mkdir -p "$RUN"
  printf '%-14s %-9s %-8s %-6s %-8s\n' component state pid cpu% rss_MB
  local n pid line pids
  for n in replay b210 ingest air_bridge; do
    if alive "$n"; then pid=$(cat "$RUN/$n.pid"); line=$(ps -o pcpu=,rss= -p "$pid" | awk '{printf "%-6s %-8.0f",$1,$2/1024}'); printf '%-14s %-9s %-8s %s\n' "$n" "$(c_ok running)" "$pid" "$line"
    else printf '%-14s %-9s\n' "$n" "$( { [ $n = replay ] || [ $n = b210 ]; } && echo '-' || c_bad stopped)"; fi
  done
  pids=$(pids_of "$BIN_TD" | tr '\n' ' ')
  if [ -n "$pids" ]; then pid=${pids%% *}; line=$(ps -o pcpu=,rss= -p "$pid" | awk '{printf "%-6s %-8.0f",$1,$2/1024}'); printf '%-14s %-9s %-8s %s\n' air_decoder "$(c_ok running)" "$pid" "$line"
  else printf '%-14s %-9s\n' air_decoder "$(c_bad stopped)"; fi
  alive ui && printf '%-14s %-9s %-8s\n' console "$(c_ok running)" "$(cat "$RUN/ui.pid")"
  echo
  IQ_PORT="${IQ_ADDR##*:}" python3 - <<'PY'
import json, os, time, zmq
ctx = zmq.Context()
def probe(addr, prefix, secs=4.0):
    s = ctx.socket(zmq.SUB); s.setsockopt(zmq.RCVHWM, 100000); s.connect(addr); s.setsockopt(zmq.SUBSCRIBE, prefix.encode())
    n, t0 = 0, time.time()
    while time.time() - t0 < secs:
        if s.poll(200):
            s.recv_multipart(); n += 1
    s.close(0)
    return n / secs
def ctl(addr, req):
    r = ctx.socket(zmq.REQ); r.setsockopt(zmq.RCVTIMEO, 3000); r.setsockopt(zmq.LINGER, 0); r.connect(addr)
    try:
        r.send(json.dumps(req).encode()); return json.loads(r.recv_multipart()[0])
    except zmq.Again:
        return None
    finally:
        r.close(0)
print(f"{'stream':14s} {'answers':8s} {'msgs/s':>8s}  details")
rate = probe('tcp://127.0.0.1:5565', 'air/')
g = ctl('tcp://127.0.0.1:5566', {'cmd': 'get_state'})
det = ''
if g and g.get('ok'):
    s2 = g['status']; ic = s2.get('iq_connected', {})
    det = f"decoder {'RUNNING' if s2.get('decoder_running') else 'stopped'}, IQ arriving {sum(ic.values())}/{len(ic)} bands, {len(g.get('ais_vessels', []))} vessels, {len(g.get('adsb_aircraft', []))} aircraft"
print(f"{'air & maritime':14s} {'yes' if g else 'NO':8s} {rate:8.0f}  {det}")
s = ctx.socket(zmq.SUB); s.connect('tcp://127.0.0.1:' + os.environ['IQ_PORT']); s.setsockopt(zmq.SUBSCRIBE, b'ingest/status')
if s.poll(3500):
    h = json.loads(s.recv_multipart()[1])
    print(f"{'ingest':14s} {'yes':8s} {'':8s}  routed {sum(h['routed'].values())} messages to {len(h['routed'])} bands, unknown {h['unknown']}, ignored (other module) {h.get('ignored', 0)}, bad {h['bad']}, upstream loss {h['src_lost']}, dropped {h['out_dropped']}")
else:
    print(f"{'ingest':14s} {'NO':8s}")
PY
}

cmd_ui() {
  [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || { say "no desktop session (DISPLAY/WAYLAND_DISPLAY not set): run this from a terminal on the console machine"; return 1; }
  [ -x "$BIN_UI" ] || { say "console not built: run $0 build"; return 1; }
  mkdir -p "$RUN"
  alive ui && { say "console already open"; return 0; }
  start_bg ui "$HERE" X=1 -- "$BIN_UI" --tiles "$TILES"
  say "console started"
}
cmd_logs() { tail -n 40 "$RUN/${1:-air_bridge}.log"; }

case "${1:-}" in
  build) cmd_build;;
  start) shift; cmd_start "$@";;
  stop) cmd_stop;;
  restart) shift; cmd_stop; sleep 1; cmd_start "$@";;
  status) cmd_status;;
  ui) cmd_ui;;
  logs) shift; cmd_logs "$@";;
  *) sed -n 2,15p "$0" | sed 's/^# \{0,1\}//'; exit 2;;
esac
