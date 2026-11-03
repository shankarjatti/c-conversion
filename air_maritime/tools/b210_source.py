#!/usr/bin/env python3
"""Lab stand-in for the RTOS with a real USRP B210 for the AIR & MARITIME module: continuous IQ of ONE band (AIS or ADS-B), published the way the RTOS will publish it.

  python3 tools/b210_source.py --freq 162.0 --rate 250000 [--gain 40]   (AIS)    or    --freq 1090.0 --rate 6000000 [--gain 40]   (ADS-B)    [--antenna RX2] [--addr tcp://127.0.0.1:5580]

One PUB socket (bound at --addr, which the Ingest subscribes to), topic "iq", 3 frames per message:
  header {"centre_hz", "rate_sps", "seq", "t", "n"}   payload complex64, n = rate/50 samples (20 ms of IQ: 5000 for AIS, 120000 for ADS-B)
The Ingest routes it by centre_hz (a route within +-50 kHz of the tuned frequency). Nothing is invented: when the B210 reports an
overflow the samples are lost, `seq` skips one number (the Ingest counts it as upstream loss) and the overflow is printed.
Every 5 s: level (rms dBFS), peak, clipped samples, messages sent, overflows. Gain: the level line shows rms/peak/clipped samples; reduce the gain if samples clip.
Needs UHD python (system package) and the B210 on USB 3. Run with PATH=/usr/local/bin:/usr/bin:/bin (UHD 4.10).
"""
import argparse
import json
import signal
import sys
import time

import numpy as np
import zmq

ap = argparse.ArgumentParser()
ap.add_argument('--freq', type=float, required=True, help='centre frequency, MHz (162.0 for AIS, 1090.0 for ADS-B)')
ap.add_argument('--gain', type=float, default=40.0, help='RX gain, dB')
ap.add_argument('--rate', type=float, required=True, help='sample rate, Sps (250000 for AIS, 6000000 for ADS-B: what the decoder expects)')
ap.add_argument('--antenna', default='RX2')
ap.add_argument('--args', default='', help='UHD device args, e.g. type=b200')
ap.add_argument('--addr', default='tcp://127.0.0.1:5580')
a = ap.parse_args()

try:
    import uhd
    usrp = uhd.usrp.MultiUSRP(a.args)
except Exception as e:                       # no device: say so plainly, never pretend
    sys.exit(f'[b210] cannot open the B210: {e}\n[b210] connect it (USB 3) and check:  PATH=/usr/local/bin:/usr/bin:/bin uhd_find_devices')

usrp.set_rx_rate(a.rate, 0)
usrp.set_rx_freq(uhd.types.TuneRequest(a.freq * 1e6), 0)
usrp.set_rx_gain(a.gain, 0)
usrp.set_rx_antenna(a.antenna, 0)
time.sleep(0.5)
print(f'[b210] {usrp.get_mboard_name()}: rate {usrp.get_rx_rate(0) / 1e3:.1f} kSps, freq {usrp.get_rx_freq(0) / 1e6:.6f} MHz, '
      f'gain {usrp.get_rx_gain(0):.1f} dB, antenna {usrp.get_rx_antenna(0)}', flush=True)
if abs(usrp.get_rx_rate(0) - a.rate) > 1:
    sys.exit(f'[b210] the radio cannot run at {a.rate} Sps (got {usrp.get_rx_rate(0)})')

ctx = zmq.Context()
pub = ctx.socket(zmq.PUB)
pub.setsockopt(zmq.SNDHWM, 2000)
pub.setsockopt(zmq.LINGER, 0)
pub.bind(a.addr)
time.sleep(1.0)                              # let the Ingest connect before the first message (slow joiner)

streamer = usrp.get_rx_stream(uhd.usrp.StreamArgs('fc32', 'sc16'))
cmd = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
cmd.stream_now = True
streamer.issue_stream_cmd(cmd)
md = uhd.types.RXMetadata()
N = int(a.rate // 50)                        # 20 ms per message
buf = np.zeros((1, N * 4), dtype=np.complex64)
pend = np.zeros(0, dtype=np.complex64)
seq, sent, over, drop, clip = 0, 0, 0, 0, 0
peak, ssum, scnt = 0.0, 0.0, 0
t_rep = time.time()
stop = False
signal.signal(signal.SIGINT, lambda *_: globals().update(stop=True))
signal.signal(signal.SIGTERM, lambda *_: globals().update(stop=True))
centre = int(round(usrp.get_rx_freq(0)))
print(f'[b210] publishing on {a.addr} as topic "iq", centre_hz {centre}', flush=True)
while not stop:
    n = streamer.recv(buf, md, 1.0)
    if md.error_code == uhd.types.RXMetadataErrorCode.overflow:
        over += 1
        seq += 1                             # a number is skipped on purpose: downstream counts the loss
        pend = pend[:0]                      # partial samples before a gap would be misaligned: drop them
        print('[b210] overflow (samples lost; USB 3 port / cable / CPU?)', flush=True)
        continue
    if md.error_code not in (uhd.types.RXMetadataErrorCode.none, uhd.types.RXMetadataErrorCode.timeout):
        print(f'[b210] stream error: {md.strerror()}', flush=True)
        continue
    if n:
        pend = np.concatenate([pend, buf[0, :n]])
    while len(pend) >= N:
        chunk, pend = pend[:N], pend[N:]
        seq += 1
        hdr = json.dumps({'centre_hz': centre, 'rate_sps': int(a.rate), 'seq': seq, 't': time.time(), 'n': N}).encode()
        try:
            pub.send_multipart([b'iq', hdr, chunk.tobytes()], flags=zmq.DONTWAIT, copy=False)
            sent += 1
        except zmq.Again:
            drop += 1
        mag = np.abs(chunk)
        peak = max(peak, float(mag.max()))
        clip += int(np.count_nonzero(mag > 0.98))
        ssum += float(np.mean(mag * mag))
        scnt += 1
    if time.time() - t_rep >= 5:
        rms = 10 * np.log10(max(ssum / max(scnt, 1), 1e-20)) / 1.0
        print(f'[b210] level {rms:6.1f} dBFS rms, peak {20 * np.log10(max(peak, 1e-9)):6.1f} dBFS, clipped {clip}, sent {sent}, '
              f'overflows {over}, send drops {drop}', flush=True)
        peak, ssum, scnt, clip, t_rep = 0.0, 0.0, 0, 0, time.time()
streamer.issue_stream_cmd(uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
print('[b210] stopped', flush=True)
