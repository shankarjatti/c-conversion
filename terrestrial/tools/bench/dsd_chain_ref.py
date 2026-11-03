#!/usr/bin/env python3
"""Offline reference for ONE terrestrial DSD chain, built from the same GNU Radio blocks and parameters as allinone.grc,
file in -> file out (no ZeroMQ, no real time):

  python3 tools/bench/dsd_chain_ref.py make-iq nxdn /tmp/nxdn_iq.cf32 [seconds=30]     # the replay's IQ for that band
  python3 tools/bench/dsd_chain_ref.py run nxdn /tmp/nxdn_iq.cf32 /tmp/nxdn_py.f32      # Python flowgraph
  SECURECOM_TERR_TEST_BAND=nxdn SECURECOM_TERR_TEST_IN=... SECURECOM_TERR_TEST_OUT=... cpp_backend/build/terr_decoder/securecom_terr_decoder
  python3 tools/bench/dsd_chain_ref.py compare /tmp/nxdn_py.f32 /tmp/nxdn_cpp.f32
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
CFG = {   # band: nchans, interp, decim, lpf cutoff, lpf width, frame, mod, volume   (allinone.grc)
    'dmr': (16, 96, 25, 6250, 2000, 'DMR_MOTOTRBO', 'C4FM', 0.2),
    'nxdn': (16, 96, 25, 6250, 2000, 'NXDN96', 'GFSK', 0.5),
    'p25': (10, 12, 5, 9500, 2000, 'P25_PHASE_1', 'C4FM', 0.5),
    'nxdn48': (32, 192, 25, 3000, 1000, 'NXDN48_IDAS', 'GFSK', 3.0),
    'dstar': (16, 96, 25, 6250, 2000, 'DSTAR', 'GFSK', 0.5),
    'x2tdma': (16, 96, 25, 6250, 2000, 'X2_TDMA', 'C4FM', 0.5),
    'provoice': (8, 48, 25, 9500, 2000, 'PROVOICE', 'GFSK', 0.5),
}


def make_iq(band, path, secs):
    import replay_iq as rb
    key = {v: k for k, v in rb.KEYS.items()}[band]
    rng = np.random.default_rng(1)                      # the live replay builds ALL bands from this one sequence, in order
    iq = None
    for e in rb.BANDS:
        x = rb.build(rng, e[2], e[3])
        if e[1] == key:
            iq = x
            break
    n = int(secs * rb.FS)
    iq = np.tile(iq, int(np.ceil(n / len(iq))))[:n]
    iq.astype(np.complex64).tofile(path)
    print(f'{path}: {len(iq)} samples ({len(iq) / rb.FS:.1f} s) of the replay IQ for {band}')


def run(band, src, dst):
    import dsd
    from gnuradio import analog, blocks, filter, gr
    from gnuradio.fft import window
    from gnuradio.filter import firdes, pfb
    nch, interp, decim, cut, wid, frame, mod, vol = CFG[band]
    fs = 200000

    class TB(gr.top_block):
        def __init__(s):
            gr.top_block.__init__(s)
            f = blocks.file_source(gr.sizeof_gr_complex, src, False)
            f.set_max_output_buffer(1024)
            ch = pfb.channelizer_ccf(nch, firdes.low_pass(float(nch), fs, (fs / float(nch)) / 2.0, 1000), 1.0, 100)
            ch.set_channel_map([])
            ch.declare_sample_delay(0)
            ch.set_max_output_buffer(1024)
            sm = blocks.add_vcc(1)
            sm.set_max_output_buffer(1024)
            rs = filter.rational_resampler_ccf(interpolation=interp, decimation=decim, taps=[], fractional_bw=0.4)
            lp = filter.fir_filter_ccf(1, firdes.low_pass(1.0, 48000, cut, wid, window.WIN_HAMMING, 6.76))
            sq = analog.simple_squelch_cc(-73, 0.05)
            fm = analog.quadrature_demod_cf(1.0)
            d = dsd.dsd_block_ff(getattr(dsd, 'dsd_FRAME_' + frame), getattr(dsd, 'dsd_MOD_' + mod), 3, True, 0)
            ar = filter.rational_resampler_fff(interpolation=6, decimation=1, taps=[], fractional_bw=0.4)
            v = blocks.multiply_const_ff(vol)
            for b_ in (rs, lp, sq, fm, d, ar, v):
                b_.set_max_output_buffer(1024)
            o = blocks.file_sink(gr.sizeof_float, dst)
            if os.environ.get('SECURECOM_TERR_TEST_THROTTLE') == '1':     # real-time pacing, like the live replay
                th = blocks.throttle(gr.sizeof_gr_complex, fs)
                th.set_max_output_buffer(1024)
                s.connect(f, th, ch)
            else:
                s.connect(f, ch)
            for i in range(nch):
                s.connect((ch, i), (sm, i))
            s.connect(sm, rs, lp, sq, fm, d, ar, v, o)
    t = TB()
    t.run()


def compare(a, b):
    x, y = np.fromfile(a, dtype=np.float32), np.fromfile(b, dtype=np.float32)
    n = min(len(x), len(y))
    print(f'samples: python {len(x)}, C++ {len(y)}')
    vx, vy = np.count_nonzero(np.abs(x) > 1e-4), np.count_nonzero(np.abs(y) > 1e-4)
    print(f'voiced samples (|x|>1e-4): python {vx} ({vx / 48000:.2f} s), C++ {vy} ({vy / 48000:.2f} s)')
    same = np.array_equal(x[:n], y[:n])
    print('byte-identical over the common length' if same else f'max |difference| {np.max(np.abs(x[:n] - y[:n])):.6g}, differing samples {np.count_nonzero(x[:n] != y[:n])}')
    sys.exit(0 if same else 1)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'make-iq':
        make_iq(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 30)
    elif cmd == 'run':
        run(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == 'compare':
        compare(sys.argv[2], sys.argv[3])
