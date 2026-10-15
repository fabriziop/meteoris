# Raw I/Q recording format v2 (non-overlapping hops)

The writer now creates `meteoris v2` files. `/iq/samples` stores one new
`fft_size/2` complex float32 hop per PSD frame, shaped `(frames, fft_size/2, 2)`.
Each hop contains only the newest half of its corresponding 50%-overlap FFT
window. No sample is duplicated between consecutive frames.

For the first frame of a run (including after gaps between events), the missing
first half is saved once in `/iq/segment_prefix`, shaped
`(runs, fft_size/2, 2)`. `/iq/prefix_index` maps each frame to its run's
prefix. `/iq/frame_index` identifies consecutive windows. To reconstruct a
full FFT window, concatenate the run prefix (first frame of a run) or the
preceding frame's hop (consecutive frames) with the current frame's hop.
All frame metadata remains in `/iq/` and the writer continues to support SWMR.

`meteoris_plot` reads both v1 (overlapping full windows) and v2 (hops plus
prefixes). The legacy plotter remains dedicated to PSD-format recordings.
The detector's internal PSD and the network I/Q frame protocol are unchanged.

**Compatibility:** Existing `meteoris v1` daily files cannot be appended to by
the v2 writer. Move/rename the old daily HDF5 file before restarting acquisition
on the same UTC day. Existing v1 files remain readable by the plotter.

At 250 kS/s the non-overlapping CF32 stream is approximately 120 MB/min
(decimal) during sustained recording, plus HDF5 metadata. The example TOML
file-growth safety limit was raised to 160 MiB/min to allow this rate.
