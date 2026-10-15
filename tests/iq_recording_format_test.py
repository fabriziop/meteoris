"""Raw-IQ-to-PSD parity tests for the new HDF5 and web adapters."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile

import h5py
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'web'))
import meteoris_plot
import meteoris_web


def test_iq_psd_file_and_wire_parity():
    fs = 250_000.0
    n = 1024
    t = np.arange(n) / fs
    iq = np.exp(2j * np.pi * 12000 * t).astype('complex64')
    channels = np.stack((iq.real, iq.imag), axis=-1)
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / 'iq.h5'
        dst = Path(temp) / 'psd.h5'
        with h5py.File(src, 'w') as f:
            m = f.create_group('metadata')
            m.attrs['output_rate_hz'] = fs
            m.attrs['psd_bandwidth_hz'] = 100000.0
            g = f.create_group('iq')
            g.create_dataset('samples', data=channels[None])
            for name, dtype in [('timestamp_ns', 'u8'), ('frame_index', 'u8'),
                                ('event_id', 'u8'), ('detector_state', 'u1'),
                                ('detector_max_db_hz', 'f4'), ('gain_db', 'f4')]:
                g.create_dataset(name, data=np.zeros(1, dtype=dtype))
        meteoris_plot.convert_iq_to_psd(src, dst)
        with h5py.File(dst) as f:
            expected = f['psd/power_density'][0]
            freq = f['psd/frequency_hz'][:]
        header = bytearray(68)
        header[:4] = b'MIQ1'
        struct.pack_into('<H', header, 4, 2)
        struct.pack_into('<I', header, 12, n)
        struct.pack_into('<d', header, 52, freq[0])
        struct.pack_into('<d', header, 60, fs/n)
        result = meteoris_web.iq_to_psd_packet(header, channels.astype('<f4').tobytes())
        assert result[:4] == b'MPSD'
        assert struct.unpack_from('<I', result, 12)[0] == len(freq)
        np.testing.assert_allclose(np.frombuffer(result[68:], '<f4'), expected, rtol=1e-5)


def test_nonoverlapping_hops_reconstruct_original_fft_windows():
    fs, n = 250000.0, 1024
    rng = np.random.default_rng(7)
    stream = (rng.normal(size=3*n) + 1j*rng.normal(size=3*n)).astype('complex64')
    frame_starts = [0, n//2, n*2]
    frames = [stream[i:i+n] for i in frame_starts]
    with tempfile.TemporaryDirectory() as temp:
        src, dst = Path(temp)/'v2.h5', Path(temp)/'psd.h5'
        with h5py.File(src, 'w') as f:
            m=f.create_group('metadata')
            m.attrs['output_rate_hz']=fs
            m.attrs['psd_bandwidth_hz']=100000.0
            g=f.create_group('iq')
            hops=np.stack([np.stack([v[n//2:].real,v[n//2:].imag],axis=-1) for v in frames])
            prefixes=np.stack([np.stack([frames[i][:n//2].real,frames[i][:n//2].imag],axis=-1) for i in (0,2)])
            g.create_dataset('samples',data=hops)
            g.create_dataset('segment_prefix',data=prefixes)
            g.create_dataset('prefix_index',data=[0,0,1])
            g.create_dataset('frame_index',data=[10,11,30])
            for name,dtype in [('timestamp_ns','u8'),('event_id','u8'),('detector_state','u1'),('detector_max_db_hz','f4'),('gain_db','f4')]:
                g.create_dataset(name,data=np.zeros(3,dtype=dtype))
        meteoris_plot.convert_iq_to_psd(src,dst,block_size=1)
        with h5py.File(dst) as f:
            freq=f['psd/frequency_hz'][:]
            got=f['psd/power_density'][:]
        fftfreq=np.fft.fftfreq(n,1/fs)
        selected=np.flatnonzero(np.abs(fftfreq)<=50000)
        selected=selected[np.argsort(fftfreq[selected])]
        window=np.hanning(n)
        expected=np.stack([np.abs(np.fft.fft(v*window)[selected])**2/(fs*np.sum(window**2)) for v in frames])
        np.testing.assert_allclose(got,expected,rtol=2e-5,atol=1e-12)
