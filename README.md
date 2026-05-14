# alsatcp

Single-process ALSA↔TCP audio streaming daemon. Replaces `arecord | socat` pipelines with one binary, one PID, and clean SIGTERM handling.

## Features

- **TX mode**: read ALSA capture device → send over TCP
- **RX mode**: receive TCP → write to ALSA playback device
- **Bidi mode**: both simultaneously (two threads, two subdevices)
- Automatic reconnect on TCP disconnect
- Waits silently when ALSA Loopback has no writer (no log spam)
- OpenRC-friendly: clean stop with SIGTERM

## Build

```bash
# Dependencies: alsa-lib, pthreads
make

# Cross-compile for aarch64 (e.g. Raspberry Pi with musl):
scp alsatcp.c rpi501:/tmp/alsatcp.c
ssh rpi501 "gcc -O2 -Wall -Wextra -o /tmp/alsatcp /tmp/alsatcp.c -lasound -lpthread"
```

## Usage

```
alsatcp <tx|rx|bidi> [options]

  -d DEVICE     ALSA device (default: hw:Loopback,1,0 for tx; hw:Loopback,0,0 for rx)
  -D DEVICE     ALSA RX device (bidi only)
  -H HOST       Remote host (tx/bidi)
  -p PORT       Remote port (tx) or listen port (rx)
  -P PORT       Listen port (bidi RX side)
  -r RATE       Sample rate (default: 48000)
  -c CHANNELS   Channels (default: 2)
  -f FORMAT     s16|s32|f32 (default: s16)
  -B BUFFER_US  ALSA buffer time µs (default: 50000)
  -F PERIOD_US  ALSA period time µs (default: 10000)
  -R RETRY_MS   TCP reconnect interval ms (default: 2000)
  -v            Verbose
```

## Examples

```bash
# TX: send ALSA Loopback capture to remote
alsatcp tx -d hw:Loopback,1,0 -H remote-host -p 12345

# RX: receive and play to bluealsa (Bluetooth speaker)
alsatcp rx -d 'bluealsa:DEV=B4:E7:B3:96:8D:BF,PROFILE=a2dp' -p 12345

# Bidi: two subdevices, two ports
alsatcp bidi -d hw:Loopback,1,0 -D hw:Loopback,1,1 -H remote -p 12345 -P 12346
```

## Wire format

Raw interleaved PCM frames, no headers. Default: S16_LE, 48000 Hz, stereo. Compatible with `arecord | socat` pipelines.

## Use case

Stream audio from an ALSA Loopback device on one machine to a Bluetooth speaker on another, over TCP/WireGuard:

```
[apps] → ALSA_OUT=loop → hw:Loopback,0,0
                          hw:Loopback,1,0 → alsatcp tx → TCP → alsatcp rx → bluealsa → BT speaker
```

Multiple streams on separate subdevices and ports:
- `loop`  / port 12345 → Edifier R1280DBs
- `loop2` / port 12346 → Headphones

## License

MIT
