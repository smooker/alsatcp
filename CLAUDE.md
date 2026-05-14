# alsatcp

C daemon за двупосочен аудио стрийминг през TCP между две машини, използвайки ALSA Loopback устройства.

## Цел

Замества `arecord | socat TCP` pipeline-а с един процес, един PID, чист signal handling и правилен OpenRC lifecycle.

## Архитектура

```
st (sender side):                    rpi501 (receiver side):
  apps → hw:Loopback,0,0               alsatcp rx → bluealsa → Edifier
  alsatcp tx: hw:Loopback,1,0 → TCP →  hw:Loopback,0,0 (бъдеще: rpi501→st)
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
  apps → ALSA_OUT=loop → hw:Loopback,0,0
  rc-service alsatcp  →  alsatcp tx -d hw:Loopback,1,0 -H rpi501.wg.smooker.org -p 12345

rpi501:
  rc-service alsatcp-rx  →  alsatcp rx -d bluealsa:DEV=B4:E7:B3:96:8D:BF,PROFILE=a2dp -p 12345
                                         → Edifier R1280DBs (A2DP)
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

### Примери

```sh
# TX (st → rpi501):
alsatcp tx -d hw:Loopback,1,0 -H rpi501.wg.smooker.org -p 12345

# RX (rpi501, слуша):
alsatcp rx -d 'bluealsa:DEV=B4:E7:B3:96:8D:BF,PROFILE=a2dp' -p 12345

# Bidi (два subdevice-а, два порта):
alsatcp bidi -d hw:Loopback,1,0 -D hw:Loopback,1,1 \
             -H remote -p 12345 -P 12346
```

## Wire format

Raw PCM frames — без headers. S16_LE, 48000 Hz, 2ch. Съвместимо с `arecord | socat`.

## Init scripts

- `alsatcp.initd` → `/etc/init.d/alsatcp` на **st** (TX режим, user=smooker, group=audio)
- `alsatcp-rx.initd` → `/etc/init.d/alsatcp-rx` на **rpi501** (RX режим, user=root за bluealsa dbus)

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
```

## Бъдещи функции

- RX режим на st: rpi501 → st (втори subdevice, loop2)
- loop2 = слушалки stream (hci1 USB BT — блокирано от firmware проблем с RTL8761BU)
- bidi режим: TX + RX едновременно с два Loopback subdevice-а
