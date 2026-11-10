# omd-tunnel

Secure TCP tunnel over the OnMyDisk gateway — zero-config remote access to any
device, ready to use as an `ssh ProxyCommand`.

No OMD account, no profile, no admin rights on the remote side: on first run
`omd-tunnel` generates a self-signed device identity and connects straight to
the gateway. Your home machine just needs a running agent (`onmydisk`) that is
reachable by its link ID.

```
[you: laptop] ---- omd-tunnel ---- [OnMyDisk gateway] ---- agent ---- [home machine:22]
```

## Why

- **No public IP, no port forwarding, no DDNS.** The tunnel is initiated from
  *your* side through the gateway; the target only needs outbound access to it.
- **Works from any network** — office, mobile, public Wi-Fi. Symmetric NAT and
  firewalls do not matter (see [Current status](#current-status)).
- **One binary.** Standalone Qt console app, static builds for Windows and
  Linux, no runtime dependencies.
- **Multi-client.** The `--listen` mode multiplexes several local connections
  over a single tunnel (SSH ControlMaster, SFTP, editor sessions — no kicking).

## Quick start

### Linux (target: home machine)

```sh
# 1. Install
sudo apt install ./omd-tunnel_<ver>_amd64.deb

# 2. Open a tunnel to the home agent on your laptop
omd-tunnel --listen 22222 <home-link-id> 22

# 3. Connect
ssh -p 22222 user@127.0.0.1
```

Or skip step 2/3 and let SSH open the tunnel by itself:

```sh
# ~/.ssh/config
Host home
    HostName 127.0.0.1
    ProxyCommand omd-tunnel <home-link-id> 22
```

Now `ssh home` just works, from anywhere.

### Windows

```sh
# 1. Download omd-tunnel.exe (no install required, no admin)
# 2. Open a tunnel in a terminal
omd-tunnel.exe --listen 22222 <home-link-id> 22

# 3. In another terminal, connect
ssh -p 22222 user@127.0.0.1
```

`--listen` is the recommended mode on Windows: the stdin/stdout pipe mode is
available but listen mode plays nicer with PowerShell and SSH clients.

### Via SSH config (both platforms)

```sh
ssh -o ProxyCommand="omd-tunnel <home-link-id> 22" user@home
```

## How it works

1. `omd-tunnel` connects to the gateway over TLS and authenticates with a
   self-signed certificate (created once in the config dir).
2. The gateway forwards a `TunnelOpen` to the target agent; the agent connects
   to the requested `host:port` and answers.
3. From then on, TCP data is relayed through the gateway in both directions.

```
1. omd-tunnel  --TLS cert-->  gateway  --TunnelOpen-->  agent
2. agent  --TCP open host:port-->  local service
3. gateway relays TunnelData both ways  (omd-tunnel <-> service)
```

## Configuration

Everything is optional — defaults work out of the box.

| Option | Default | Meaning |
| --- | --- | --- |
| `--server` | `disk.direct` (config / `OMD_TUNNEL_SERVER`) | gateway host |
| `--port` | `9000` (config / `OMD_TUNNEL_PORT`) | gateway TLS port |
| `--user` | `omd-tunnel` (config) | certificate CN / user name |
| `--dir` | `~/.config/omd-tunnel` | identity storage |
| `--host` | `127.0.0.1` | remote host on the target |
| `--timeout` | `30` | connection timeout, seconds |
| `--listen` | — | local TCP listener instead of stdin/stdout |
| `--debug` | — | per-packet traffic diagnostics |
| `linkID` | required | link ID of the target device |
| `port` | `22` | TCP port on the target device |

Persistent settings live in `~/.config/omd-tunnel/omd-tunnel.conf`
(INI, `[Network] host/port/user/timeout`). The device identity — `device.id`,
`client.key`, `client.crt` — is generated once in the same directory.

## Identity & security

- On first run a 2048-bit RSA self-signed certificate is created
  (`CN=<user>`, `O=omd-tunnel`). The gateway derives a tunnel-only link from
  it — no device registration happens.
- The control channel to the gateway is TLS (`VerifyNone` is used for the
  gateway's own cert, the private key is stored locally and used to sign the
  TLS handshake).
- Access to a target still requires its link ID. Per-user authorization of
  tunnel certificates is on the roadmap (see below).

## Current status

**PoC / fallback mode.** Traffic is relayed through the gateway today. The
roadmap:

- **Phase 1 — E2EE over the relay:** tunnel payloads encrypted with
  ChaCha20-Poly1305 after an X25519 handshake; the gateway becomes a blind
  relay that cannot decrypt `TunnelData`.
- **Phase 2 — P2P:** direct UDP channel with NAT hole punching (WebRTC data
  channel or a custom STUN + Noise path); the gateway remains signaling /
  relay-only as a transparent fallback.
- **Auth:** certificate issuance for a tunnel requires authorization by the
  device owner (or a user the owner added to the allowed list).

## Building

```sh
# Linux (Debian/Ubuntu package)
./deploy/build-deb.sh

# Windows (static, requires MXE)
./deploy/build-windows-mxe.sh
```

## Examples

```
omd-tunnel --listen 22222 deadbeef01 22          # SSH to home
omd-tunnel --listen 18080 deadbeef01 80          # HTTP to home
omd-tunnel --debug --listen 22223 deadbeef01 22  # watch the traffic
ssh -o ProxyCommand="omd-tunnel deadbeef01 22" user@home
```
