#!/bin/sh
# Install jsui-wm on Ubuntu Touch (tested target: Galaxy Tab A7 SM-T500,
# Ubuntu Touch 24.04). Run on the tablet as the phablet user, next to
# jsui-wm-ubuntu-touch.tar.gz (or from the unpacked jsui-wm directory):
#
#   sh install.sh            # unpack to ~/jsui-wm and switch the shell
#   sh install.sh --now      # ... and restart the session right away
#   sh install.sh --system   # also the system bits Lomiri normally sets up (sudo):
#                            # power key via repowerd, double-tap to wake, GPU touch boost,
#                            # Wi-Fi on the stock Samsung vendor (wifi-on.sh)
#
# Nothing outside your home directory changes: a systemd user drop-in makes
# Lomiri's launcher (lomiri-systemd-wrapper) run ~/jsui-wm/jsui-session
# instead of lomiri. uninstall.sh removes the drop-in again.
set -e
# Run as root (sudo sh install.sh ...): again as the user who ran sudo, with
# their session bus. The steps above are per user (HOME, systemctl --user),
# and the system ones call sudo themselves; as root it unpacked to
# /root/jsui-wm and stopped at "Failed to connect to bus".
if [ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != root ]; then
  U=$(id -u "$SUDO_USER")
  exec sudo -u "$SUDO_USER" -H env XDG_RUNTIME_DIR="/run/user/$U" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$U/bus" sh "$(readlink -f "$0")" "$@"
fi
HERE=$(dirname "$(readlink -f "$0")")
DEST="$HOME/jsui-wm"
if [ -f "$HERE/jsui-wm-ubuntu-touch.tar.gz" ]; then
  tar xzf "$HERE/jsui-wm-ubuntu-touch.tar.gz" -C "$HOME"
elif [ "$HERE" != "$DEST" ] && [ -x "$HERE/jsui-wl" ]; then
  mkdir -p "$DEST" && cp -a "$HERE/." "$DEST/"
fi
[ -x "$DEST/jsui-wl" ] || { echo "no jsui-wl in $DEST" >&2; exit 1; }

# The libraries jsui-wl needs must all resolve.
missing=$(LD_LIBRARY_PATH="$DEST/lib" ldd "$DEST/jsui-wl" | grep "not found" || true)
if [ -n "$missing" ]; then
  echo "missing libraries:" >&2; echo "$missing" >&2; exit 1
fi

# lomiri-full-greeter.service runs instead of lomiri-full-shell.service until
# the first-run wizard is done: take both.
for UNIT in lomiri-full-shell lomiri-full-greeter; do
  D="$HOME/.config/systemd/user/$UNIT.service.d"
  mkdir -p "$D"
  cat > "$D/jsui-wm.conf" <<CONF
# jsui-wm instead of Lomiri (remove this file, or run ~/jsui-wm/uninstall.sh, to go back)
[Service]
Environment=LOMIRI_BINARY=$DEST/jsui-session
# jsui-session runs jsui-wl as a child, which reports READY to systemd.
NotifyAccess=all
CONF
done
# Lomiri's polkit agent shows its prompts as Lomiri snap decisions: under
# jsui-notifyd (no "dialogs" capability) it aborts at start, and with
# Restart=always that was a restart every 3 s, for good. It stays out while
# jsui-wl runs (its control socket); under Lomiri it starts as before.
D="$HOME/.config/systemd/user/lomiri-polkit-agent.service.d"
mkdir -p "$D"
cat > "$D/jsui-wm.conf" <<'CONF'
# jsui-wm: no Lomiri polkit agent while jsui-wl runs (it needs Lomiri's snap decisions)
[Unit]
ConditionPathExists=!%t/jsui-wl.ctl
CONF
systemctl --user daemon-reload
systemctl --user stop lomiri-polkit-agent.service 2>/dev/null || true
echo "jsui-wm installed in $DEST."
# --system (sudo): what Lomiri normally sets up outside the session. logind
# must ignore the power key (else a short press powers off), and on the
# Galaxy Tab A7 the touch controller's double tap to wake gets switched on,
# and on Adreno GPUs the session may raise the GPU clock while touched.
case " $* " in *" --system "*)
  sudo install -m 644 -D "$DEST/jsui-wm-logind.conf" /etc/systemd/logind.conf.d/jsui-wm.conf &&
    sudo systemctl kill -s HUP systemd-logind && echo "logind: power key handled by repowerd."
  if [ -e /sys/class/sec/tsp/cmd ]; then
    sudo install -m 644 "$DEST/jsui-wm-tap-to-wake.service" /etc/systemd/system/ &&
      sudo systemctl daemon-reload && sudo systemctl enable --now jsui-wm-tap-to-wake.service &&
      echo "double-tap to wake enabled."
  fi
  if [ -e /sys/class/kgsl/kgsl-3d0/devfreq/min_freq ]; then
    sudo install -m 644 "$DEST/jsui-wm-gpu-boost.service" /etc/systemd/system/ &&
      sudo systemctl daemon-reload && sudo systemctl enable --now jsui-wm-gpu-boost.service &&
      echo "GPU touch boost enabled (applies when the session next starts)."
  fi
  # adb: a restarted adbd gets its USB gadget re-attached (adbd-rebind.conf).
  if systemctl cat adbd.service >/dev/null 2>&1 && [ -d /sys/kernel/config/usb_gadget ]; then
    sudo install -m 644 -D "$DEST/adbd-rebind.conf" /etc/systemd/system/adbd.service.d/jsui-rebind.conf &&
      sudo systemctl daemon-reload && echo "adbd: USB re-attached after a restart."
  fi
  # Wi-Fi: the stock vendor loads a Wi-Fi module this kernel refuses; wifi-on.sh
  # loads the kernel's own, and only once that worked installs it for every boot.
  if [ -e /vendor/lib/modules/qca_cld3_wlan.ko ] && modinfo wlan >/dev/null 2>&1 &&
     ! systemctl is-enabled jsui-wm-wifi.service >/dev/null 2>&1; then
    sh "$DEST/wifi-on.sh" || echo "Wi-Fi not set up (see above)."
  fi
  ;;
esac
case " $* " in *" --now "*) now=1 ;; *) now= ;; esac
if [ -n "$now" ]; then
  systemctl --user restart lomiri-full-shell.service lomiri-full-greeter.service 2>/dev/null || true
else
  echo "Start it by rebooting (or restart lomiri-full-shell/lomiri-full-greeter.service)."
fi
echo "Log: \$XDG_RUNTIME_DIR/jsui-wl.log. Back to Lomiri: sh $DEST/uninstall.sh"
