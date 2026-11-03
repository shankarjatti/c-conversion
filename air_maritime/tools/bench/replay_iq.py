#!/usr/bin/env python3
"""Stand-in RTOS for the AIR & MARITIME module: real recorded AIS and ADS-B IQ, published the way the RTOS will publish it.

  python3 tools/bench/replay_iq.py /tmp/replay_stats.txt

ONE PUB socket (bound at SECURECOM_RTOS_ADDR, default tcp://127.0.0.1:5580), ONE generic topic "iq", 3 frames per message:
  header {"seq", "t", "centre_hz", "rate_sps", "n"}   payload complex64 (20 ms of IQ: 5000 samples AIS @ 250 kSps, 120000 ADS-B @ 6 MSps)
No band name in the header; the order is shuffled every 20 ms and the centre frequency jitters +-2 kHz (route by centre frequency alone).
The recordings are real and played as recorded; they are NOT part of this repository (set SECURECOM_AIS_IQ / SECURECOM_ADSB_IQ):
  AIS   a stereo 16-bit I/Q WAV at 250 kSps (a B210 cable capture of real AIS)
  ADS-B a complex64 file at 6 MSps (a real 1090 MHz recording resampled to 6 MSps)
"""
import json
import os
import random
import sys
import time
import wave

import numpy as np
import zmq

AIS_IQ = os.environ.get('SECURECOM_AIS_IQ', '/home/shankar/Downloads/tutorials/delhi_testing/all_protocols_wav_samples/AIS_Marine/ais_b210_cable_capture_250k.wav')
ADSB_IQ = os.environ.get('SECURECOM_ADSB_IQ', '/home/shankar/am_gt/adsb_real_6M_10s.cf32')
SRC = {'ais': (AIS_IQ, 250000, 162000000), 'adsb': (ADSB_IQ, 6000000, 1090000000)}
RTOS_ADDR = os.environ.get('SECURECOM_RTOS_ADDR', 'tcp://127.0.0.1:5580')


def load(path):
    if path.endswith('.wav'):
        w = wave.open(path)
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2).astype(np.float32) / 32768.0
        return (a[:, 0] + 1j * a[:, 1]).astype(np.complex64)
    return np.fromfile(path, dtype=np.complex64)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/replay_stats.txt'
    bands = {}
    for k, (path, rate, fc) in SRC.items():
        iq = load(path)
        print(f'{k:5s} {len(iq) / rate:5.1f} s loop  (real recording, as recorded)', flush=True)
        bands[k] = iq
    ctx = zmq.Context()
    pub = ctx.socket(zmq.PUB)
    pub.setsockopt(zmq.SNDHWM, 2000)
    pub.setsockopt(zmq.LINGER, 0)
    pub.bind(RTOS_ADDR)
    time.sleep(1.0)
    pos = {k: 0 for k in bands}
    sent = {k: 0 for k in bands}
    dropped = {k: 0 for k in bands}
    order_rng = random.Random(7)
    gseq, n_chunk, t0, t_rep = 0, 0, time.time(), time.time()
    print(f'publishing AIS 250 kSps + ADS-B 6 MSps on ZeroMQ {RTOS_ADDR} (topic "iq"); stats -> {out}', flush=True)
    while True:
        tick = list(bands)
        order_rng.shuffle(tick)
        for key in tick:
            iq, (_, rate, fc) = bands[key], SRC[key]
            chunk = rate // 50
            end = pos[key] + chunk
            block = iq[pos[key]:end] if end <= len(iq) else np.concatenate([iq[pos[key]:], iq[:end - len(iq)]])
            pos[key] = end % len(iq)
            gseq += 1
            hdr = json.dumps({'seq': gseq, 't': time.time(), 'centre_hz': fc + order_rng.randint(-2000, 2000), 'rate_sps': rate, 'n': chunk}).encode()
            try:
                pub.send_multipart([b'iq', hdr, block.tobytes()], flags=zmq.DONTWAIT, copy=False)
                sent[key] += 1
            except zmq.Again:
                dropped[key] += 1
        n_chunk += 1
        due = t0 + n_chunk * 0.02
        now = time.time()
        if due > now:
            time.sleep(due - now)
        if now - t_rep > 20:
            t_rep = now
            with open(out, 'w') as f:
                f.write(time.strftime('%H:%M:%S') + '\n' + '\n'.join(f"iq {k:5s} sent {sent[k] * SRC[k][1] // 50 * 8 / 1e6:9.1f} MB dropped {dropped[k]}" for k in bands) + '\n')


if __name__ == '__main__':
    main()
