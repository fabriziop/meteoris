# Meteoris DSP pipeline

This document describes the digital signal-processing pipeline implemented in
`meteoris.cpp` for **Meteoris 0.1.0**. It follows the current C++ implementation
from native SoapySDR `CS8` acquisition through frequency translation, FIR
decimation, PSD calculation, and delivery of PSD frames to the detector.

---

## 1. Pipeline overview

The main real-time path is:

```text
SDR / SoapySDR
native interleaved CS8 I,Q
        |
        | Fs = input sample rate
        v
+-------------------------------+
| NCO complex frequency shift   |
| block CS8 -> float I/Q        |
+-------------------------------+
        |
        | still Fs
        v
+-------------------------------+
| FIR decimator stage 1         |
| optimized block kernel        |
| SIMD + optional 2nd worker    |
+-------------------------------+
        |
        | Fs / D1
        | normalize CS8 by 1/128
        v
+-------------------------------+
| FIR decimator stage 2         |
| stateful complex FIR          |
+-------------------------------+
        |
        | Fs / (D1 D2)
        v
+-------------------------------+
| Welch PSD                     |
| Hann window                   |
| radix-2 FFT                   |
| 50% overlap                   |
+-------------------------------+
        |
        | selected +/- B/2 bins
        v
PsdFrame
        |
        +--> detector
        +--> triggered HDF5 recording
```

For the common configuration:

```toml
[sdr]
sample_rate = 10000000

[dsp]
decimation_stage1 = 8
decimation_stage2 = 5
bandwidth_hz = 100000

[psd]
fft_size = 4096
```

the rates are:

```text
SDR input             10.000 Msps
after FIR1             1.250 Msps
after FIR2             0.250 Msps
final complex rate       250 ksps
PSD selected band        100 kHz
```

The total decimation factor is:

```text
D = D1 * D2 = 8 * 5 = 40
```

---

## 2. SDR input representation

Meteoris requests the SoapySDR stream format:

```text
CS8
```

The stream contains interleaved signed 8-bit complex samples:

```text
I0, Q0, I1, Q1, I2, Q2, ...
```

Each I and Q component is represented by `int8_t`.

The application attempts to use SoapySDR direct-access buffers when
`runtime.direct_buffer = true` and the driver supports them. Otherwise it uses
the normal `readStream()` path and an application-owned fallback buffer.

The DSP algorithm itself receives:

```cpp
const int8_t *iq
```

plus the number of complex samples in the block.

Using native CS8 reduces USB/memory bandwidth compared with converting the
entire 10 Msps stream to complex float before it reaches Meteoris.

---

## 3. DSP configuration and derived rates

Let:

```text
Fs = sdr.sample_rate
D1 = dsp.decimation_stage1
D2 = dsp.decimation_stage2
B  = dsp.bandwidth_hz
```

Meteoris derives:

```text
Fs1 = Fs / D1
Fout = Fs / (D1 * D2)
passEdge = B / 2
```

For the standard values:

```text
Fs       = 10,000,000 Hz
D1       = 8
D2       = 5
B        = 100,000 Hz

Fs1      = 1,250,000 Hz
Fout     =   250,000 Hz
passEdge =    50,000 Hz
```

The configured observation bandwidth must be smaller than the final sample
rate.

---

# 4. FIR filter design

Both decimation filters are generated at program startup by
`designLowpass()`.

The implementation uses a **Kaiser-windowed sinc low-pass FIR**.

The filter length is estimated from the requested attenuation and transition
width using:

```text
N ~= (A - 8) / (2.285 * DeltaOmega)
```

where:

```text
A          = attenuation_db
DeltaOmega = normalized transition width in rad/sample
```

The implementation then:

1. rounds the estimate upward;
2. enforces at least 15 taps;
3. forces an odd number of taps;
4. computes a Kaiser beta from the requested attenuation;
5. generates the windowed sinc;
6. normalizes coefficients so their sum is one.

The cutoff used to construct the sinc is halfway between the requested pass
and stop edges:

```text
cutoff = (passEdge + stopEdge) / 2
```

Thus `dsp.attenuation_db` controls the desired out-of-band attenuation and,
indirectly, the number of FIR taps and CPU cost.

---

## 5. Stage-1 FIR design

Stage 1 performs the large rate reduction:

```text
Fs -> Fs / D1
```

Its pass edge is:

```text
B / 2
```

The stage-1 stop edge is chosen by the implementation as:

```text
stop1 = 0.96 * (Fs1 / 2)
```

where:

```text
Fs1 = Fs / D1
```

For the standard configuration:

```text
Fs1 / 2 = 625 kHz
stop1   = 600 kHz
pass    =  50 kHz
```

so FIR1 has a wide transition band:

```text
50 kHz -> 600 kHz
```

This is intentional. Stage 1 primarily protects the first decimation Nyquist
boundary while keeping the expensive 10 Msps filter short.

Stage 2 performs the tighter final channel selection.

---

## 6. Stage-2 FIR design

FIR2 runs at the much lower stage-1 rate:

```text
Fs1 = Fs / D1
```

Its pass edge is again:

```text
B / 2
```

and its stop edge is:

```text
Fout / 2
```

For:

```text
Fs1  = 1.25 MHz
Fout = 250 kHz
B    = 100 kHz
```

the edges are:

```text
pass =  50 kHz
stop = 125 kHz
```

Because this tighter filter runs after the first 8:1 reduction, its arithmetic
cost is much lower than implementing the complete channel filter at 10 Msps.

This two-stage design is one of the principal computational optimizations in
Meteoris.

---

# 7. NCO frequency translation

Before FIR1 decimation, the incoming complex signal is translated in frequency
by `NcoMixer`.

Conceptually, for complex input:

```text
x[n] = I[n] + j Q[n]
```

the mixer computes:

```text
x_shifted[n] = x[n] * exp(j * 2*pi*shift_hz*n/Fs)
```

Therefore a spectral component at baseband frequency `f` is translated to:

```text
f + shift_hz
```

under the complex-frequency convention used by the implementation.

For example, an input component at:

```text
+1.1 MHz
```

with:

```toml
shift_hz = -1100000
```

is moved approximately to:

```text
0 Hz
```

before decimation.

---

## 8. NCO periodic lookup optimization

The NCO checks whether:

```text
shift_hz / Fs
```

can be represented as a short repeating rational sequence.

When possible, it precomputes one oscillator period as a complex lookup table:

```text
cos(phi[k]) + j sin(phi[k])
```

and then cycles through the table.

This is particularly effective for shifts such as:

```text
-1 MHz at 10 Msps
```

which have a short periodic relationship.

The lookup path avoids evaluating trigonometric functions for each sample and
also avoids repeated complex oscillator recursion.

If a suitable short periodic representation is not available, the mixer uses
a recursively updated complex phase:

```text
phase *= step
```

with occasional renormalization to limit numerical magnitude drift.

---

## 9. Block NCO implementation

The current optimized path does not call a scalar mixer function for every
sample.

Instead:

```cpp
mixBlockCs8(...)
```

processes an entire native CS8 block into separate floating-point arrays:

```text
blockRe[]
blockIm[]
```

For each native sample it performs the complex product:

```text
outI = I*c - Q*s
outQ = I*s + Q*c
```

where `c` and `s` are the NCO cosine and sine values.

The block mixer also optionally counts native CS8 samples that approach the
ADC rails for the low-cost clipping AGC.

This means clipping measurement is folded into an already-required pass over
the input rather than requiring another 10 Msps scan.

---

# 10. FIR1 block decimator

The first decimator is implemented by `BlockFir1Decimator`.

This replaced the earlier design in which every input sample was pushed
through a stateful sample-by-sample interface.

The optimized algorithm works on blocks.

For every SDR block it constructs:

```text
history from previous block
+
new mixed I/Q samples
```

in contiguous arrays.

Only input positions that actually produce a decimated output are evaluated.

Conceptually:

```text
y[m] = sum h[k] * x[D1*m - k]
```

rather than evaluating a complete FIR result for samples that will immediately
be discarded by decimation.

This gives the arithmetic reduction expected from a decimating/polyphase
implementation.

---

## 11. FIR1 history across SDR blocks

An FIR cannot treat SoapySDR blocks as independent because the first outputs of
a new block depend on samples from the preceding block.

`BlockFir1Decimator` therefore retains the required FIR history separately for
I and Q:

```text
historyRe
historyIm
```

and prepends that history to the next block before computing outputs.

It also maintains absolute input/output indices so the decimation phase remains
continuous across arbitrary SDR block boundaries.

Therefore changing the Soapy stream MTU/block size does not reset FIR phase or
NCO phase.

---

## 12. FIR1 SIMD-friendly layout

The block FIR uses separate real and imaginary arrays rather than performing
the inner dot product over `std::complex<float>` objects.

Conceptually:

```text
sumRe += h[k] * inputRe[k]
sumIm += h[k] * inputIm[k]
```

This structure-of-arrays style makes the inner loops easier to vectorize.

The implementation contains optimized paths for supported CPU instruction
sets, including:

```text
AVX2  on suitable x86 systems
NEON  on suitable ARM systems
```

with a scalar/compiler-vectorized fallback.

This stage is the primary DSP hotspot because it processes information derived
from the full 10 Msps stream.

---

## 13. Optional two-thread FIR1 execution

`runtime.fir1_threads` can be:

```text
1
or
2
```

With one thread, all FIR1 outputs are computed by the caller.

With two threads and a sufficiently large output block, the output range is
split approximately in half:

```text
first half  -> caller/main DSP thread
second half -> persistent FIR1 worker
```

The worker is persistent; a new operating-system thread is not created for
each SDR block.

The two ranges are independent because each output is calculated directly from
the input block plus retained history.

For small output blocks, the implementation avoids the worker overhead and
computes the block on one thread.

Two threads are not guaranteed to be faster. On a fast SIMD-capable CPU,
synchronization overhead can exceed the saved arithmetic time. It is intended
to be benchmarked on the target platform.

---

# 14. CS8 normalization point

An important implementation detail is that Meteoris does **not** immediately
normalize every CS8 sample to approximately `[-1, +1]`.

The native integer amplitude is preserved through:

```text
NCO
+
FIR1
```

After stage-1 decimation, each complex FIR1 output is multiplied by:

```cpp
1.0f / 128.0f
```

This normalization therefore occurs at:

```text
Fs / D1
```

rather than at the full input rate.

For an 8:1 first decimator, this moves a simple scaling operation from:

```text
10 million complex samples/s
```

to:

```text
1.25 million complex samples/s
```

which reduces unnecessary arithmetic in the hottest part of the pipeline.

---

# 15. FIR2 decimator

The second stage uses `FirDecimator`.

Unlike the optimized block FIR1, this is a stateful sample-push decimator.

For each FIR1 output sample:

1. the sample is inserted into a circular/doubled delay buffer;
2. a countdown determines whether the current input instant produces a
   decimated output;
3. only at output instants is the FIR dot product evaluated;
4. if an output exists, it is passed immediately to the PSD stage.

Thus FIR2 also avoids calculating FIR outputs that would simply be discarded.

Because FIR2 operates at only:

```text
Fs / D1
```

its computational cost is much smaller than FIR1.

For the standard chain:

```text
FIR1 input  = 10.00 Msps
FIR2 input  =  1.25 Msps
PSD input   =  0.25 Msps
```

---

# 16. Integrated processing loop

`IntegratedDspChain::processCs8()` ties the stages together.

The sequence is approximately:

```text
t0

FIR1.processCs8(
    NCO,
    native CS8 block,
    stage1 output,
    clipping counter
)

t1

for each stage1 complex sample:
    normalize by 1/128

    if FIR2 produces output:
        PSD.consume(output)
        output_count++

t2
```

The code accumulates separate wall-time counters:

```text
t1 - t0 -> FIR1 + NCO wall time
t2 - t1 -> post-FIR1 wall time
```

These are later reported for performance analysis.

The first measurement intentionally groups NCO and FIR1 because the block
decimator invokes the mixer directly as part of its input preparation.

---

# 17. Final complex sample rate

The final sample rate is:

```text
Fpsd = Fs / (D1 * D2)
```

For:

```text
Fs = 10 MHz
D1 = 8
D2 = 5
```

this is:

```text
Fpsd = 250 ksample/s
```

The PSD engine consumes every final complex sample.

The selected 100 kHz observation band is narrower than the 250 kHz complex
Nyquist span, leaving transition/guard space outside the useful band.

---

# 18. Welch PSD engine

PSD processing is implemented by `WelchBandPsd`.

The constructor receives:

```text
final sample rate
requested PSD bandwidth
FFT size
```

For the standard configuration:

```text
Fs_psd = 250 kHz
B      = 100 kHz
Nfft   = 4096
```

---

## 19. Circular FFT input buffer

Final complex samples are written into a circular ring buffer.

The ring is allocated in a duplicated form so the current chronological FFT
window can be addressed as one contiguous memory range even when the logical
ring wraps.

Once at least one complete FFT window has been collected, PSD frames are
generated at the configured hop interval.

---

## 20. 50% FFT overlap

The PSD hop is half the FFT length:

```text
hop = Nfft / 2
```

so consecutive PSD frames overlap by 50%.

For:

```text
Nfft = 4096
```

the hop is:

```text
2048 samples
```

At 250 ksps:

```text
frame period =
2048 / 250000
= 8.192 ms
```

and:

```text
frame rate ~= 122.07 PSD/s
```

This high temporal cadence is important to the detector because the causal
PSD time mean and peak-to-track association operate once per PSD frame, while
activation/loss/context parameters are expressed in seconds.

---

# 21. Hann window

Before each FFT, the chronological input block is multiplied by a Hann window:

```text
w[n] =
0.5 - 0.5*cos(2*pi*n/(Nfft-1))
```

The implementation also computes:

```text
windowPower = sum w[n]^2
```

which is later used in PSD-density normalization.

The Hann window reduces spectral leakage compared with a rectangular FFT
window.

---

# 22. Radix-2 FFT

Meteoris contains its own in-place radix-2 FFT implementation,
`Radix2Fft`.

Consequently the DSP path does not require an external FFT library.

The FFT size must be a power of two.

The FFT operates on complex floating-point values produced by the final
decimator.

---

# 23. FFT frequency mapping

FFT bins are mapped to signed baseband frequencies.

Conceptually the frequency axis is:

```text
0 ... +Fs/2, -Fs/2 ... 0
```

in raw FFT order, but Meteoris selects and sorts the useful bins by their
signed frequency.

The final stored frequency array is therefore monotonic from negative to
positive frequency.

---

# 24. Selected PSD band

Meteoris does not retain every FFT bin.

At PSD construction time it selects only bins satisfying:

```text
|f| <= bandwidth / 2
```

For:

```toml
bandwidth_hz = 100000
```

the retained spectrum is approximately:

```text
-50 kHz ... +50 kHz
```

although the FFT itself is operating on the full 250 ksps complex stream.

This reduces the amount of PSD data passed to the detector and HDF5 writer.

---

# 25. PSD-density normalization

For each selected FFT bin, Meteoris calculates:

```text
raw = |FFT[k]|^2
```

and scales it by:

```text
1 / (windowPower * Fs_psd)
```

giving:

```text
PSD_linear[k] =
|FFT[k]|^2 /
(windowPower * Fs_psd)
```

The resulting values are stored in `PsdFrame::powerDensity`.

The detector later converts selected values to dB/Hz with:

```text
10 * log10(PSD_linear)
```

The PSD stage itself retains linear density values.

---

# 26. PSD frame timestamp

Each PSD frame is timestamped near the center of its FFT input interval.

The implementation calculates a center sample index:

```text
centerSampleIndex =
samples_processed - Nfft + (Nfft - 1)/2
```

and converts that index to time using the final PSD sample rate.

The timestamp is therefore associated with the temporal center of the
windowed FFT rather than its end.

This gives waterfall and detector timestamps a more meaningful correspondence
to the signal energy represented by each PSD.

---

# 27. Gain metadata in the DSP path

The PSD engine stores the current receiver gain in each `PsdFrame`.

The current gain is updated by the application, including when the clipping
AGC changes SDR gain.

Consequently recorded PSD rows can later be associated with the receiver gain
that was active when they were produced.

This is useful when interpreting absolute or relative PSD levels.

---

# 28. PSD callback to the detector

After a PSD frame is constructed, the PSD engine invokes its configured
callback.

When the detector is enabled, the callback sends the frame to:

```text
PsdDetectorRecorder::consume()
```

The DSP pipeline therefore ends, from a signal-processing perspective, at:

```text
PsdFrame
```

The detector then performs:

```text
symmetric frequency-band selection
maximum-bin dB/Hz thresholding
N-frame qualification
event state management
HDF5 recording
```

Those operations are described separately in the Meteoris detector
documentation.

---

# 29. Low-cost clipping AGC interaction

Although AGC is not part of the linear frequency-selective DSP chain, its
measurement is integrated into the high-rate processing path.

During native CS8 block mixing, Meteoris can count a complex sample as clipped
when either I or Q reaches the configured native threshold.

For example:

```toml
clip_level = 126
```

marks samples close to the signed 8-bit rails.

Because the clipping test is performed while the input samples are already
being read for NCO/FIR1 processing, there is no second pass over the 10 Msps
stream.

The block clipping count is passed to `ClippingAgc`, which applies a smoothed,
low-rate gain-control decision through SoapySDR.

---

# 30. Numerical data types through the chain

The main representations are:

```text
SDR:
    int8 I + int8 Q
    native CS8

NCO output:
    float I[]
    float Q[]
    native CS8 amplitude scale retained

FIR1 output:
    complex<float>
    native amplitude scale retained

after FIR1:
    multiply by 1/128

FIR2 output:
    complex<float>
    normalized complex samples

FFT:
    complex<float>

PSD:
    float linear power density

detector:
    converts selected PSD bins to dB/Hz
```

Using floating point after acquisition simplifies filtering, FFT processing,
and PSD normalization while retaining a compact SDR input representation.

---

# 31. Streaming continuity

Several pieces of state must remain continuous across SoapySDR blocks:

```text
NCO phase / LUT index
FIR1 sample history
FIR1 decimation phase
FIR2 delay line
FIR2 decimation countdown
PSD circular buffer
PSD hop position
```

Meteoris preserves each of these.

An SDR buffer boundary therefore has no intended signal-processing meaning.
The DSP behaves as a continuous stream processor even when the driver's block
sizes vary.

---

# 32. Why two decimation stages are used

A single FIR could theoretically perform the complete 40:1 decimation.

However, filtering at 10 Msps is expensive.

Meteoris instead uses:

```text
10 Msps
   |
   | relatively relaxed FIR
   v
 /8
   |
1.25 Msps
   |
   | tighter FIR
   v
 /5
   |
250 ksps
```

The expensive high-rate FIR only needs to suppress components that would alias
across the first decimation boundary.

The tighter final filter is moved to the lower-rate domain.

This is the main reason the full chain can run with a relatively low DSP CPU
budget on modern CPUs.

---

# 33. Why FIR work is performed only at output instants

For a decimator by \(D\), only one output is retained for every \(D\) input
samples.

A naive implementation could calculate:

```text
FIR output for every input sample
then discard D-1 outputs
```

which wastes most FIR arithmetic.

Meteoris instead computes a dot product only for the input instants that
produce retained decimated samples.

For stage 1:

```text
number of FIR outputs/s ~= Fs / D1
```

rather than:

```text
Fs
```

This is equivalent in arithmetic intent to polyphase decimation and is
essential to the achieved throughput.

---

# 34. Performance instrumentation

Meteoris measures DSP execution time independently from total wall-clock
streaming time.

Important metrics include:

```text
DSP_time
DSP_CPU_budget
max_DSP_block_time
block_budget
deadline misses
block-time percentiles
FIR1_NCO wall time
post_FIR1 wall time
```

`DSP_CPU_budget` compares accumulated DSP processing time with the amount of
real-time input represented by the samples.

For example:

```text
DSP_CPU_budget = 18.9%
```

means the DSP computations occupied approximately 18.9% of one real-time
stream duration on the measured processing path.

This provides substantial average headroom, although block-time percentiles
and SDR overflows are also important for judging real-time robustness.

---

# 35. Block deadline

For an SDR block containing `Nblock` complex input samples at rate `Fs`, its
real-time arrival budget is:

```text
Tblock = Nblock / Fs
```

To process synchronously without falling behind, DSP work for that block
should normally finish before another equivalent block arrives.

Meteoris therefore reports both average DSP utilization and per-block timing
statistics.

A low average CPU budget does not by itself guarantee that every individual
block meets its deadline.

---

# 36. Typical 10 Msps / 40:1 example

Using:

```toml
[sdr]
sample_rate = 10000000

[dsp]
shift_hz = -1100000
decimation_stage1 = 8
decimation_stage2 = 5
bandwidth_hz = 100000
attenuation_db = 60

[psd]
fft_size = 4096
```

the chain is approximately:

```text
10,000,000 CS8 complex samples/s
        |
        | NCO -1.1 MHz
        v
10,000,000 mixed samples/s
        |
        | FIR1, pass ~50 kHz
        | stop ~600 kHz
        | decimate /8
        v
1,250,000 complex float samples/s
        |
        | normalize 1/128
        | FIR2, pass ~50 kHz
        | stop 125 kHz
        | decimate /5
        v
250,000 complex samples/s
        |
        | Hann 4096
        | radix-2 FFT
        | hop 2048
        v
~122.07 PSD frames/s
        |
        | retain -50 ... +50 kHz
        | ~61 Hz FFT-bin spacing
        v
PsdFrame
        |
        v
detector
```

---

# 37. Approximate PSD dimensions

At:

```text
Fs_psd = 250 kHz
Nfft   = 4096
```

bin spacing is:

```text
DeltaF = 250000 / 4096
       ~= 61.035 Hz
```

A 100 kHz selected band therefore contains on the order of:

```text
100000 / 61.035 ~= 1638
```

frequency intervals/bins, subject to the exact inclusive FFT-bin selection.

This is the approximate width of each PSD row passed to the detector and,
during events, stored in HDF5.

---

# 38. Computational-load hierarchy

The intended load hierarchy is approximately:

```text
most expensive:
    NCO + FIR1 at/derived from 10 Msps

lower:
    FIR2 at 1.25 Msps

lower-rate:
    FFT/PSD at 250 ksps

very low rate:
    detector at ~122 frames/s
```

This explains why optimization work has concentrated on FIR1:

- block processing;
- decimation-aware output calculation;
- separate real/imaginary arrays;
- AVX2/NEON SIMD;
- optional second worker.

The FFT is important but is not normally the dominant cost of this specific
10 Msps -> 250 ksps chain.

---

# 39. Configuration constraints relevant to DSP

Meteoris validates several DSP-related conditions.

The following must be positive/nonzero:

```text
sample_rate
decimation_stage1
decimation_stage2
bandwidth_hz
```

`fft_size` must be a power of two.

`fir1_threads` must be:

```text
1 or 2
```

The final bandwidth must satisfy:

```text
bandwidth_hz < final output sample rate
```

The FIR design also requires valid pass and stop edges.

In particular, the stage-1 pass edge must remain below its calculated stop
edge.

---

# 40. Relationship between RF tuning and final PSD frequency

Ignoring hardware/driver frequency error, the intended mapping is:

```text
raw baseband frequency =
RF signal frequency - SDR center frequency
```

followed by:

```text
translated frequency =
raw baseband frequency + shift_hz
```

The FIRs preserve the selected translated band around zero.

The PSD frequency axis then reports frequency offset relative to that
translated center.

Therefore a desired RF signal can be placed at 0 Hz in the final PSD by
choosing the SDR tuning and NCO shift so that:

```text
RF_signal - center_frequency + shift_hz ~= 0
```

---

# 41. Pipeline latency

The signal path introduces several forms of latency:

1. SDR/driver buffering;
2. FIR group delay;
3. decimation;
4. accumulation of one FFT window;
5. 50%-overlap PSD scheduling;
6. detector N-frame qualification.

The detector timestamps PSD frames at the center of their FFT windows, which
helps separate timestamp meaning from processing/display latency.

Meteoris is designed primarily for reliable detection and recording rather
than minimum-latency demodulation.

---

# 42. Design rationale summary

The DSP architecture is optimized around a simple objective:

> Reduce a high-rate native CS8 SDR stream to a narrow, accurately filtered
> PSD band with enough CPU margin for continuous unattended operation.

The principal design choices are:

- native CS8 acquisition to reduce I/O/memory bandwidth;
- integrated NCO mixing and FIR1 input preparation;
- short first-stage anti-alias filtering at the high sample rate;
- aggressive first-stage decimation;
- tighter channel filtering after the rate has fallen;
- normalization after FIR1 rather than at 10 Msps;
- decimation-aware FIR computation only at retained output instants;
- SIMD-friendly FIR1 memory layout;
- optional multicore FIR1 processing;
- 50%-overlapped Hann-window PSD;
- retention of only the configured final frequency band;
- direct callback of PSD frames into the low-rate detector.

For the standard configuration, this reduces:

```text
10 Msps native SDR input
```

to:

```text
250 ksps filtered complex stream
```

and finally to roughly:

```text
122 PSD frames/s over a 100 kHz observation band
```

before event detection and recording.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
