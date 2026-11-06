#!/bin/sh
# Back to Lomiri: removes the drop-in install.sh added and restarts the
# session. ~/jsui-wm stays (delete it by hand if you like).
rm -f "$HOME/.config/systemd/user/lomiri-full-shell.service.d/jsui-wm.conf"
rmdir "$HOME/.config/systemd/user/lomiri-full-shell.service.d" 2>/dev/null
systemctl --user daemon-reload
echo "Lomiri restored."
[ "$1" = "--keep-session" ] || systemctl --user restart lomiri-full-shell.service
