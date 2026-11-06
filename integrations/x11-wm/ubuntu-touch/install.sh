#!/bin/sh
# Install jsui-wm on Ubuntu Touch (tested target: Galaxy Tab A7 SM-T500,
# Ubuntu Touch 24.04). Run on the tablet as the phablet user, next to
# jsui-wm-ubuntu-touch.tar.gz (or from the unpacked jsui-wm directory):
#
#   sh install.sh            # unpack to ~/jsui-wm and switch the shell
#   sh install.sh --now      # ... and restart the session right away
#
# Nothing outside your home directory changes: a systemd user drop-in makes
# Lomiri's launcher (lomiri-systemd-wrapper) run ~/jsui-wm/jsui-session
# instead of lomiri. uninstall.sh removes the drop-in again.
set -e
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

D="$HOME/.config/systemd/user/lomiri-full-shell.service.d"
mkdir -p "$D"
cat > "$D/jsui-wm.conf" <<CONF
# jsui-wm instead of Lomiri (remove this file, or run ~/jsui-wm/uninstall.sh, to go back)
[Service]
Environment=LOMIRI_BINARY=$DEST/jsui-session
# jsui-session runs jsui-wl as a child, which reports READY to systemd.
NotifyAccess=all
CONF
systemctl --user daemon-reload
echo "jsui-wm installed in $DEST."
if [ "$1" = "--now" ]; then
  systemctl --user restart lomiri-full-shell.service
else
  echo "Start it with: systemctl --user restart lomiri-full-shell.service (or reboot)."
fi
echo "Log: \$XDG_RUNTIME_DIR/jsui-wl.log. Back to Lomiri: sh $DEST/uninstall.sh"
