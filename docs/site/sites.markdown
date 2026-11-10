---
layout: page
title: Sites Engine
permalink: /sites/
nav_order: 10
---
# 🌐 Sites Engine & P2P Jekyll Hosting
<hr>

**On My Disk 3.0** includes a built-in **Sites Engine** that lets you build and publish static websites (documentation portals, blogs, portfolios, and landing pages) directly from your own Linux storage node — served over **P2P ServiceWorker page tunnels** with optional custom domain binding.

---

## 1. Prerequisites & Service Setup (Linux Node)

`onmydisk-connector` installs Ruby, Bundler, and **Jekyll** automatically during package setup (`sitesRoot=/var/onmydisk/sites`, `sitesEnabled=true` in **Device Settings**).

Verify Jekyll is available:

```bash
which jekyll
# /usr/local/bin/jekyll
```

If you run multiple user sites via a dedicated `sites.service` unit (`/etc/systemd/system/sites.service`):

```ini
[Unit]
Description=On My Disk Sites Service
After=network.target

[Service]
Type=simple
User=your_username
WorkingDirectory=/usr/lib/onmydisk/sites
Environment="PATH=/home/your_username/.rbenv/shims:/home/your_username/.rbenv/bin:/home/your_username/gems/bin:/usr/local/bin:/usr/bin:/bin"
Environment="GEM_HOME=/home/your_username/gems"
ExecStart=/bin/bash /usr/lib/onmydisk/sites/serve-all.sh
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Reload systemd after any unit edits:

```bash
sudo systemctl daemon-reload
sudo systemctl restart onmydisk
```

---

## 2. Creating a Website with the Site Wizard

1. In the **On My Disk** File Manager, enable **Sites** in your node's **Device Settings** and open the **Sites** share on your device.
2. Launch the **New Website** wizard from the toolbar.
3. Enter:
   * **Site Name**: Folder slug (letters and numbers, no spaces).
   * **Title & Description**: Site metadata written into `_config.yml`.
   * **Template**: Choose a starter theme (documentation knowledge base such as `just-the-docs`, blog, or landing page).
4. `serve-all.sh` automatically allocates a local port (e.g., `4001`–`4006`), starts `jekyll serve`, and watches your Markdown files for live rebuilds whenever you edit them over P2P or SSH.

### How Visitors Reach Your Site
Sites are delivered directly from your node over P2P: the gateway resolves `/api/site-meta` (including custom `Host` headers via nginx pointing to `/sites/<site_name>/`), and `site-view.html` streams pages and assets over a ServiceWorker-backed P2P tunnel.
