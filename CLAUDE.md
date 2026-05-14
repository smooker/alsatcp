# alsatcp

C daemon за двупосочен аудио стрийминг през TCP между две машини, използвайки ALSA Loopback устройства.

## Цел

Замества `arecord | socat TCP` pipeline-а с един процес, един PID, чист signal handling и правилен OpenRC lifecycle.

## Архитектура

```
st (sender side):                         rpi501 (receiver side):
  apps → ALSA_OUT=loop  → Loopback,0,0     alsatcp-rx  → bluealsa → Edifier R1280DBs (hci1, A2DP)
  apps → ALSA_OUT=loop2 → Loopback,0,1     alsatcp-rx2 → bluealsa → RB-HX330B (hci1, A2DP)
  alsatcp tx  (Loopback,1,0) → TCP:12345 →
  alsatcp tx2 (Loopback,1,1) → TCP:12346 →
```

## ALSA Loopback паринг

```
write hw:Loopback,0,N  ←→  read hw:Loopback,1,N
write hw:Loopback,1,N  ←→  read hw:Loopback,0,N
```

Subdevice N свързва Device 0 и Device 1 кръстосано. Всеки subdevice (0-7) е независим канал.

## Текущ pipeline (production)

```
st-host:
  ALSA_OUT=loop  → apps → hw:Loopback,0,0 → alsatcp tx  (hw:Loopback,1,0) → TCP:12345 → rpi501
  ALSA_OUT=loop2 → apps → hw:Loopback,0,1 → alsatcp tx2 (hw:Loopback,1,1) → TCP:12346 → rpi501

rpi501:
  TCP:12345 → alsatcp-rx  → bluealsa:DEV=B4:E7:B3:96:8D:BF → Edifier R1280DBs (A2DP)
  TCP:12346 → alsatcp-rx2 → bluealsa:DEV=C4:A9:B8:77:F0:1D → RB-HX330B headphones (A2DP)
```

## BT устройства на rpi501

- **hci0**: RPi5 onboard BT
- **hci1**: RTL8761BU USB BT adapter
- Едно `bluealsa` instance обслужва и двата адаптера (без `-i` флаг)
- bluealsa конфиг: `-p a2dp-source -p a2dp-sink -p hfp-ag -p hsp-ag`
- Едifer R1280DBs MAC: `B4:E7:B3:96:8D:BF`
- RB-HX330B MAC: `C4:A9:B8:77:F0:1D`

## asoundrc на smooker@st

```
pcm.loop  → hw:Loopback,0,0  (subdevice 0 → Edifier)
pcm.loop2 → hw:Loopback,0,1  (subdevice 1 → HX330B)
ALSA_OUT=loop|loop2|dmpch|dmtv|dmg6|btphones|btspk
```

## CLI интерфейс

```
alsatcp <tx|rx|bidi> [options]

  -d DEVICE      ALSA device (default: hw:Loopback,1,0 за tx; hw:Loopback,0,0 за rx)
  -D DEVICE      ALSA RX device (bidi only)
  -H HOST        Remote host (tx/bidi)
  -p PORT        Remote port (tx) или listen port (rx)
  -P PORT        Listen port (bidi RX side)
  -r RATE        Sample rate (default: 48000)
  -c CHANNELS    Channels (default: 2)
  -f FORMAT      s16|s32|f32 (default: s16)
  -B BUFFER_US   ALSA buffer µs (default: 50000)
  -F PERIOD_US   ALSA period µs (default: 10000)
  -R RETRY_MS    TCP retry interval ms (default: 2000)
  -v             Verbose logging
```

## Wire format

Raw PCM frames — без headers. S16_LE, 48000 Hz, 2ch. Съвместимо с `arecord | socat`.

## Init scripts

| Файл | Машина | Описание |
|------|--------|----------|
| `alsatcp.initd`    | st     | TX loop0 → rpi501:12345 (user=smooker) |
| `alsatcp2.initd`   | st     | TX loop2 → rpi501:12346 (user=smooker) |
| `alsatcp-rx.initd` | rpi501 | RX 12345 → Edifier (root за bluealsa) |
| `alsatcp-rx2.initd`| rpi501 | RX 12346 → HX330B (root за bluealsa) |

## Машини

- **st** (ThinkPad P53, x86_64): 10.200.0.9, WireGuard
- **rpi501** (RPi5, aarch64, musl): 10.200.0.7, WireGuard
- SSH: port 1022 навсякъде
- No sudo — `ssh toor` за root (localhost:1022, само от claude user на същата машина)

## Build

```bash
# native (st):
make

# на rpi501 директно:
scp alsatcp.c rpi501:/tmp/alsatcp.c
ssh rpi501 "gcc -O2 -Wall -Wextra -o /tmp/alsatcp /tmp/alsatcp.c -lasound -lpthread"
ssh rpi501 "ssh toor 'cp /tmp/alsatcp /usr/local/bin/alsatcp'"
```

## iptables на rpi501

```bash
iptables -A INPUT -p tcp --dport 12345 -j ACCEPT
iptables -A INPUT -p tcp --dport 12346 -j ACCEPT
iptables-save > /var/lib/iptables/rules-save
```

## Бъдещи функции

- RX mic от HX330B: SCO/HFP 16kHz моно към st (певец setup)
- bidi режим: едновременен TX+RX с два Loopback subdevice-а
