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
