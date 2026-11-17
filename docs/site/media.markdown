---
layout: page
title: Media Library
permalink: /media/
nav_order: 6.5
---
# 🖼️ Media Library (Timeline & Smart Auto-Albums)
<hr>

**Media Library** turns every photo and video indexed in your **[Knowledge Base](/rag/)** into a fast, private, zero-cloud photo & video experience - accessible in one click from the left sidebar in both **Disk Direct** (`#/media`) and **AI Assistant Direct** (`#/chat/media`).

Instead of maintaining a separate cloud gallery database, **Media Library** builds your chronological timeline and automatic albums directly on top of your existing multimodal **Knowledge** records (`ChromaDB` on your paired node + client-encrypted `GunDB` cards from your devices).

---

## 1. Chronological Timeline & Date Scrubber

Switch to **Timeline** in the top segmented bar to browse all your indexed photos and videos ordered by capture date:

* **True Capture Timestamps**: Dates are resolved automatically from camera **EXIF `DateTimeOriginal`**, sidecar metadata, filename timestamps (`IMG_YYYYMMDD_HHMMSS`), or file creation dates.
* **Sticky Day & Place Headers**: Each day group displays a sticky date header paired with the dominant city/place recognized from GPS metadata for that day.
* **Interactive Year-Month Date Scrubber**: Jump across months and years of thousands of photos instantaneously using the vertical scrubber along the right edge, backed by progressive 60 fps lazy rendering.
* **Toolbar Controls**:
  * **Search**: Filter instantaneously across cities, countries, recognized people, AI scene descriptions, OCR text, and `#tags`.
  * **Sort Order** (`Newest first` / `Oldest first`): Flip chronological direction in one click (matching **Creations**).
  * **Grid Aspect Toggle**: Switch between uniform `1:1` square tiles and full-aspect (`4:3` contain) framing.
  * **Media Type Pills**: When videos are present, filter between **All**, **Photos**, and **Videos**.

---

## 2. Smart Auto-Albums

Switch to **Auto-Albums** to explore automatic collections assembled locally from face recognition, GPS reverse geocoding, capture timestamps, and vision tags:

1. **👥 People**: Horizontal shelf of recognized family, friends, and teammates powered by on-device facial recognition (`InsightFace` on your AI node and face tagging in **Memory Cards**).
2. **🗓️ Moments & Trips**: Automatic chronological and geographical event clusters (weekend getaways, vacations, and multi-photo gatherings) complete with 3-photo collage covers and `City · Date Range` titles.
3. **📍 Places**: Grouped by city and country extracted from camera EXIF GPS coordinates and location metadata.
4. **🏷️ Themes & Tags**: Meaningful visual themes and scene `#hashtags` (`#mountains`, `#sunset`, `#architecture`, `#cat`, etc.), with technical noise tags filtered out automatically.

---

## 3. Scoped Photo & Video Viewers

* **Unified Photo & Video Indexing**: Both photos (`.jpg`, `.jpeg`, `.png`, `.webp`, `.heic`, `.avif`) and videos (`.mp4`, `.mov`, `.webm`, `.m4v`, `.mkv`) are indexed with poster thumbnails extracted via `<video>` canvas on mobile/browser or cached via `ffmpeg` on your AI node.
* **Strictly Scoped Lightbox & Playback**: Clicking a photo or video opens the fullscreen `lightGallery` viewer or `VideoViewer` **strictly scoped to the day group or Auto-Album you clicked** (rather than scrolling through an entire raw storage folder). The same scoped gallery behavior applies when clicking photo sources under **Chats** answers.
* **Quick Knowledge Card Inspector (`ⓘ`)**: Hover or tap the `ⓘ` button on any tile to inspect or edit its underlying **Memory Card** caption, `#tags`, and marked people.

---

## 4. `+ Creations` Filter & `Work / Fun` Privacy

* **Clean Camera Roll by Default**: AI-generated artworks from **[Creations](/creations/)** are excluded from your personal photo timeline by default. Click **`+ Creations`** in the top toolbar anytime to blend generated images and videos into your timeline and albums.
* **Contextual `Work / Fun` Toggle**: When **`+ Creations`** is enabled, the **`Work / Fun`** mode pill appears in the toolbar. Any `#sensitive` media stays blurred and locked in **Work** mode across **Media Library**, **Memory Cards**, and **Creations** (and remains permanently blurred on public shared links until downloaded locally).

---

## 5. Mobile Indexing & Offline Device Handling

Click **Index this device** in the toolbar on **Disk Direct** mobile or web to scan camera roll folders (`DCIM`, `Camera`, `Pictures`, `Movies`), extract EXIF dates & GPS coordinates, generate compact encrypted `base64` thumbnails in **GunDB**, and run vision analysis (including hardware-accelerated on-device recognition on Android, with iOS on-device vision support arriving next, plus seamless P2P AI node fallback).

When you browse **Media Library** on your laptop while your phone is **offline**:
* **Zero Broken Requests**: Direct detects from `deviceRegistry` that the source device is offline and never attempts to fetch unavailable full-resolution files over `/p2p/`.
* **Instant Encrypted Previews**: Tiles and Auto-Album covers display the embedded `base64` thumbnail stored in your Knowledge Base as a non-clickable preview with a subtle offline device badge (or a clean **Device offline** placeholder if no thumbnail was stored), while keeping the `ⓘ` card metadata button available. Once your phone comes back online over P2P, full-resolution streaming and lightbox zoom unlock automatically.
