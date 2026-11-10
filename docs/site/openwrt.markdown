---
layout: page
title: OpenWrt Routers
permalink: /openwrt/
nav_order: 3
---
# 📡 OpenWrt Routers & Embedded Gateways
<hr>

Turn any **OpenWrt** Wi-Fi router, travel router, or CPE gateway with a USB port into a silent (`< 5 W`), always-on **On My Disk Direct** P2P personal cloud and remote SSH/TCP gateway — **with zero port forwarding, zero DDNS, and full compatibility behind ISP CGNAT**.

The OpenWrt node (`DEFINES+=ROUTER`) is built from the exact same native C++ P2P codebase as the Linux and Windows nodes, compiled against **`musl` libc** with embedded **WebRTC DataChannels** (`libdatachannel`) and packaged as standard `.ipk` archives.

---

## 1. Supported OpenWrt Architectures (`.ipk`)

| Architecture | Typical Router SoCs & Targets | Package Download |
|--------------|-------------------------------|------------------|
| **`mips`** *(big-endian)* | Qualcomm Atheros (`ath79`), Lantiq, Realtek, InnBox (`mips_24kc`) | [onmydisk-latest-mips.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-mips.ipk) |
| **`mipsel`** *(little-endian)* | MediaTek `MT7621`, `MT7628` (`ramips` / `mipsel_24kc`) | [onmydisk-latest-mipsel.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-mipsel.ipk) |
| **`armv7`** *(32-bit ARM)* | Cortex-A7 / Cortex-A9 (`ipq40xx`, `mvebu`, `bcm53xx`) | [onmydisk-latest-armv7.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-armv7.ipk) |
| **`arm64`** *(`aarch64`)* | MediaTek Filogic (`mt7981`/`mt7986`), Qualcomm `ipq807x`, Rockchip, RPi | [onmydisk-latest-arm64.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-arm64.ipk) |

### Flash & RAM Recommendations
* **RAM**: `128 MB+` recommended (`256 MB–1 GB` on modern `MT7621` / Filogic / `ipq` routers gives plenty of headroom for concurrent WebRTC streams).
* **Flash Space**: Routers with `128 MB+` NAND/eMMC flash can install `/opt/bin/onmydisk` directly to internal storage. On older routers with small `16–32 MB` SPI NOR flash, configure **OpenWrt `extroot`** (or mount `/opt` onto your external USB drive) before installing the `.ipk`.

---

## 2. Installing `onmydisk` on OpenWrt

Connect to your router via SSH (`ssh root@192.168.1.1`) and check its target architecture:

```sh
# 1. Inspect your OpenWrt CPU architecture:
opkg print-architecture

# 2. Download the matching .ipk (choose mips, mipsel, armv7, or arm64):
wget -O /tmp/onmydisk.ipk https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-arm64.ipk

# 3. Install and clean up temp archive:
opkg update
opkg install /tmp/onmydisk.ipk
rm -f /tmp/onmydisk.ipk
```

Enable autostart on router boot and launch the daemon:

```sh
/etc/init.d/onmydisk enable
/etc/init.d/onmydisk start
```

### Installed Paths on OpenWrt
* **Daemon Binary**: `/opt/bin/onmydisk`
* **Init Script**: `/etc/init.d/onmydisk` (`start` / `stop` / `restart` / `enable`)
* **Configuration & PKI**: `/etc/onmydisk/onmydisk.conf` (and user overrides in `/etc/onmydisk/onmydisk.conf.user`)
* **LAN Discovery Companion**: `/opt/bin/wsdd` (`/etc/init.d/wsdd`)

---

## 3. Claiming the Router (Port `8081` — LuCI-Safe)

Because OpenWrt's **LuCI** administration panel occupies port `80`, the router configuration (`/etc/onmydisk/onmydisk.conf`) binds the local **On My Disk Claim Page** on **port `8081`** and automatically excludes internal system partitions (`IgnoredDevices="/dev/mmcblk0,/dev/zram"`):

1. Plug an external USB flash drive, SSD, or HDD into your router's USB port (formatted as `ext4`, `exFAT`, or `NTFS` mounted under `/mnt/...`).
2. Sign in to [**direct.onmydisk.net**](https://direct.onmydisk.net) in your browser.
3. Open **`http://192.168.1.1:8081`** (replace `192.168.1.1` with your router's LAN IP if different).
4. Click **Connect in On My Disk** to pair the router with your decentralized DID identity (`PairedDIDs`).

Your router immediately appears in the **On My Disk Direct** sidebar and workspace as an always-on storage node.

---

## 4. Remote Router Shell (`WebSSH`) & Reaching LuCI Behind CGNAT

Once paired, your OpenWrt router also acts as a zero-configuration P2P gateway into your home or office network:

* **In-Browser Router Terminal (`WebSSH`)**: Open the **Terminal** workspace in [**direct.onmydisk.net**](https://direct.onmydisk.net) to connect straight to OpenWrt's local `dropbear` / `openssh` server (`127.0.0.1:22`) over an encrypted WebRTC DataChannel.
* **Remote Access to LuCI Without Exposing Port 80**: Use **`omd-tunnel`** on your laptop to forward a local port to your router's LuCI interface (`:80`) from anywhere in the world:
  ```bash
  omd-tunnel --listen 18080 <router-node-linkID> 80
  # Now open http://127.0.0.1:18080 in your browser to use LuCI securely over P2P!
  ```
* **AI Chat Alongside Router Storage**: While the router itself runs in lightweight **Storage-Only** mode (`localAI=false`), **On My Chat** in your browser seamlessly uses **Automatic (Auto)** mode — connecting to the **On My Chat Public Cloud AI Node** (or your home GPU workstation whenever it is awake) while reading and saving files directly on your router's USB drive.
