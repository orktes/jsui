#!/bin/sh
# Wi-Fi on the Galaxy Tab A7: run on the tablet as the phablet user,
#
#   sh ~/jsui-wm/wifi-on.sh
#
# 1. loads the Wi-Fi driver and switches the radio on (sudo: your PIN), then
#    lists the networks it sees;
# 2. if wlan0 came up, installs jsui-wm-wifi.service so that happens at every
#    boot (join a network in System Settings > Wi-Fi; NetworkManager keeps it).
# Safe to run again. --force retries after a bring-up that never finished.
HERE=$(dirname "$(readlink -f "$0")")
echo "== 1/2: Wi-Fi driver and radio (sudo may ask for your PIN)"
sudo sh "$HERE/jsui-wm-wifi-up" "$@"
if ! ip link show wlan0 >/dev/null 2>&1; then
  echo "Wi-Fi did not come up; nothing installed. Output above says where it stopped."
  exit 1
fi
nmcli device
echo
nmcli device wifi list
echo
echo "== 2/2: Wi-Fi at every boot"
sudo install -m 755 "$HERE/jsui-wm-wifi-up" /etc/writable/jsui-wm-wifi-up &&
  sudo install -m 644 "$HERE/jsui-wm-wifi.service" /etc/systemd/system/jsui-wm-wifi.service &&
  sudo systemctl daemon-reload &&
  sudo systemctl enable jsui-wm-wifi.service &&
  echo "jsui-wm-wifi.service installed and enabled." ||
  { echo "installing jsui-wm-wifi.service failed."; exit 1; }
echo "Done. Join a network in System Settings > Wi-Fi (or: nmcli device wifi connect SSID password PASSWORD)."
