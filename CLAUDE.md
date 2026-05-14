# alsatcp

C daemon for bidirectional audio streaming over TCP between two machines using ALSA Loopback devices.

## Purpose

Replaces the `arecord | socat TCP` pipeline with a single process, single PID, clean signal handling, and proper OpenRC lifecycle.

## Architecture

```
st (sender side):                         rpi501 (receiver side):
  apps → ALSA_OUT=loop  → Loopback,0,0     alsatcp-rx  → bluealsa → Edifier R1280DBs (hci1, A2DP)
  apps → ALSA_OUT=loop2 → Loopback,0,1     alsatcp-rx2 → bluealsa → RB-HX330B (hci1, A2DP)
  alsatcp tx  (Loopback,1,0) → TCP:12345 →
  alsatcp tx2 (Loopback,1,1) → TCP:12346 →
```

## ALSA Loopback pairing

```
write hw:Loopback,0,N  ←→  read hw:Loopback,1,N
write hw:Loopback,1,N  ←→  read hw:Loopback,0,N
```

Subdevice N cross-connects Device 0 and Device 1. Each subdevice (0-7) is an independent channel.

## Current pipeline (production)

```
st-host:
  ALSA_OUT=loop  → apps → hw:Loopback,0,0 → alsatcp tx  (hw:Loopback,1,0) → TCP:12345 → rpi501
  ALSA_OUT=loop2 → apps → hw:Loopback,0,1 → alsatcp tx2 (hw:Loopback,1,1) → TCP:12346 → rpi501

rpi501:
  TCP:12345 → alsatcp-rx  → bluealsa:DEV=B4:E7:B3:96:8D:BF → Edifier R1280DBs (A2DP)
  TCP:12346 → alsatcp-rx2 → bluealsa:DEV=C4:A9:B8:77:F0:1D → RB-HX330B headphones (A2DP)
```

## BT devices on rpi501

- **hci0**: RPi5 onboard BT
- **hci1**: RTL8761BU USB BT adapter
- Single `bluealsa` instance serves both adapters (no `-i` flag)
- bluealsa config: `-p a2dp-source -p a2dp-sink -p hfp-ag -p hsp-ag`
- Edifier R1280DBs MAC: `B4:E7:B3:96:8D:BF`
- RB-HX330B MAC: `C4:A9:B8:77:F0:1D`

## asoundrc on smooker@st

```
pcm.loop  → hw:Loopback,0,0  (subdevice 0 → Edifier)
pcm.loop2 → hw:Loopback,0,1  (subdevice 1 → HX330B)
ALSA_OUT=loop|loop2|dmpch|dmtv|dmg6|btphones|btspk
```

## CLI

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

## Wire format

Raw interleaved PCM frames, no headers. Default: S16_LE, 48000 Hz, stereo. Compatible with `arecord | socat`.

## Init scripts

| File | Machine | Description |
|------|---------|-------------|
| `alsatcp.initd`    | st     | TX loop0 → rpi501:12345 (user=smooker) |
| `alsatcp2.initd`   | st     | TX loop2 → rpi501:12346 (user=smooker) |
| `alsatcp-rx.initd` | rpi501 | RX 12345 → Edifier (root for bluealsa) |
| `alsatcp-rx2.initd`| rpi501 | RX 12346 → HX330B (root for bluealsa) |

## Machines

- **st** (ThinkPad P53, x86_64): 10.200.0.9, WireGuard
- **rpi501** (RPi5, aarch64, musl): 10.200.0.7, WireGuard
- SSH: port 1022 everywhere
- No sudo — `ssh toor` for root (localhost:1022, from claude user on the same machine only)

## Build

```bash
# native (st):
make

# on rpi501 directly:
scp alsatcp.c rpi501:/tmp/alsatcp.c
ssh rpi501 "gcc -O2 -Wall -Wextra -o /tmp/alsatcp /tmp/alsatcp.c -lasound -lpthread"
ssh rpi501 "ssh toor 'cp /tmp/alsatcp /usr/local/bin/alsatcp'"
```

## iptables on rpi501

```bash
iptables -A INPUT -p tcp --dport 12345 -j ACCEPT
iptables -A INPUT -p tcp --dport 12346 -j ACCEPT
iptables-save > /var/lib/iptables/rules-save
```
