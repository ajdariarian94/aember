# Opt-in packages

aember's images carry only what a board lists. Besides the services
(`services.json`) and containers (`containers.json`), a board can add
prebuilt programs from aember-resources: SSH, ext4 tools, Wi-Fi. List them
in the board's overlay, `resources/<arch>/rootfs/etc/aember/packages.json`:

```json
{
  "packages": ["dropbear", "wifi"]
}
```

`aember app build` then downloads just those (with kernel, BusyBox, LXC and
the SDK) and `aember app pack` puts their files into the root filesystem,
with the libraries they need. A package that isn't listed is neither
downloaded nor packed. A program in `services.json` that the image doesn't
have (an SSH server without its package) fails the pack.

| Package     | What                                               | Size (x64) |
|-------------|----------------------------------------------------|------------|
| `dropbear`  | SSH server and client (`dropbear`, `ssh`, `scp`)   | 0.7 MB     |
| `e2fsprogs` | ext4 tools: `e2fsck`, `mke2fs`, `resize2fs`, `tune2fs`, `e2label` | 1.7 MB |
| `wifi`      | Wi-Fi client: `wpa_supplicant` (WPA2, WPA3, WPA2-Enterprise), `wpa_cli`, `iw`, regulatory database; Raspberry Pi 4 firmware on aarch64 | 3.7 MB |
| `wifi-ap`   | Wi-Fi access point: `hostapd` (WPA2, WPA3)         | 2.6 MB     |
| `connman`   | ConnMan network manager (instead of aember-netd) with `connmanctl` and the D-Bus system bus | 7.8 MB |

Time sync needs no package: BusyBox has `ntpd`.

**A service of a package** is two things in the board's overlay: its
folder, `usr/lib/aember/services/<name>/config.json` (the settings below),
and its name in `etc/aember/services.json`, with what it needs first:
`{"name": "dropbear", "dependencies": ["aember-netd"]}`.

## SSH (`dropbear`)

`usr/lib/aember/services/dropbear/config.json`, listed in services.json
with `"dependencies": ["aember-netd"]`:

```json
{
  "name": "dropbear",
  "executable": "/usr/sbin/dropbear",
  "args": ["-F", "-E", "-R", "-p", "22"],
  "restart_on_failure": true, "max_restarts": 0, "restart_delay_ms": 2000
}
```

`-R` creates the host key on the first connection (in `/etc/dropbear`).
Logins are by key only: put your public key into the overlay at
`resources/<arch>/rootfs/root/.ssh/authorized_keys`. `-E` logs to the
console.

## Time sync (BusyBox `ntpd`)

Boards without a real-time clock (the Raspberry Pi) start in the past;
TLS in containers and log times need the right time.

`usr/lib/aember/services/ntpd/config.json`, listed with
`"dependencies": ["aember-netd"]`:

```json
{
  "name": "ntpd",
  "executable": "/bin/busybox",
  "args": ["ntpd", "-n", "-p", "pool.ntp.org"],
  "restart_on_failure": true, "max_restarts": 0, "restart_delay_ms": 10000
}
```

Not with `aember app launch --chroot`: there it would set the host's clock.

## ext4 tools (`e2fsprogs`)

Nothing to configure: `e2fsck`, `mke2fs`/`mkfs.ext4`, `resize2fs`,
`tune2fs` and `e2label` are in `/sbin`, e.g. to check or grow a data
partition from a service.

## Wi-Fi client (`wifi`)

1. Kernel modules, in the board's `modules.json`: `cfg80211`, `mac80211`
   and the driver: `brcmfmac` on the Raspberry Pi 4 (its firmware comes
   with the package on aarch64).
2. The interface in `network.json`, **not** `required`: netd waits until
   wpa_supplicant has joined the network, then runs DHCP.

   ```json
   {"name": "wlan0", "mode": "dhcp", "required": false}
   ```

3. `resources/<arch>/rootfs/etc/aember/wpa_supplicant.conf`:

   ```
   ctrl_interface=/run/wpa_supplicant
   country=DE
   network={
       ssid="my-network"
       psk="my-passphrase"
   }
   ```

   WPA3: `key_mgmt=SAE`, `sae_password="..."` and `ieee80211w=2` instead
   of `psk`. Enterprise networks: `key_mgmt=WPA-EAP` with PEAP, TTLS or
   TLS.

4. The service (`usr/lib/aember/services/wpa_supplicant/config.json`, and
   `{"name": "wpa_supplicant"}` in services.json):

   ```json
   {
     "name": "wpa_supplicant",
     "executable": "/usr/sbin/wpa_supplicant",
     "args": ["-i", "wlan0", "-D", "nl80211", "-c", "/etc/aember/wpa_supplicant.conf"],
     "restart_on_failure": true, "max_restarts": 0, "restart_delay_ms": 3000
   }
   ```

`wpa_cli -p /run/wpa_supplicant -i wlan0 status` shows the connection.

## Wi-Fi access point (`wifi-ap`)

hostapd runs the network; BusyBox's `udhcpd` hands out addresses. The AP
interface gets a static address in `network.json`
(`{"name": "wlan1", "mode": "static", "address": "192.168.50.1/24"}`),
then:

- `etc/aember/hostapd.conf`:

  ```
  interface=wlan1
  driver=nl80211
  ctrl_interface=/run/hostapd
  ssid=my-device
  hw_mode=g
  channel=6
  wpa=2
  wpa_key_mgmt=SAE
  sae_password=change-me
  rsn_pairwise=CCMP
  ieee80211w=2
  ```

- `etc/aember/udhcpd.conf`:

  ```
  interface wlan1
  start 192.168.50.100
  end 192.168.50.150
  lease_file /tmp/udhcpd.leases
  option subnet 255.255.255.0
  option router 192.168.50.1
  ```

- services: `/usr/sbin/hostapd /etc/aember/hostapd.conf` (after
  `aember-netd`) and `/bin/busybox udhcpd -f /etc/aember/udhcpd.conf`
  (after `hostapd`).

## ConnMan instead of aember-netd (`connman`)

ConnMan manages Ethernet and Wi-Fi itself (DHCP, DNS proxy, saved
networks, roaming). It is the bigger choice: 7.8 MB, about half of it
GLib; aember-netd + `wifi` covers a fixed network in 3.7 MB. Wi-Fi needs
the `wifi` package too: `{"packages": ["wifi", "connman"]}`.

- `services.json`: drop `aember-netd`, let `aember-containerd` depend on
  `connman`, and add D-Bus, wpa_supplicant (D-Bus mode) and ConnMan. The
  last two wait for the bus socket, so they don't start before D-Bus is
  up:

  ```json
  {"services": [
    {"name": "dbus"},
    {"name": "wpa_supplicant", "dependencies": ["dbus"]},
    {"name": "connman", "dependencies": ["dbus", "wpa_supplicant"]},
    {"name": "aember-containerd", "dependencies": ["connman"]}]}
  ```

  and their folders, `usr/lib/aember/services/<name>/config.json`:

  ```json
  {"name": "dbus", "executable": "/bin/sh",
   "args": ["-c", "mkdir -p /run/dbus; [ -s /etc/machine-id ] || dbus-uuidgen > /etc/machine-id; exec /usr/bin/dbus-daemon --system --nofork --nopidfile"],
   "restart_on_failure": true, "max_restarts": 0, "restart_delay_ms": 2000}
  {"name": "wpa_supplicant", "executable": "/bin/sh",
   "args": ["-c", "while [ ! -S /run/dbus/system_bus_socket ]; do sleep 0.2; done; exec /usr/sbin/wpa_supplicant -u"],
   "restart_on_failure": true, "max_restarts": 0, "restart_delay_ms": 3000}
  {"name": "connman", "executable": "/bin/sh",
   "args": ["-c", "while [ ! -S /run/dbus/system_bus_socket ]; do sleep 0.2; done; exec /usr/sbin/connmand -n"],
   "restart_on_failure": true, "max_restarts": 0, "restart_delay_ms": 3000}
  ```

- `etc/connman/main.conf`: keep ConnMan off the container interfaces, and
  let Wi-Fi connect while Ethernet is up (ConnMan stops auto-connecting at
  the first connected technology that isn't listed here):

  ```
  [General]
  NetworkInterfaceBlacklist = vmnet,vboxnet,virbr,ifb,veth,lxcbr
  AlwaysConnectedTechnologies = ethernet,wifi
  ```

- A Wi-Fi network to join, `var/lib/connman/<name>.config`:

  ```
  [service_home]
  Type = wifi
  Name = my-network
  Passphrase = my-passphrase
  ```

- Kernel modules (`modules.json`): `nf_tables` (ConnMan's firewall), and
  the Wi-Fi ones above.

`connmanctl state` and `connmanctl services` show what it did;
`/etc/resolv.conf` points at its DNS proxy (127.0.0.1).

## Testing Wi-Fi in QEMU

Every aember kernel has `mac80211_hwsim`, which simulates two radios
(`wlan0`, `wlan1`) connected to each other. List `mac80211_hwsim` in
`modules.json`, both Wi-Fi packages, run the access point above on `wlan1`
and the client on `wlan0`: the client joins over WPA3 and gets
`192.168.50.100` from udhcpd.
