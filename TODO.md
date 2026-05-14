# TODO

## High priority

- [ ] **asoundrc на rpi501** — дефинирай `pcm.edifier` и `pcm.hx330b` в `/home/smooker/.asoundrc` на rpi501, така че init script-овете да ползват прости имена вместо MAC string-ове. User-editable без touch на init scripts.

## Medium priority

- [ ] **RX mic от HX330B** — SCO/HFP capture (16kHz моно) към st за певец setup. Нужен е `alsatcp tx` от rpi501 към st (обратна посока).
- [ ] **bidi режим** — едновременен TX+RX на един alsatcp процес с два subdevice-а.

## Low priority

- [ ] **btspk.sh за rpi501** — скрипт за connect/disconnect на BT устройства (по модела на st).
- [ ] **Auto-reconnect на BT** — bluez `AutoConnect=true` или скрипт при disconnect event.
- [ ] **loop3+** — допълнителни subdevice-и за повече дестинации.
