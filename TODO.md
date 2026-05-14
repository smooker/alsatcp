# TODO

## High priority

- [ ] **asoundrc on rpi501** — define `pcm.edifier` and `pcm.hx330b` in `/home/smooker/.asoundrc` on rpi501, so init scripts use simple names instead of MAC strings. User-editable without touching init scripts.

## Medium priority

- [ ] **RX mic from HX330B** — SCO/HFP capture (16kHz mono) back to st for vocalist setup. Requires `alsatcp tx` from rpi501 → st (reverse direction).
- [ ] **bidi mode** — simultaneous TX+RX in a single alsatcp process using two Loopback subdevices.

## Low priority

- [ ] **Multiplexer / serializer** — multiple audio channels over a single TCP connection. Simple framing header (channel ID + length) to multiplex/demultiplex N streams on one port. Eliminates the need for one port per channel.

- [ ] **btspk.sh for rpi501** — connect/disconnect script for BT devices (modeled after st).
- [ ] **BT auto-reconnect** — bluez `AutoConnect=true` or udev/script on disconnect event.
- [ ] **loop3+** — additional subdevices for more destinations.
