#!/usr/bin/env python3
"""Stand-in RTOS for the TERRESTRIAL module: real recorded IQ of the 8 terrestrial bands, published the way the RTOS will publish it.

  python3 tools/bench/replay_iq.py /tmp/replay_stats.txt

ONE PUB socket (bound at SECURECOM_RTOS_ADDR, default tcp://127.0.0.1:5580), ONE generic topic "iq", 3 frames per message:
  header {"seq", "t", "centre_hz", "rate_sps", "n"}   payload complex64 (4000 samples = 20 ms at 200 kSps)
No band name in the header; the order of the messages is shuffled every 20 ms and the centre frequency jitters +-2 kHz, so the
receiver (the Ingest) must route by centre frequency alone. Each band loops a real recording, resampled to 200 kSps and scaled to a
realistic level on top of a realistic idle-channel noise floor; a band with no recording gets the noise floor only.
The recordings are NOT part of this repository: set the paths in BANDS below (or the env SECURECOM_SAMPLES for the base folder).
"""
import json
import os
import random
import sys
import time
import wave
from fractions import Fraction

import numpy as np
import zmq
from scipy import signal

FS = 200000
SIG_RMS_DBFS = -45.0     # rms of a recorded signal in the band (cable bench tests read -41 ... -50 dBFS)
NOISE_RMS_DBFS = -78.0   # idle B210 floor measured on every band
CHUNK = 4000             # 20 ms
BASE = os.environ.get('SECURECOM_SAMPLES_BASE', '/home/shankar/Downloads/tutorials/')
SRC = os.environ.get('SECURECOM_SAMPLES', '/home/shankar/Downloads/tutorials/delhi_testing/all_protocols_wav_samples/')
BANDS = [   # port, name, wav, swap I/Q, note
    (7001, 'DMR',      SRC + 'DMR/SDRSharp_20160101_231914Z_72125hz_IQ.wav', False, 'real recording'),
    (7002, 'NXDN96',   SRC + 'NXDN/NXDN96 IQ.wav', False, 'real recording'),
    (7003, 'P25',      BASE + 'sigidwiki_p25_voice/P25-C4FM-VC_IF.wav', False, 'real recording (sigidwiki)'),
    (7004, 'TETRA',    SRC + 'TETRA/TETRA IQ.wav', True, 'real recording'),
    (7005, 'NXDN48',   SRC + 'NXDN/NXDN48 IQ.wav', False, 'real recording'),
    (7006, 'DSTAR',    BASE + 'web_protocol_samples/dstar/DStar_Sound_fm_iq_48k.wav', False,
     'real off-air discriminator signal, FM-modulated to IQ'),
    (7007, 'X2TDMA',   None, False, 'no sample: noise floor only'),
    (7008, 'PROVOICE', None, False, 'no sample: noise floor only'),
]


def load_wav_iq(path, swap):
    w = wave.open(path)
    n, rate, sw = w.getnframes(), w.getframerate(), w.getsampwidth()
    raw = w.readframes(n)
    if sw == 2:
        a = np.frombuffer(raw, dtype=np.int16).reshape(-1, 2).astype(np.float32) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16)
        a = np.where(v >= 1 << 23, v - (1 << 24), v).reshape(-1, 2).astype(np.float32) / float(1 << 23)
    else:
        raise SystemExit(f'{path}: unsupported sample width {sw}')
    i, q = (a[:, 1], a[:, 0]) if swap else (a[:, 0], a[:, 1])
    return (i + 1j * q).astype(np.complex64), rate


def build(rng, wav, swap):
    secs = 30
    if wav is None:
        x = np.zeros(FS * secs, dtype=np.complex64)
    else:
        iq, rate = load_wav_iq(wav, swap)
        r = Fraction(FS, rate).limit_denominator(1000)
        x = signal.resample_poly(iq, r.numerator, r.denominator).astype(np.complex64)
        x = x - np.mean(x)                                   # the recording's own DC is not part of the signal
        x = x * (10 ** (SIG_RMS_DBFS / 20) / float(np.sqrt(np.mean(np.abs(x) ** 2))))
        reps = max(1, int(np.ceil(FS * 10 / len(x))))        # at least 10 s so the loop seam is rare
        x = np.tile(x, reps)
    sig = 10 ** (NOISE_RMS_DBFS / 20) / np.sqrt(2)
    nz = (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x))).astype(np.complex64) * np.float32(sig)
    return (x + nz).astype(np.complex64)


CENTRE_HZ = {'dmr': 446118528, 'nxdn': 451093750, 'p25': 450093750, 'tetra': 420000000, 'nxdn48': 451006250,
             'dstar': 439412500, 'x2tdma': 452000000, 'provoice': 453000000}
KEYS = {'DMR': 'dmr', 'NXDN96': 'nxdn', 'P25': 'p25', 'TETRA': 'tetra', 'NXDN48': 'nxdn48', 'DSTAR': 'dstar',
        'X2TDMA': 'x2tdma', 'PROVOICE': 'provoice'}
RTOS_ADDR = os.environ.get('SECURECOM_RTOS_ADDR', 'tcp://127.0.0.1:5580')


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/replay_stats.txt'
    rng = np.random.default_rng(1)
    bands = []
    for port, name, wav, swap, note in BANDS:
        iq = build(rng, wav, swap)
        print(f'{KEYS[name]:9s} {len(iq) / FS:5.1f} s loop  ({note})', flush=True)
        bands.append((KEYS[name], iq))
    ctx = zmq.Context()
    pub = ctx.socket(zmq.PUB)
    pub.setsockopt(zmq.SNDHWM, 2000)
    pub.setsockopt(zmq.LINGER, 0)
    pub.bind(RTOS_ADDR)
    time.sleep(1.0)                       # let the Ingest connect before the first chunk (slow joiner)
    pos = {k: 0 for k, _ in bands}
    sent = {k: 0 for k, _ in bands}
    dropped = {k: 0 for k, _ in bands}
    order_rng = random.Random(7)
    gseq, n_chunk, t0 = 0, 0, time.time()
    print(f'publishing {len(bands)} terrestrial bands at 200 kSps on ZeroMQ {RTOS_ADDR} (topic "iq"); stats -> {out}', flush=True)
    t_rep = time.time()
    while True:
        tick = list(bands)
        order_rng.shuffle(tick)
        for key, iq in tick:
            end = pos[key] + CHUNK
            block = iq[pos[key]:end] if end <= len(iq) else np.concatenate([iq[pos[key]:], iq[:end - len(iq)]])
            pos[key] = end % len(iq)
            gseq += 1
            hdr = json.dumps({'seq': gseq, 't': time.time(), 'centre_hz': CENTRE_HZ[key] + order_rng.randint(-2000, 2000),
                              'rate_sps': FS, 'n': CHUNK}).encode()
            try:
                pub.send_multipart([b'iq', hdr, block.tobytes()], flags=zmq.DONTWAIT, copy=False)
                sent[key] += 1
            except zmq.Again:
                dropped[key] += 1
        n_chunk += 1
        due = t0 + n_chunk * CHUNK / FS
        now = time.time()
        if due > now:
            time.sleep(due - now)
        if now - t_rep > 20:
            t_rep = now
            with open(out, 'w') as f:
                f.write(time.strftime('%H:%M:%S') + '\n' + '\n'.join(f"iq {k:9s} sent {sent[k] * CHUNK * 8 / 1e6:9.1f} MB dropped {dropped[k]}" for k, _ in bands) + '\n')


if __name__ == '__main__':
    main()
