#!/bin/sh
# Lets this user open the board's USB serial port (Espressif USB-Serial/JTAG, 303a), for
# Flycast's VMU mirror, flycast_sync.py and esptool. Installs a udev rule so it survives
# replugging, then applies it to a board that is already plugged in. Safe to run again;
# does nothing when the port is already accessible and the rule is in place.
set -eu

RULE=/etc/udev/rules.d/60-espressif-serial.rules
WANT='# Espressif USB-Serial/JTAG (ESP32-S3 e-paper VMU): usable without root, so Flycast'"'"'s VMU
# mirror and the board'"'"'s tools can open it again after the board is unplugged.
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666", TAG+="uaccess"'

ports() {
    for p in /dev/serial/by-id/*Espressif*; do
        [ -e "$p" ] && readlink -f "$p"
    done
    return 0
}

need_rule=false
[ "$(cat "$RULE" 2>/dev/null)" = "$WANT" ] || need_rule=true
need_port=false
for p in $(ports); do
    [ -r "$p" ] && [ -w "$p" ] || need_port=true
done

if ! $need_rule && ! $need_port; then
    echo "ok: rule installed and $(ports | tr '\n' ' ')accessible"
    exit 0
fi

if $need_rule; then
    echo "installing $RULE"
    printf '%s\n' "$WANT" | sudo tee "$RULE" >/dev/null
    sudo udevadm control --reload
fi
for p in $(ports); do
    sudo udevadm trigger --action=add "$p"
done
sudo udevadm settle

status=0
for p in $(ports); do
    if [ -r "$p" ] && [ -w "$p" ]; then
        echo "ok: $p $(stat -c '%A %U:%G' "$p")"
    else
        echo "still no access to $p ($(stat -c '%A %U:%G' "$p")): unplug and replug the board" >&2
        status=1
    fi
done
[ -n "$(ports)" ] || echo "rule installed; no board plugged in right now"
exit $status
