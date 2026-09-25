# Meteoris live network and web interface

Meteoris can expose the live PSD stream and a small control channel to the
separate `meteoris_web` process. The acquisition/DSP process does not contain an
HTTP server and does not depend on a browser.

## Architecture

```text
SDR -> DSP front end -> immutable PsdFrame
                         |       |       |
                         |       |       +--> TCP PSD publisher (best effort)
                         |       +----------> recorder
                         +------------------> detector

                                        TCP PSD :5510
meteoris ------------------------------------------------> meteoris_web
        <-----------------------------------------------  TCP control :5511
                                                            |
                                                            | HTTP/WebSocket
                                                            v
                                                         browser
```

The DSP computes each PSD once. A completed frame is owned by a
`shared_ptr<const PsdFrame>`. Detector/recorder and network queues retain that
same immutable object; the PSD vector is not copied for fan-out. A frame is
released only after the last consumer has finished with it. Detector
pre-context also retains shared frame references instead of copying PSD arrays.

The recorder queue is lossless by policy: if it overruns, Meteoris stops rather
than silently discarding scientific data. The network queue is deliberately
best-effort: when a slow web client fills it, the oldest network-only reference
is dropped. Recorder/detector delivery is unaffected.

## Network configuration

```toml
[network]
enabled = true
bind_address = "127.0.0.1"
psd_port = 5510
control_port = 5511
queue_frames = 16
```

`127.0.0.1` is the safe default when `meteoris_web` runs on the same computer.
For a gateway on another trusted machine, bind to an appropriate interface
address (or `0.0.0.0`) and protect the ports with the host firewall/VPN. The
Meteoris TCP protocol itself does not provide TLS or authentication.

When networking is enabled, Meteoris creates listening sockets, but **PSD
frames are not queued or transmitted until a PSD client is connected**. The
control channel similarly performs no work until a control client connects.

`meteoris_web` uses a demand-driven PSD connection: merely running the gateway
does not connect to the DSP PSD port. The upstream PSD socket is opened when
the first browser connects to `/ws/psd` and is closed when the last browser
disconnects. This keeps the Raspberry Pi DSP/network overhead close to the
no-network baseline while nobody is viewing the waterfall. The control socket
remains independent and is opened on demand by status/config/control requests.

## PSD wire protocol

The PSD connection is a sequence of binary frames. Version 1 uses a packed
68-byte little-endian header followed immediately by `bin_count` IEEE-754
`float32` PSD-density values.

```text
Offset  Type      Field
0       char[4]   "MPSD"
4       uint16    version (=1)
6       uint16    header_bytes (=68)
8       uint32    payload_bytes
12      uint32    bin_count
16      uint64    frame_index
24      uint64    timestamp_ns
32      uint64    center_sample_index
40      float32   gain_db
44      float64   center_frequency_hz
52      float64   frequency_start_hz
60      float64   frequency_step_hz
68      float32[] PSD payload
```

The network thread sends the header and then sends directly from the immutable
`PsdFrame::powerDensity` buffer. It does not construct a second PSD payload.

## TCP control protocol

The control socket is a reliable, UTF-8, newline-delimited request/reply
protocol. On connection Meteoris sends:

```text
HELLO METEORIS 1
```

Implemented commands are:

```text
PING
GET_CONFIG
GET_STATUS
SET sdr.center_frequency <Hz>
SET sdr.gain <dB>
```

`GET_CONFIG` returns `CONFIG <byte-count>\n` followed by the exact effective
TOML bytes. `GET_STATUS` returns one `STATUS {...}\n` JSON line. `SET` requests
are queued to the acquisition thread; only that thread calls SoapySDR mutation
methods. The reply is sent after the device reports the applied value.

At present center frequency and gain are live-settable (manual gain changes are
rejected while AGC is enabled). Other TOML parameters are visible in the browser but return `restart_required_or_read_only` if sent
through `SET`. This is intentional: FFT geometry, decimation, detector
construction, output paths, and similar settings require coordinated component
reinitialization rather than mutation from a network thread.

## Running meteoris_web

`meteoris_web` uses Python and `aiohttp`:

```bash
python -m pip install aiohttp
python web/meteoris_web.py --dsp-host 127.0.0.1
```

The default browser URL is:

```text
http://127.0.0.1:8080/
```

Useful options include:

```text
--dsp-host HOST
--psd-port 5510
--control-port 5511
--listen 127.0.0.1
--http-port 8080
--web-root PATH
```

The gateway keeps the Meteoris control connection separate from the PSD
connection. PSD data is relayed to browsers through a binary WebSocket. The
PSD connection is demand-driven: zero browser waterfall subscribers means zero
DSP PSD TCP traffic. The browser maintains a bounded queue and renders on
`requestAnimationFrame`, so the DSP PSD cadence (for example about 120--125
frames/s) is independent of display refresh cadence. With a browser connected,
all PSD frames are still forwarded; there is no implicit decimation.

The web UI provides a live waterfall, center-frequency/gain status and
controls, network status, dB display range, and the complete effective TOML.

The waterfall toolbar also has a `Sound` / `Mute` button. Audio is muted by
default and starts only after the user presses `Sound`, as required by browser
autoplay policies. Because the network stream contains PSD power rather than
time-domain samples or phase, this is spectrum sonification rather than
reconstructed receiver audio. The currently visible waterfall band is divided
into 48 bands and mapped linearly to 80 Hz--5 kHz. A median-noise gate suppresses
the idle spectrum, while narrow PSD peaks become tones according to their power
above that noise floor. Changing the visible frequency limits changes the
RF-offset range being heard. Press `Mute` to suspend browser audio processing.
