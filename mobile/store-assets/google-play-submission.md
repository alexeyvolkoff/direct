# Google Play Console — Пакет публикации `Disk Direct` (`net.onmydisk.app`)

> [!IMPORTANT]
> Все лимиты символов проверены под требования Google Play Console (`App name` ≤ 30 симв., `Short description` ≤ 80 симв., `Full description` ≤ 4000 симв.). Все тексты листинга и скриншоты подготовлены специально для Android-экосистемы (на скриншотах фигурируют имена узлов `Pixel 9 Pro` / `Android Tablet` / `Home-NAS` / `Linux-Workstation`).

---

## 1. Основные параметры и ссылки (`Store settings` & `App content`)

| Поле в Google Play Console | Значение |
| :--- | :--- |
| **Package Name** | `net.onmydisk.app` |
| **App Name** | `Disk Direct` |
| **Category** | `Productivity` *(Продуктивность)* |
| **Privacy Policy URL** | `https://disk.direct/tos#gdpr` |
| **Website** | `https://disk.direct` |
| **Support / Docs URL** | `https://docs.disk.direct/support/` |
| **Managed Publishing** | Включить (`Managed publishing on`) для синхронного релиза с iOS |

---

## 2. `Main store listing` — English (`en-US`)

### App name *(11 / 30 chars)*
```text
Disk Direct
```

### Short description *(79 / 80 chars)*
```text
Direct P2P personal cloud, automatic photo backup, knowledge base & private AI.
```

### Full description
```text
Disk Direct turns your own devices — your Windows, macOS, or Linux PC, home server, NAS, or OpenWrt router with a USB drive — into a fast, zero-cloud personal workspace connected directly peer-to-peer over encrypted WebRTC channels.

Your files stay on your hardware. There is no third-party cloud storage limit, no central password database, and no middleman holding your data.

KEY FEATURES

• Automatic Photo & Video Backup
Continuously back up and synchronize your Android camera roll and media folders directly to your home computer, NAS, or router drive in original full resolution with intact EXIF metadata.

• Direct Peer-to-Peer File Access
Browse, stream, upload, organize, and share files across all your paired devices in real time over encrypted WebRTC DataChannels.

• Instant Device Pairing & Custom Gateways (BYOS)
Pair your desktop, server, or router storage nodes in seconds via QR code or 10-character Link ID, send files straight from any Android app via the system Share menu, or connect to your own self-hosted gateway.

• Personal Knowledge Base (RAG) & Private AI Assistant
Install your own self-hosted AI node to automatically build a searchable visual knowledge base from your photos and documents, or use the built-in public demo assistant directly from the sidebar for document Q&A, chat, and creative studio illustrations.

• Rich Media & Office Document Suite
Preview high-resolution photo galleries, stream video and audio, and view or edit Markdown, code, spreadsheets, presentations, and documents directly in the app.

• Decentralized Cryptographic Identity (DID)
Sign in with Google or a self-custodial cryptographic keypair. Paired device links and preferences sync end-to-end encrypted across your devices.

Self-hosting guides & node installation for Linux, Windows, macOS, Docker, and OpenWrt: https://docs.disk.direct

© 2026 Alexey Volkov
```

---

## 3. Перевод страницы — Russian (`ru-RU`)

### App name *(11 / 30 симв.)*
```text
Disk Direct
```

### Short description *(78 / 80 симв.)*
```text
Прямое P2P-облако на ваших устройствах, автобэкап фото, база знаний и ИИ-диск.
```

### Full description
```text
Disk Direct превращает ваши собственные устройства — домашний ПК на Windows, macOS или Linux, сервер, NAS или роутер OpenWrt с USB-диском — в быстрое личное облако с прямым P2P-соединением по зашифрованным каналам WebRTC.

Ваши файлы остаются только на вашем оборудовании — без сторонних облачных хранилищ, ограничений по объёму и централизованных баз паролей.

ОСНОВНЫЕ ВОЗМОЖНОСТИ

• Автоматическая синхронизация фото и видео
Автоматически сохраняйте и синхронизируйте снимки и видеоролики с вашего Android-устройства в оригинальном качестве и с сохранением EXIF-метаданных прямо на домашний компьютер, NAS или диск роутера.

• Прямой P2P-доступ к файлам
Просматривайте, загружайте, упорядочивайте файлы и делитесь ссылками напрямую со своих подключённых устройств в реальном времени через зашифрованные каналы WebRTC.

• Быстрое подключение, меню «Поделиться» и свои шлюзы (BYOS)
Привяжите домашний узел за пару секунд по QR-коду или Link ID, отправляйте файлы из любых приложений Android напрямую на свой диск и при желании подключайтесь к собственному шлюзу.

• Личная база знаний (RAG) и приватный ИИ-ассистент
Установите собственный узел, чтобы автоматически формировать визуальную базу знаний из ваших фотографий и документов, или открывайте Базу знаний, студию Creations и Чаты прямо из боковой панели проводника через встроенный демо-узел.

• Просмотр медиа и работа с документами
Удобная фотогалерея, потоковое воспроизведение видео и аудио, просмотр и редактирование текстовых файлов, таблиц, презентаций, кода и документов прямо в приложении.

• Децентрализованная криптографическая учётная запись (DID)
Вход через Google или криптографический ключ со сквозным шифрованием настроек и списка привязанных устройств.

Документация и установка собственного узла для Linux, Windows, macOS, Docker и OpenWrt: https://docs.disk.direct

© 2026 Alexey Volkov
```

---

## 4. `App content` → Инструкции для ревьюера (`App access`) и декларация `Photo and video permissions`

### App access (Demo Account)
- **Username:** `demouser`
- **Password:** `demouser1234`
- **Instructions:**
```text
Disk Direct is a peer-to-peer personal cloud client (https://disk.direct / https://docs.disk.direct).
1. Sign in with the provided demo credentials (demouser / demouser1234) or create a new free account right in the app (no external hardware or paid subscription required).
2. Browse folders, upload/sync photos and documents from the device, preview media, and edit text/office files.
3. Tap Chats, Creations, or Knowledge in the left sidebar and accept the Responsible Use notice to test AI Q&A, Knowledge Base cards, and safe image generation on the public demo node.
4. Account deletion is available anytime in Settings -> Profile -> Danger Zone -> "Delete Account".
```

### Photo and video permissions (`READ_MEDIA_IMAGES` / `READ_MEDIA_VIDEO`)
- **Access type:** `Persistent / broad access` (Backup, sync, and file management)
- **Justification:**
```text
Disk Direct is a personal P2P cloud storage and file synchronization client. Its core feature is automatic and continuous gallery backup and two-way synchronization of photos and videos between the user's Android device and their self-hosted storage node over an encrypted peer-to-peer WebRTC connection. Broad access to READ_MEDIA_IMAGES and READ_MEDIA_VIDEO (and ACCESS_MEDIA_LOCATION to preserve original EXIF metadata during backup) is strictly required to scan device camera/media folders, detect newly captured photos and videos, and synchronize full-resolution media libraries without requiring manual per-file selection in a one-time picker.
```

### Release notes (`Production`)
```xml
<en-US>
Disk Direct 3.1: rebranded UI with direct sidebar access to Knowledge Base, Creations Studio, and Chats, custom BYOS gateway support, automatic camera sync, and faster WebRTC P2P file streaming.
</en-US>
<ru-RU>
Disk Direct 3.1: обновлённый интерфейс с быстрым доступом к Базе знаний, студии Creations и Чатам из боковой панели, поддержка собственных шлюзов (BYOS), автосинхронизация фото и ускоренный P2P-обмен по WebRTC.
</ru-RU>
```

---

## 5. Файлы графики и скриншотов в `mobile/store-assets/google-play/`

1. **App icon (`512 × 512` px, RGB PNG):** `icon_512x512.png`
2. **Feature graphic (`1024 × 500` px, RGB PNG):** `feature_graphic_1024x500.png`
3. **Phone screenshots (`1080 × 1920`, `9:16`, RGB PNG):** `phone/01_files.png` … `phone/06_settings.png`
4. **7-inch tablet screenshots (`1920 × 1200`, `16:10`, RGB PNG):** `tablet_7/01_files.png` … `tablet_7/06_settings.png`
5. **10-inch tablet screenshots (`2560 × 1600`, `16:10`, RGB PNG):** `tablet_10/01_files.png` … `tablet_10/06_settings.png`

