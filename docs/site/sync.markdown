---
layout: page
title: Folder Synchronization
permalink: /sync/
nav_order: 12
---
# 🔄 Folder Synchronization & Mobile Backup
<hr>

**Disk Direct** mounts all your paired nodes simultaneously in a unified peer-to-peer workspace (`DeviceRegistry`) so you can browse, stream, and copy across devices without duplicating gigabytes across disks — while offering dedicated background backup workflows for mobile cameras and Markdown vaults.

---

## Supported Sync & Backup Workflows

1. **Mobile Camera & Folder Auto-Backup (iOS & Android)**:
   * The native **Disk Direct** mobile app (`Settings → Folder & File Backup`) supports automatic background photo and video backup directly to your home Linux, Windows, macOS, or OpenWrt node over chunked WebRTC DataChannels (`HomeWrite`) in original full resolution with intact EXIF metadata, with zero third-party cloud storage in between.
2. **Obsidian & Markdown Vault Sync**:
   * Synchronize Markdown notes and attachments between desktop/mobile **Obsidian** vaults and your **Disk Direct** node, automatically feeding your notes into your private **Knowledge Base (RAG)**.
3. **External Cloud Mounts (`rclone`)**:
   * Mount **ProtonDrive** or **WebDAV** remotes into your node via **[External Cloud Storage](/external/)**.
4. **Scheduled Disk Backups**:
   * Because **Disk Direct** works directly on native directories without proprietary container formats, standard backup tools (`rsync`, `restic`, `borg`, `rclone sync`, `Robocopy`, Time Machine) work directly against your shared folders on disk.
