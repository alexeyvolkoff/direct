---
layout: page
title: Folder Synchronization
permalink: /sync/
nav_order: 12
---
# 🔄 Folder Synchronization & Mobile Backup
<hr>

> **OMD 3.0 Architecture Note**: The legacy node-to-node `SyncEngine` mirroring rules from 2.x were retired in **On My Disk 3.0** in favor of direct multi-device P2P streaming (`DeviceRegistry`), where all paired nodes are simultaneously mounted without duplicating gigabytes across disks. Existing 2.x sync entries in `onmydisk.conf` are safely ignored.
{: .note }

---

## Supported Sync & Backup Workflows in 3.0

1. **Mobile Camera & Folder Auto-Sync (Android / iOS)**:
   * The native **On My Disk** mobile app supports automatic background photo/video backup directly to your home Linux or Windows node over chunked WebRTC DataChannels (`HomeWrite`), with zero third-party cloud storage in between.
2. **Obsidian Vault Sync (`onmydisk-sync` Plugin)**:
   * Synchronize Markdown notes and attachments between desktop/mobile **Obsidian** vaults and your **On My Disk** node, automatically feeding your notes into **On My Chat** RAG.
3. **External Cloud Mounts (`rclone`)**:
   * Mount **ProtonDrive** or **WebDAV** remotes into your node via **[External Cloud Storage](/external/)**.
4. **Scheduled Disk Backups**:
   * Because **On My Disk** works directly on native directories without proprietary container formats, standard tools (`rsync`, `restic`, `borg`, `rclone sync`, Robocopy) work directly against your shared folders on disk.
