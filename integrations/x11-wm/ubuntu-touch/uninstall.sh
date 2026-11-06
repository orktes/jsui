#!/bin/sh
# Back to Lomiri: removes the drop-in install.sh added and restarts the
# session. ~/jsui-wm stays (delete it by hand if you like).
for UNIT in lomiri-full-shell lomiri-full-greeter; do
  rm -f "$HOME/.config/systemd/user/$UNIT.service.d/jsui-wm.conf"
  rmdir "$HOME/.config/systemd/user/$UNIT.service.d" 2>/dev/null
done
systemctl --user daemon-reload
echo "Lomiri restored."
[ "$1" = "--keep-session" ] || systemctl --user restart lomiri-full-shell.service lomiri-full-greeter.service 2>/dev/null
