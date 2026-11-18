#!/bin/bash
# Runs inside a clean Ubuntu VM (as root, from cloud-init), started by
# tools/tests/appimage-vm-test.sh. Drives the AppImage like a user:
# double-click equivalent on a display, pkexec's password prompt in the
# window, aember running isolated, closing the window. Results go to /out
# (a 9p share) as "AITEST key=value" lines, plus screenshots.
set -u
OUT=/out
exec > >(tee "$OUT/guest.log") 2>&1
say() { echo "AITEST $*"; }
as_user() { su - tester -c "DISPLAY=:99 $*"; }
PASSWORD=aember-test

say kernel="$(uname -r)" release="$(. /etc/os-release; echo "$VERSION")"
say pkexec="$(command -v pkexec || echo none)" fuse="$([ -c /dev/fuse ] && echo yes || echo no)" \
    fusermount="$(command -v fusermount3 || command -v fusermount || echo none)" \
    xterm_installed="$(command -v xterm || echo no)"

# Only test tools; nothing aember needs (xterm, slirp4netns) is installed.
apt-get update -qq >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq xvfb xdotool imagemagick x11-xserver-utils >/dev/null 2>&1
say tools_installed=$?

cp /out/aember.AppImage /home/tester/aember.AppImage
chown tester: /home/tester/aember.AppImage
chmod +x /home/tester/aember.AppImage

# Interfaces, addresses (without the lease's countdown), routes, firewall.
net_state() { ip -o link | sed 's/ link\/.*//'; ip -o -4 addr | sed 's/ valid_lft.*//'; ip route; nft list ruleset 2>/dev/null; sysctl -n net.ipv4.ip_forward; }
net_state >/tmp/net-before

su - tester -c 'Xvfb :99 -screen 0 1400x900x24 >/dev/null 2>&1 &'
sleep 3

scenario() {  # $1: how root is asked for (sudo | pkexec)
MODE=$1

# "Double-click": start it with a display and no terminal, in a logind
# session of its own like a desktop login (polkit only authenticates
# processes in a session: "No session for cookie" otherwise).
systemd-run -q --unit="aember-$MODE" -p PAMName=login -p User=tester \
  -p Environment=DISPLAY=:99 /home/tester/aember.AppImage

WIN=""
for _ in $(seq 1 60); do
  WIN=$(as_user "xdotool search --name '^aember$'" 2>/dev/null | head -1)
  [ -n "$WIN" ] && break
  sleep 1
done
say $MODE.window="${WIN:-none}"

# pkexec asks in the window (polkit's text agent: no desktop agent here).
sleep 6
as_user "import -window root /out/$MODE-1-password-prompt.png" 2>/dev/null
# No window manager under Xvfb: focus directly, then real (XTEST) keys.
as_user "xdotool windowfocus --sync $WIN" 2>/dev/null
as_user "xdotool type --delay 50 '$PASSWORD'" 2>/dev/null
as_user "xdotool key Return" 2>/dev/null
sleep 15
as_user "import -window root /out/$MODE-1b-after-password.png" 2>/dev/null
if ! pgrep -x aember >/dev/null; then
  # pkexec without a desktop agent: the launcher falls back to sudo.
  say $MODE.second_prompt=yes
  as_user "xdotool windowfocus --sync $WIN" 2>/dev/null
  as_user "xdotool type --delay 50 '$PASSWORD'" 2>/dev/null
  as_user "xdotool key Return" 2>/dev/null
fi

PID=""
for _ in $(seq 1 90); do
  PID=$(pgrep -x aember | head -1)
  [ -n "$PID" ] && grep -q "started successfully" /proc/"$PID"/root/var/log/aember-init.log 2>/dev/null && break
  sleep 1
done
say $MODE.aember_pid="${PID:-none}"
if [ -n "$PID" ]; then
  say $MODE.aember_user="$(ps -o user= -p "$PID")"
  say $MODE.aember_nspid="$(awk '/^NSpid/{print $NF}' /proc/$PID/status)"   # 1 = PID 1 in its namespace
  for ns in pid mnt net cgroup uts ipc; do
    same=$([ "$(readlink /proc/$PID/ns/$ns)" = "$(readlink /proc/1/ns/$ns)" ] && echo same || echo own)
    say $MODE.ns_$ns=$same
  done
  sleep 20  # services and containers start
  say $MODE.aember_eth0="$(nsenter -t "$PID" -n ip -4 -o addr show eth0 | awk '{print $4}')"
  # From inside aember (its resolver, its BusyBox): the NAT works ...
  say $MODE.aember_internet="$(nsenter -t "$PID" -a -r -w /bin/busybox wget -q -T 10 -O /dev/null http://example.com/ && echo yes || echo no)"
  # ... the host's loopback (10.0.2.2 = slirp's gateway) does not.
  say $MODE.aember_reaches_host_lo="$(nsenter -t "$PID" -a -r -w /bin/busybox wget -q -T 3 -O /dev/null http://10.0.2.2:22/ 2>/dev/null && echo yes || echo no)"
  say $MODE.services="$(grep -o "Service '[a-z-]*': starting -> running" /proc/$PID/root/var/log/aember-init.log | sort -u | wc -l)"
  say $MODE.mounts_on_host="$(grep -c aember /proc/1/mounts)"
  as_user "import -window root /out/$MODE-2-aember-running.png" 2>/dev/null
fi

# Close the window, as a user would.
as_user "xdotool windowclose $WIN" 2>/dev/null || as_user "xdotool windowkill $WIN" 2>/dev/null
sleep 10
say $MODE.after_aember_procs="$(pgrep -c -x aember)"
say $MODE.after_slirp_procs="$(pgrep -c slirp4netns)"
say $MODE.after_lxc_procs="$(pgrep -c -f 'lxc monitor')"
say $MODE.after_mounts="$(grep -cE 'aember|appimage' /proc/self/mounts)"
say $MODE.after_cgroups="$(find /sys/fs/cgroup -name 'aember-sandbox-*' -o -name 'lxc.*' 2>/dev/null | wc -l)"
say $MODE.after_tmp="$(ls -d /tmp/aember-appimage.* /tmp/.mount_aember* /tmp/appimage_extracted_* 2>/dev/null | tr "\n" " ")"
say $MODE.after_xhost_root="$(as_user xhost 2>/dev/null | grep -c 'localuser:root')"
net_state >/tmp/net-after
say $MODE.network_unchanged="$(cmp -s /tmp/net-before /tmp/net-after && echo yes || echo no)"
diff /tmp/net-before /tmp/net-after | head -10
}

MODES=$(cat /out/modes 2>/dev/null || echo "sudo pkexec")
for MODE_NAME in $MODES; do
  if [ "$MODE_NAME" = pkexec ]; then
    # As on Ubuntu Desktop, which has pkexec (the server image does not).
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq pkexec >/dev/null 2>&1
    say pkexec_installed="$(command -v pkexec || echo no)"
  fi
  scenario "$MODE_NAME"
done
journalctl -b --no-pager -t pkexec -t polkitd 2>/dev/null | tail -8 | sed 's/^/POLKIT /' 
say DONE
sync
