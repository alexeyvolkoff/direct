<script>
  import { createEventDispatcher, onMount, onDestroy, tick } from "svelte";
  import { fly } from "svelte/transition";
  import { _, locale } from "../lib/i18n/index.js";
  import { Chat } from "@ai-sdk/svelte";
  import { marked } from "marked";
  import markedKatex from "marked-katex-extension";
  import "katex/dist/katex.min.css";
  import lightGallery from "lightgallery";
  import { setupGalleryPagination, isJustFileName } from "../lib/gallery-pagination.js";
  import { OnmyChatTransport } from "../lib/onmychat-transport.js";
  import { ttsPlayer, ttsState } from "../lib/tts-player.js";
  import AssistantAvatarModal from "./AssistantAvatarModal.svelte";
  import { getViewerSettings } from "../lib/viewer-settings.js";
  import { deviceRegistry } from "../lib/device-registry.js";
  import { profileStore, resolveChatUserName } from "../lib/profile-store.js";
  import DocumentPicker from "./DocumentPicker.svelte";
  import ImagePicker from "./ImagePicker.svelte";
  import FolderPicker from "./FolderPicker.svelte";
  import { uploadDocumentForImport, importUploadedFile, FILE_INPUT_ACCEPT } from "../lib/document-text.js";
  import { uploadImageForRecognize, recognizeUploadedImage } from "../lib/document-text.js";
  import { IMAGE_ACCEPT } from "../lib/file-accept.js";
  import { issueConsent, getClientDeviceServer } from "../lib/client-device-server.js";
  import { DEFAULT_STORAGE_PATH, parseStoragePath, cleanStoragePath, getLocalDevice, formatStoragePath, persistStoragePath, getDefaultStoragePath } from "../lib/storage-path.js";
  import { getStoredOmdKey } from "../lib/omd-key.js";
  import { DEFAULT_JUNE_SYSTEM_PROMPT, DEFAULT_JUNE_FUN_PROMPT, blobToDataUrl } from "../lib/assistant-defaults.js";

  marked.use(markedKatex({ throwOnError: false, nonStandard: true }));
  marked.setOptions({ breaks: true, gfm: true });

  const mdCache = new Map();
  function renderMd(text) {
    if (!text) return "";
    if (mdCache.has(text)) return mdCache.get(text);
    let html = "";
    try {
      html = marked.parse(text);
    } catch (e) {
      html = text;
    }
    mdCache.set(text, html);
    return html;
  }

  // Факты из бэкенда приходят как «сырой» текст, где LLM часто оставляет
  // технические артефакты: `• [From file X]: путь`, `[File: X]`, `# Заголовок`.
  // В свёрнутом аккордеоне Known facts оно выглядит сломанным (голые квадратные
  // скобки, огромные h1-блоки). Приводим к компактному markdown: файлы — чистым
  // путём без избыточного жирного форматирования, заголовки — жирным текстом вместо блоков.
  function sanitizeFactsMarkdown(text) {
    if (!text) return "";
    const out = [];
    let lastFile = "";
    for (const raw of text.split("\n")) {
      const line = raw.replace(/\r$/, "");
      let t = line.trim();
      if (!t) {
        out.push("");
        continue;
      }

      // Очищаем избыточные звёздочки и сдвоенные слэши в путях:
      // "• ****//beelink/path.jpg**: desc" -> "• /beelink/path.jpg: desc"
      // "• **/beelink/path.jpg**: desc" -> "• /beelink/path.jpg: desc"
      t = t.replace(/^(?:•\s*)?\*+\/*([^*:\n]+?)\*+:\s*(.*)/, (match, path, rest) => {
        let cleanPath = "/" + path.replace(/^\/+/, "");
        if (!isPrivateMode && !isOwnerMode) cleanPath = cleanPath.replace(/^\/[^\/]+(?=\/)/, "");
        return `• ${cleanPath}: ${rest}`;
      });
      t = t.replace(/^(?:•\s*)?\*+\/*([^*\n]+?)\*+/, (match, path) => {
        let cleanPath = "/" + path.replace(/^\/+/, "");
        if (!isPrivateMode && !isOwnerMode) cleanPath = cleanPath.replace(/^\/[^\/]+(?=\/)/, "");
        return `• ${cleanPath}`;
      });
      t = t.replace(/(?:^|(?<=[•\s]))\/{2,}(?=[a-zA-Z0-9_])/g, "/");
      t = t.replace(/^(?:•\s*)(\/[^:\n]+?)\*+:\s*(.*)/, (match, pth, rest) => {
        let cleanPath = pth;
        if (!isPrivateMode && !isOwnerMode) cleanPath = cleanPath.replace(/^\/[^\/]+(?=\/)/, "");
        return `• ${cleanPath}: ${rest}`;
      });

      // "• [File: X] (Path: /path)": путь с описанием
      let m = t.match(/^(?:•\s*)?\[(?:From file\s+|File\s*:?\s*)([^\]]+)\]\s*\(Path:\s*([^)]+)\)\s*(.*)$/i);
      if (m) {
        const name = m[1].trim();
        let pth = m[2].trim();
        if (!isPrivateMode && !isOwnerMode) pth = pth.replace(/^\/[^\/]+(?=\/)/, "");
        const rest = m[3] ? " " + m[3].trim() : "";
        out.push(`• ${pth}${rest}`);
        lastFile = name.toLowerCase();
        continue;
      }
      // "• [From file X]: /Data/Phone/X.jpg"
      m = t.match(/^(?:•\s*)?\[(?:From file\s+|File\s*:?\s*)([^\]]+)\]:\s*(\S+)\s*$/i);
      if (m) {
        const name = m[1].trim();
        let pth = m[2].trim();
        if (!isPrivateMode && !isOwnerMode) pth = pth.replace(/^\/[^\/]+(?=\/)/, "");
        out.push("• " + pth);
        lastFile = name.toLowerCase();
        continue;
      }
      // "[File: /path/to/file]"
      m = t.match(/^(?:•\s*)?\[(?:From file\s+|File\s*:?\s*)([^\]]+)\]\s*(.*)$/i);
      if (m) {
        const pth = m[1].trim();
        const rest = m[2] ? " " + m[2].trim() : "";
        if (pth.toLowerCase() === lastFile && !rest) {
          out.push("");
          lastFile = "";
          continue;
        }
        out.push("• " + pth + (rest ? ": " + rest : ""));
        lastFile = pth.toLowerCase();
        continue;
      }
      // "• # Заголовок" / "# Заголовок" — не раздувать аккордеон h1/h2-блоками.
      const boldTitle = (v) => (v.trim().length <= 90 ? "**" + v.trim() + "**" : v.trim());
      m = t.match(/^•\s*#{1,6}\s+(.+)$/);
      if (m) {
        out.push("• " + boldTitle(m[1]));
        lastFile = "";
        continue;
      }
      m = t.match(/^#{1,6}\s+(.+)$/);
      if (m) {
        out.push(boldTitle(m[1]));
        lastFile = "";
        continue;
      }
      out.push(t);
    }
    return out.join("\n");
  }

  export let chatId = "";
  export let httpDrive = null;
  export let deviceDrive = null;
  export let targetDevice = null;
  export let storageDevice = null;
  // linkId устройств пользователя (identity + поделившиеся). Позволяет показать
  // выбор любого своего устройства даже там, где isOwner не выставлен.
  export let ownLinkIds = [];
  export let targetLinkId = "";
  export let isPrivateMode = undefined;
  export let omdKey = "";
  export let chatStore = null;
  export let isDarkTheme = true;
  export let contentMode = "work"; // 'work' | 'fun'
  export let currentChatTitle = "";
  export let userName = "";
  export let isOverlayHeader = false;
  export let defaultStorage = "";
  export let thumbFetcher = null;
  export let isChatLoading = false;

  const dispatch = createEventDispatcher();

  let chat = null;
  let input = "";
  let textareaEl = null;
  let messagesContainerEl = null;
  let answeredActions = new Set();
  let showScrollTop = false;
  let showScrollBottom = false;
  let composerHeight = 100;
  let copiedId = null;
  let copiedTimer = null;
  let assistantAvatarUrl = "";
  let shouldAutoScroll = true;
  let showAttachMenu = false;
  let showLearnPicker = false;
  let learnFileInput = null;
  // Устройство, из которого берётся файл для /learn. Всегда устройство
  // пользователя (локальное или своя нода), никогда публичная AI-нода: в шаре
  // публичной ноды гостю доступен только её собственный каталог, а `/learn
  // <path>` должен указывать на файл, который реально лежит у пользователя.
  let learnSourceDevice = null;
  // /recognize: тот же выбор устройства-источника, что и для /learn.
  let recognizeSourceDevice = null;
  // Одна модалка выбора устройства на обе команды: sourceChooserMode —
  // "learn" | "recognize" ("" — закрыта).
  let sourceChooserMode = "";
  let sourceChooserSources = [];
  let lastGuestDoc = null;
  let lastGuestRecognized = null;
  let attachedDocs = [];
  let learnAbortController = null;
  // /recognize
  let showRecognizePicker = false;
  let recognizeFileInput = null;

  // Парсинг тегов из промпта: всё что начинается с # — тег
  function parseTagsFromPrompt(prompt) {
    const tagRe = /#(\w+)/g;
    const tags = [];
    let m;
    while ((m = tagRe.exec(prompt)) !== null) {
      tags.push(m[1].toLowerCase());
    }
    return tags;
  }

  // Разбор /learn: "/learn /path/to/doc #tags вопрос?" или "/learn invoice.pdf #tags вопрос?"
  // Возвращает { source, tags, question }
  function parseLearnCommand(text) {
    const rest = text.replace(/^\/learn\b/i, "").trim();
    const pathMatch = rest.match(
      /^\/(?:[^\s/#]+\/)*[^\s/#]+|^https?:\/\/[^\s]+|^[^\s/]+\.[a-z0-9]{1,6}(?=\s|$)/i
    );
    let source = "";
    let tail = rest;
    if (pathMatch) {
      source = pathMatch[0];
      tail = rest.slice(source.length).trim();
    } else {
      // Нет файла — возможно просто вопрос про тег
      source = "";
      tail = rest;
    }
    const tags = parseTagsFromPrompt(tail);
    const question = tail.replace(/#\w+/g, "").trim();
    return { source, tags, question };
  }

  // Разбор /recognize: "/recognize /path/photo.jpg #tags вопрос?" — та же схема, что /learn.
  function parseRecognizeCommand(text) {
    const rest = text.replace(/^\/recognize\b/i, "").trim();
    const pathMatch = rest.match(
      /^\/(?:[^\s/#]+\/)*[^\s/#]+|^https?:\/\/[^\s]+|^[^\s/]+\.[a-z0-9]{1,6}(?=\s|$)/i
    );
    let source = "";
    let tail = rest;
    if (pathMatch) {
      source = pathMatch[0];
      tail = rest.slice(source.length).trim();
    } else {
      source = "";
      tail = rest;
    }
    const tags = parseTagsFromPrompt(tail);
    const question = tail.replace(/#\w+/g, "").trim();
    return { source, tags, question };
  }

  // Владелец ходит в общие шары ноды (файл остаётся на ФС), гость — аплоад байтов
  $: isOwnerMode = ragScope === "all";

  // Открываем файл-пикер для /learn.
  //
  // Источник файла — всегда устройство пользователя (локальное или своя нода),
  // независимо от режима доступа к AI-ноде. Раньше гость получал системный
  // <input type="file"> и терял логический путь: карточка знаний создавалась по
  // имени файла, а не по пути в шаре, и повторный /learn не находил документ.
  function handleLearnClick() {
    showToolsMenu = false;
    if (!httpDrive) {
      chatAppend("assistant", [{ type: "text", text: "Недоступна AI-нода для импорта документов." }]);
      return;
    }
    const sources = buildDeviceSources();
    if (!sources.length) {
      // Устройства пользователя недоступны — остаётся системный выбор.
      if (learnFileInput) learnFileInput.click();
      return;
    }
    if (sources.length === 1) {
      openLearnPickerForSource(sources[0]);
      return;
    }
    openSourceChooser("learn", sources);
  }

  function openSourceChooser(mode, sources) {
    sourceChooserMode = mode;
    sourceChooserSources = sources || [];
  }

  let showChatFolderPicker = false;
  let chatStoragePickerDevice = null;
  let pendingSaveImageObj = null;

  function pickSourceDevice(device) {
    if (sourceChooserMode === "recognize") {
      openRecognizePickerForSource(device);
      return;
    }
    if (sourceChooserMode === "storage") {
      openStoragePickerForSource(device);
      return;
    }
    openLearnPickerForSource(device);
  }

  function openStoragePickerForSource(device) {
    chatStoragePickerDevice = device || getLocalDevice() || null;
    sourceChooserMode = "";
    showChatFolderPicker = true;
  }

  function handleChooseFolderForImage(imgObj) {
    pendingSaveImageObj = imgObj;
    const sources = buildDeviceSources();
    if (!sources.length) {
      const local = getLocalDevice();
      if (local) sources.push(local);
    }
    if (sources.length > 1) {
      openSourceChooser("storage", sources);
    } else {
      openStoragePickerForSource(sources[0] || getLocalDevice());
    }
  }

  async function onChatFolderSelected(e) {
    if (e.detail && (e.detail.path || e.detail.virtualPath)) {
      const chosen = e.detail.virtualPath || e.detail.path || "";
      const targetDev = chatStoragePickerDevice || getLocalDevice();
      const formatted = formatStoragePath(targetDev, chosen);
      if (chatStore && typeof chatStore.updateAssistantSettings === "function") {
        await chatStore.updateAssistantSettings({ defaultStorage: formatted });
      }
      if (!assistantSettingsData) assistantSettingsData = {};
      assistantSettingsData.defaultStorage = formatted;
      defaultStorage = formatted;
      await persistStoragePath(formatted, null, omdKey);

      if (pendingSaveImageObj) {
        await saveImageToDrive(pendingSaveImageObj);
        pendingSaveImageObj = null;
      }
    }
    showChatFolderPicker = false;
  }

  function openLearnPickerForSource(device) {
    learnSourceDevice = device || null;
    sourceChooserMode = "";
    showLearnPicker = true;
  }

  // Устройства, файлы в шарах которых пользователь может выбрать для /learn и
  // /recognize. Своими считаем: локальное устройство, узлы из identity
  // (ownLinkIds — сюда попадают и те, где флаг isOwner не выставлен, т.к. он
  // ставится лишь при admin-паре), плюс поделившиеся с пользователем.
  // Публичная AI-нода исключена: в её каталоге лежит общее содержимое ноды, а не
  // документы пользователя, и доступ к нему гостю запрещён.
  function buildDeviceSources() {
    const out = [];
    const seen = new Set();
    const own = new Set(ownLinkIds || []);
    const push = (d) => {
      if (!d || !d.drive) return;
      if (d.isAiNode === true) return;
      const id = d.linkId || d.deviceId;
      const mine = d.local === true || d.isOwner === true || own.has(id);
      if (!mine) return;
      if (!id || seen.has(id)) return;
      seen.add(id);
      out.push(d);
    };
    for (const d of deviceRegistry.list()) push(d);
    push(targetDevice);
    return out;
  }

  // Полный логический путь документа /<устройство>/<путь-к-файлу>. Именно он
  // становится document_id карточки знаний, поэтому одинаковые имена на разных
  // устройствах не сливаются в одну карточку.
  // Namespace-сегмент устройства — его префикс (hostname, напр. /beelink),
  // а НЕ внутренний linkId: getByPrefix() резолвит именно prefix/linkId/name,
  // и все document_id в unified_memory исторически начинаются с /<hostname>.
  function deviceNamespace(device) {
    const raw =
      device?.prefix ||
      device?.name ||
      device?.linkId ||
      device?.deviceId ||
      "";
    return String(raw).replace(/^\/+/, "").trim();
  }

  function withDeviceNamespace(device, path) {
    const ns = deviceNamespace(device);
    if (!ns || !path) return path || "";
    if (path.startsWith("http")) return path;
    if (path === "/" || path === "") return `/${ns}/`;
    const rel = path.startsWith("/") ? path : "/" + path;
    // Уже в своём namespace — не дублируем префикс.
    if (rel === `/${ns}` || rel.startsWith(`/${ns}/`)) return rel;
    return `/${ns}${rel}`;
  }

  function logicalDocPath(path) {
    return withDeviceNamespace(learnSourceDevice, path);
  }

  function logicalRecognizeDocPath(path) {
    return withDeviceNamespace(recognizeSourceDevice, path);
  }

  // Разбивает путь источника: если первый сегмент — известное устройство
  // (hostname-префикс/linkId/имя), путь внутри устройства — всё остальное.
  // Возвращает относительный source, драйвер и само устройство (для полного
  // логического пути, набранного вручную или вставленного из пикера).
  function splitSourcePath(path, fallbackDevice) {
    const parts = String(path || "").split("/").filter(Boolean);
    if (parts.length > 1) {
      const dev = deviceRegistry.getByPrefix(parts[0]);
      if (dev) {
        return { path: "/" + parts.slice(1).join("/"), drive: dev.drive || null, device: dev };
      }
    }
    return { path: path || "", drive: (fallbackDevice && fallbackDevice.drive) || null, device: fallbackDevice || null };
  }

  function nsDeviceFor(fallbackDevice, splitDevice) {
    if (splitDevice && splitDevice.drive && fallbackDevice && splitDevice.linkId !== fallbackDevice.linkId) {
      return splitDevice;
    }
    return fallbackDevice || splitDevice || null;
  }

  // Гость выбрал файл системным пикером: stateless-импорт + карточка прямо сейчас,
  // файл на ноде НЕ хранится (только имя). В промпт вставляем /learn <имя>.
  async function handleGuestLearnFile(e) {
    const el = e.currentTarget;
    const file = el && el.files && el.files[0];
    el.value = "";
    if (!file) return;
    try {
      activePhaseStatus = "learning";
      const data = await importUploadedFile(httpDrive, file, {
        tags: [],
        scope: "public",
        owner: userName || "anonymous",
      });
      await saveLearnCard(data, file.name, []);
      renderImportedCard(data, file.name);
      lastGuestDoc = { name: file.name, payload: data };
      input = `/learn ${file.name} `;
      await tick();
      textareaEl?.focus();
      if (textareaEl) {
        const len = textareaEl.value.length;
        textareaEl.setSelectionRange(len, len);
      }
    } catch (err) {
      console.error("[chat] /learn guest import error:", err);
      chatAppend("assistant", [{ type: "text", text: "⚠️ Ошибка импорта: " + (err.message || err) }]);
    } finally {
      activePhaseStatus = "";
    }
  }

  // Пользователь выбрал файл в пикере — вставляем /learn /<устройство>/<путь>:
  // полный логический путь, чтобы карточка и focus жили по нему.
  async function handleLearnFileSelected(e) {
    showLearnPicker = false;
    const filePath = e.detail?.path || e.detail?.id || "";
    if (filePath) {
      input = `/learn ${logicalDocPath(filePath) || filePath} `;
      await tick();
      textareaEl?.focus();
      // Поставить курсор в конец
      if (textareaEl) {
        const len = textareaEl.value.length;
        textareaEl.setSelectionRange(len, len);
      }
    }
  }

  // Импорт для /learn /path: файл уже на ноде (шapa /Documents и т.п.) —
  // нода индексирует его в ChromaDB (факты граничатся по document_id через rag_focus)
  // и возвращает payload для карточки. Файл остаётся на файловой системе.
  // Fallback: файл на устройстве → аплоад сырых байтов (/rag/import/raw).
  async function runLearnImport(path, tags, signal = null) {
    if (!httpDrive) throw new Error("AI node is not reachable (no httpDrive)");
    const scope = ragScope === "all" ? "private" : "public";
    // /learn принимает и полный логический путь (/beelink/Docs/x.pdf), и путь
    // внутри устройства: source всегда относительный, docPath — полный.
    const split = splitSourcePath(path, learnSourceDevice);
    const relSource = split.path || path;
    // Полный логический путь /<устройство>/<путь> — им карточка знаний
    // идентифицирует документ; source остаётся путём внутри устройства.
    const docPath = withDeviceNamespace(nsDeviceFor(learnSourceDevice, split.device), relSource);
    // Local-импорт требует trusted доступ к диску ноды: гость на публичной ноде
    // всегда получает 403 — не тратим первый запрос на заведомый отказ.
    if (scope !== "public") {
      try {
        const res = await httpDrive.fetch("/rag/import/local", {
          method: "POST",
          headers: { "Content-Type": "application/json", Accept: "application/json" },
          body: JSON.stringify({ source: relSource, docPath, tags: tags || [], scope }),
          ...(signal ? { signal } : {}),
        });
        let data = null;
        try { data = await res.json(); } catch (_) {}
        if (res.ok && data && data.docId) return data;
        if (res.status === 422) {
          const msg = (data && (data.detail || data.error?.message)) || "Invalid document";
          throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
        }
        console.warn(`[chat] /learn local import failed (${res.status}), falling back to device upload`);
      } catch (e) {
        if (e && e.name === "AbortError") throw e;
        if (e && e.message !== "Invalid document") {
          console.warn("[chat] /learn local import error, falling back to device upload:", e && e.message);
        } else {
          throw e;
        }
      }
    }
    // Файл лежит на устройстве пользователя — читаем оттуда, а не с публичной
    // ноды: её каталог гостю недоступен, и логический путь в шаре устройства
    // должен стать document_id карточки (как при своей ноде).
    const drive = (learnSourceDevice && learnSourceDevice.drive)
      || split.drive
      || deviceDrive
      || (targetDevice && targetDevice.drive);
    const dataPayload = await uploadDocumentForImport(httpDrive, drive, relSource, {
      tags: tags || [],
      scope,
      owner: userName || "anonymous",
      docPath,
      ...(signal ? { signal } : {}),
    });
    return dataPayload;
  }

  // ─── /recognize: распознавание изображения (та же логика, что /learn) ─────

  // Кнопка /recognize в меню инструментов: так же, как /learn — предлагаем выбор
  // любого устройства пользователя, публичная AI-нода исключена.
  function handleRecognizeClick() {
    showToolsMenu = false;
    if (!httpDrive) {
      chatAppend("assistant", [{ type: "text", text: "Недоступна AI-нода для распознавания изображений." }]);
      return;
    }
    const sources = buildDeviceSources();
    if (!sources.length) {
      if (recognizeFileInput) recognizeFileInput.click();
      return;
    }
    if (sources.length === 1) {
      openRecognizePickerForSource(sources[0]);
      return;
    }
    openSourceChooser("recognize", sources);
  }

  function openRecognizePickerForSource(device) {
    recognizeSourceDevice = device || null;
    sourceChooserMode = "";
    showRecognizePicker = true;
  }

  // Гость выбрал картинку системным пикером: stateless-распознавание прямо сейчас,
  // файл на ноде НЕ хранится (только имя). В промпт вставляем /recognize <имя>.
  async function handleGuestRecognizeFile(e) {
    const el = e.currentTarget;
    const file = el && el.files && el.files[0];
    el.value = "";
    if (!file) return;
    try {
      activePhaseStatus = "recognizing";
      const data = await recognizeUploadedImage(httpDrive, file, {
        tags: [],
        scope: isOwnerMode ? "private" : "public",
        owner: userName || "anonymous",
      });
      await saveLearnCard(data, file.name, []);
      renderImportedCard(data, file.name);
      lastGuestRecognized = { name: file.name, payload: data };
      input = `/recognize ${file.name} `;
      await tick();
      textareaEl?.focus();
      if (textareaEl) {
        const len = textareaEl.value.length;
        textareaEl.setSelectionRange(len, len);
      }
    } catch (err) {
      console.error("[chat] /recognize guest error:", err);
      chatAppend("assistant", [{ type: "text", text: "⚠️ Ошибка распознавания: " + (err.message || err) }]);
    } finally {
      activePhaseStatus = "";
    }
  }

  // Владелец выбрал картинку в пикере на ноде — вставляем /recognize /path в ввод
  async function handleRecognizeFileSelected(e) {
    showRecognizePicker = false;
    const filePath = e.detail?.path || e.detail?.id || "";
    if (filePath) {
      input = `/recognize ${logicalRecognizeDocPath(filePath) || filePath} `;
      await tick();
      textareaEl?.focus();
      if (textareaEl) {
        const len = textareaEl.value.length;
        textareaEl.setSelectionRange(len, len);
      }
    }
  }

  // Импорт/распознавание для /recognize /path: картинка на ноде →
  // /rag/recognize/local (ChromaDB + memory_card). Fallback: файл на устройстве →
  // аплоад сырых байтов (/rag/recognize/raw), stateless.
  async function runRecognizeImport(path, tags, signal = null, prompt = "") {
    if (!httpDrive) throw new Error("AI node is not reachable (no httpDrive)");
    const scope = ragScope === "all" ? "private" : "public";
    const split = splitSourcePath(path, recognizeSourceDevice);
    const relSource = split.path || path;
    const docPath = withDeviceNamespace(nsDeviceFor(recognizeSourceDevice, split.device), relSource);
    // Random доступ к диску ноды гость не имеет — local-эндпоинт всегда 403.
    if (scope !== "public") {
      try {
        const res = await httpDrive.fetch("/rag/recognize/local", {
          method: "POST",
          headers: { "Content-Type": "application/json", Accept: "application/json" },
          body: JSON.stringify({ source: relSource, docPath, tags: tags || [], scope, prompt }),
          ...(signal ? { signal } : {}),
        });
        let data = null;
        try { data = await res.json(); } catch (_) {}
        if (res.ok && data && data.docId) return data;
        if (res.status === 422) {
          const msg = (data && (data.detail || data.error?.message)) || "Invalid image";
          throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
        }
        console.warn(`[chat] /recognize local failed (${res.status}), falling back to device upload`);
      } catch (e) {
        if (e && e.name === "AbortError") throw e;
        if (e && e.message !== "Invalid image") {
          console.warn("[chat] /recognize local error, falling back to device upload:", e && e.message);
        } else {
          throw e;
        }
      }
    }
    const drive = (recognizeSourceDevice && recognizeSourceDevice.drive)
      || split.drive
      || deviceDrive
      || (targetDevice && targetDevice.drive);
    const dataPayload = await uploadImageForRecognize(httpDrive, drive, relSource, {
      tags: tags || [],
      scope,
      owner: userName || "anonymous",
      prompt,
      docPath,
      ...(signal ? { signal } : {}),
    });
    return dataPayload;
  }
  // saveAttachedDocs — fire-and-forget: GunDB ack может не прийти (WSS-реле без
  // таймаута), и блокировать им ответ на вопрос нельзя. В памяти состояние уже
  // обновлено синхронно — buildBody увидит актуальный attachedDocs сразу.
  async function attachDoc(docId, title, tags, annotation = "", imagePreview = "", isDeviceImage = false) {
    if (!docId || !chatId) return;
    const existing = attachedDocs.findIndex((d) => (d.docId || d.document_id) === docId);
    if (existing >= 0) {
      attachedDocs = attachedDocs.map((d, i) => i === existing ? { ...d, active: true, title: title || d.title, tags: tags || d.tags, annotation: annotation || d.annotation, imagePreview: imagePreview || d.imagePreview, isDeviceImage: isDeviceImage || d.isDeviceImage } : d);
      attachedDocs = [...attachedDocs];
    } else {
      attachedDocs = [...attachedDocs, {
        docId,
        title: title || docId.split("/").pop() || "Document",
        tags: tags || [],
        annotation: annotation || "",
        imagePreview: imagePreview || "",
        isImage: !!imagePreview,
        isDeviceImage,
        active: true,
      }];
    }
    saveAttachedDocs().catch((e) => console.warn("[chat] attached docs persist error:", e));
  }

  // Сохранение карточки-знания из результата импорта в GunDB
  async function saveLearnCard(data, source, tags) {
    if (!chatStore || !data) return null;
    const card = {
      memory_id: undefined,
      _id: undefined,
      title: data.title || source.split("/").pop() || "Imported document",
      text: data.annotation || "",
      document_id: source,
      tags: [...new Set([...(tags || []), ...(data.tags || [])])],
      collection: ragScope === "all" ? "owner" : "user",
      relevance: "contextual",
      docId: data.docId || data.documentId || "",
      imagePreview: data.imagePreview || "",
      created: new Date().toISOString(),
      annotation_embedding: Array.isArray(data.annotation_embedding) ? data.annotation_embedding : null,
      chunks: Array.isArray(data.chunks) ? data.chunks : [],
    };
    return await chatStore.addKnowledge(card);
  }

  // Эмбеддинг текста через /embed endpoint
  async function embedQuery(text) {
    if (!httpDrive || !text || !text.trim()) return null;
    try {
      const res = await httpDrive.fetch("/embed", {
        method: "POST",
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({ text: text.trim() }),
      });
      if (!res.ok) return null;
      const data = await res.json();
      return Array.isArray(data.embedding) ? data.embedding : null;
    } catch (e) {
      console.warn("[chat] embedQuery error:", e);
      return null;
    }
  }

  // Adaptive relevance: absolute floor + relative to the top match.
  const KNOWLEDGE_RELEVANCE_THRESHOLD = 0.25;
  const KNOWLEDGE_RELATIVE_RATIO = 0.55;
  const KNOWLEDGE_MAX_CARDS = 3;

  let embeddingBackfillBusy = false;

  async function ensureKnowledgeEmbeddings(list) {
    if (embeddingBackfillBusy || !chatStore || !httpDrive) return;
    const need = list.filter(
      (m) => !Array.isArray(m.annotation_embedding) && (m.text || "").trim()
    );
    if (need.length === 0) return;
    embeddingBackfillBusy = true;
    try {
      for (const m of need) {
        const mid = m._id || m.memory_id || m.id;
        if (!mid) continue;
        const tagPart = Array.isArray(m.tags) && m.tags.length ? " " + m.tags.join(" ") : "";
        const emb = await embedQuery(`${(m.title || "")} ${(m.text || "")}${tagPart}`.trim());
        if (emb && chatStore) {
          chatStore.updateKnowledgeEmbedding(mid, emb);
        }
      }
      console.info(`[rag] embedding backfill: ${need.length} cards updated`);
    } catch (e) {
      console.warn("[rag] embedding backfill error:", e);
    } finally {
      embeddingBackfillBusy = false;
    }
  }

  async function selectRelevantForMessage(queryText) {
    if (!chatStore || !queryText || !queryText.trim()) {
      console.info("[rag] selectRelevant: no chatStore or empty query");
      return [];
    }
    const allCards = chatStore.listKnowledge();
    if (allCards.length === 0) {
      console.info("[rag] selectRelevant: knowledge list is EMPTY (Store cache not loaded yet?)");
      return [];
    }
    const cardsWithEmb = allCards.filter((m) => Array.isArray(m.annotation_embedding) && m.annotation_embedding.length > 0);
    if (cardsWithEmb.length === 0) {
      console.info(`[rag] selectRelevant: ${allCards.length} cards, NONE has annotation_embedding`);
      return [];
    }
    const queryEmb = await embedQuery(queryText);
    if (!queryEmb) {
      console.warn("[rag] selectRelevant: embedQuery failed");
      return [];
    }
    const scored = cardsWithEmb.map((m) => ({
      card: m,
      score: cosineSimilarity(queryEmb, m.annotation_embedding),
    }));
    scored.sort((a, b) => b.score - a.score);
    const best = scored.length ? scored[0].score : 0;
    const cutoff = Math.max(KNOWLEDGE_RELEVANCE_THRESHOLD, best * KNOWLEDGE_RELATIVE_RATIO);
    const topN = scored
      .filter(({ score }) => score >= cutoff)
      .slice(0, KNOWLEDGE_MAX_CARDS);
    console.info(
      `[rag] selectRelevant: total=${allCards.length} withEmb=${cardsWithEmb.length} best=${best.toFixed(3)} cutoff=${cutoff.toFixed(3)} -> ${topN.length}`,
      topN.map((s) => `${s.card.title}:${s.score.toFixed(3)}`).join(", ")
    );
    return topN.map(({ card: m, score }) => ({
      id: m._id || m.memory_id || m.id || "",
      title: m.title || "",
      text: m.text || "",
      document_id: m.document_id || "",
      tags: Array.isArray(m.tags) ? m.tags : [],
      score,
    }));
  }

  function cosineSimilarity(a, b) {
    if (!a || !b || a.length !== b.length || a.length === 0) return 0;
    let dot = 0, normA = 0, normB = 0;
    for (let i = 0; i < a.length; i++) {
      dot += a[i] * b[i];
      normA += a[i] * a[i];
      normB += b[i] * b[i];
    }
    const denom = Math.sqrt(normA) * Math.sqrt(normB);
    return denom === 0 ? 0 : dot / denom;
  }

  // Локальный append ассистентского сообщения (для подтверждений/ошибок
  // без запроса к API). Мимо append → через state.setMessages.
  function chatAppend(role, parts) {
    if (!chat) return;
    const msg = {
      id: "local_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7),
      role,
      parts,
    };
    chat.state.setMessages([...chat.messages, msg]);
    chat = chat;
  }

  // Показать в чате локальный блок с результатом импорта (аннотация + теги)
  function renderImportedCard(data, source) {
    if (!chat || !data) return;
    const title = data.title || source.split("/").pop() || "Imported document";
    const annotation = data.annotation || "";
    const tags = [...new Set(data.tags || [])];
    chatAppend("assistant", [{
      type: "data-imported",
      data: {
        title,
        annotation,
        tags,
        docId: (data.docId || data.documentId || source || ""),
        imagePreview: data.imagePreview || "",
        isImage: !!data.imagePreview,
        isDeviceImage: false,
        attached: false,
        loading: false,
      },
    }]);
    shouldAutoScroll = true;
    scrollToBottom(true);
  }

  // Переключает карточку приаттаченного документа (in-place, без дублей).
  function isAttachedActive(docId) {
    const doc = attachedDocs.find((d) => (d.docId || d.document_id) === docId);
    return !!doc && doc.active !== false;
  }

  function markCardAttached(docId) {
    if (!chat || !chat.messages || !docId) return;
    chat.state.setMessages(chat.messages.map((m) => {
      if (m.role !== "assistant" || !m.parts?.some?.((p) => p.type === "data-imported")) return m;
      return {
        ...m,
        parts: m.parts.map((p) => {
          if (p.type !== "data-imported") return p;
          if ((p.data?.docId || "") !== docId) return p;
          return { ...p, data: { ...p.data, attached: true } };
        }),
      };
    }));
    chat = chat;
  }

  // Патчит данные карточки (data-imported) по docId без создания новой — для гостевого
  // флоу, где карточка уже отрисована при выборе файла, а /learn|/recognize-команда
  // приходит позже (навешиваем request/requestId для свёрнутого блока и rework).
  function cardSetMeta(docId, meta) {
    if (!chat || !chat.messages || !docId) return;
    chat.state.setMessages(chat.messages.map((m) => {
      if (m.role !== "assistant" || !m.parts?.some?.((p) => p.type === "data-imported")) return m;
      return {
        ...m,
        parts: m.parts.map((p) => {
          if (p.type !== "data-imported") return p;
          if ((p.data?.docId || "") !== docId) return p;
          return { ...p, data: { ...p.data, ...meta } };
        }),
      };
    }));
    chat = chat;
  }

  // ─── Attached docs (persistent document context for /learn) ──────────────

  async function loadAttachedDocs(id) {
    if (!chatStore || !id) return;
    await chatStore.ready;
    attachedDocs = chatStore.getAttachedDocs(id) || [];
  }

  async function saveAttachedDocs() {
    if (!chatStore || !chatId) return;
    await chatStore.setAttachedDocs(chatId, attachedDocs);
  }

  // Восстанавливает карточки приаттаченных документов в чате (при загрузке сессии).
  // Каждая карточка вставляется НАД юзер-промптом с /learn, который её породил
  // (иначе все карточки сваливаются в конец чата). Если промпт не найден — в конец.
  function reconstructAttachedCards() {
    if (!chat || !attachedDocs.length || !chatStore) return;
    const list = chatStore.listKnowledge();
    for (const doc of attachedDocs) {
      const docId = doc.docId || doc.document_id || "";
      if (!docId || doc.active === false) continue;
      // Ищем карточку в knowledge
      const card = list.find((c) => c.document_id === docId || c.document_id?.split("/").pop() === docId.split("/").pop());
      const title = (card?.title || doc.title || docId.split("/").pop() || "Document").trim();
      const annotation = (card?.text || doc.annotation || "").trim();
      const tags = Array.isArray(card?.tags) ? card.tags : (Array.isArray(doc.tags) ? doc.tags : []);
      const cardMsg = {
        id: "learncard_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7),
        role: "assistant",
        parts: [{
          type: "data-imported",
          data: {
            title,
            annotation,
            tags,
            docId,
            imagePreview: card?.imagePreview || doc.imagePreview || "",
            isImage: !!(card?.imagePreview || doc.imagePreview),
            isDeviceImage: doc.isDeviceImage === true || (typeof doc.isDeviceImage !== "boolean" && isDeviceImageCard(doc)),
            request: doc.request || "",
            requestId: doc.requestId || "",
            attached: true,
            loading: false,
          },
        }],
      };
      const promptIdx = findLearnPromptIndex(docId);
      if (promptIdx >= 0) {
        const pm = chat.messages[promptIdx];
        const content = (pm.parts || []).filter((p) => p.type === "text").map((p) => p.text || "").join(" ").trim();
        if (!cardMsg.parts[0].data.request && content) cardMsg.parts[0].data.request = content;
        if (!cardMsg.parts[0].data.requestId && pm.id) cardMsg.parts[0].data.requestId = pm.id;
      }
      cardMsg.parts[0].data.command = /\/recognize\b|\/detect\b/i.test(cardMsg.parts[0].data.request || "") ? "recognize" : "learn";
      insertCardAbovePrompt(cardMsg, docId);
    }
  }

  // Индекс пользовательского сообщения с /learn|/recognize <doc>, породившего карточку docId.
  function findLearnPromptIndex(docId) {
    if (!chat || !chat.messages || !docId) return -1;
    const base = String(docId).split("/").pop() || "";
    for (let i = 0; i < chat.messages.length; i++) {
      const m = chat.messages[i];
      if (!m || m.role !== "user") continue;
      const content = (m.parts || []).filter((p) => p.type === "text").map((p) => p.text || "").join(" ");
      if (!content || !/\/learn\b|\/recognize\b/i.test(content)) continue;
      let src = "";
      try {
        if (/\/learn\b/i.test(content)) src = parseLearnCommand(content).source || "";
        else if (/\/recognize\b/i.test(content)) src = parseRecognizeCommand(content).source || "";
      } catch (_) {}
      if (src) {
        const srcBase = String(src).split("/").pop() || src;
        if (src === docId || srcBase === base || (base && srcBase.toLowerCase() === base.toLowerCase())) return i;
      } else if (base && content.includes(base)) {
        return i;
      }
    }
    return -1;
  }

  function isLearnRecognizeCommand(content) {
    return /^\s*\/(learn|recognize|detect)\b/i.test(content || "");
  }

  function commandSourceOf(content) {
    try {
      if (/\/learn\b/i.test(content)) return parseLearnCommand(content).source || "";
      if (/\/recognize\b/i.test(content) || /\/detect\b/i.test(content)) return parseRecognizeCommand(content).source || "";
    } catch (_) {}
    return "";
  }

  // Является ли распознанная картинка файлом на устройстве (видимая шара владельца)?
  // Явный флаг приоритетен; для старых данных — легаси-путь: docId нормируется
  // через _norm_doc_path ведущим слэшем (/<share>/file или /file в корне дома).
  // upload:/data:/http:/image: — загружено в браузере/remote, не локальный файл.
  function isDeviceImageCard(data) {
    if (!data || !data.docId) return false;
    if (data.isDeviceImage === true) return true;
    if (data.isDeviceImage === false) return false;
    const d = String(data.docId);
    if (d.startsWith("http") || d.startsWith("image:") || d.startsWith("upload:") || d.startsWith("data:")) return false;
    // Только картинки имеют право на p2p-превью: для PDF/DOCX /p2p всегда
    // падает и рисует бессмысленный "(on another device)".
    return d.startsWith("/") && IMAGE_SRC_EXT.test(d);
  }

  function deviceDevSegment() {
    const dev = resolveP2pDevice();
    return dev
      ? ((dev.prefix ? dev.prefix.replace(/^\/+/, "") : "") || dev.linkId || dev.name || "")
      : (targetLinkId ? targetLinkId.replace(/^\/+/, "") : "");
  }

  // Сегмент устройства хранения пользователя в /p2p/...: файлы generated-папки
  // сохраняются на устройстве пользователя, поэтому и читаться должны с него,
  // а не с AI-ноды (resolveP2pDevice может указывать на неё).
  function localDeviceSegment() {
    try {
      if (storageDevice) {
        return (storageDevice.prefix ? storageDevice.prefix.replace(/^\/+/, "") : "") || storageDevice.linkId || storageDevice.name || "";
      }
      const d = (deviceDrive ? deviceRegistry.list().find((x) => x && x.drive === deviceDrive && !x.isAiNode) : null)
        || deviceRegistry.list().find((x) => x && x.local)
        || deviceRegistry.list().find((x) => x && !x.isAiNode && (x.isOwner || (ownLinkIds && ownLinkIds.includes(x.linkId))));
      return d ? ((d.prefix ? d.prefix.replace(/^\/+/, "") : "") || d.linkId || d.name || "") : "";
    } catch (_) {
      return "";
    }
  }

  // Единый URL файла с устройства: /p2p/{устройство}{путь шара} + ресайз по настройкам
  // вьювера (mode="thumb" — 360px миниатюра, "download" — скачивание, "view" — ресайз
  // под scaleTo). Используется и для сгенерированных фото, и для распознанных с устройства.
  // document_id (/beelink/Data/f.jpg) → путь внутри шары (/Data/f.jpg):
  // первый сегмент — префикс устройства, резолвим через getByPrefix.
  // Голые пути остаются без изменений.
  function docIdToSharePath(docId) {
    const raw = String(docId || "");
    if (!raw.startsWith("/")) return raw;
    const parts = raw.split("/").filter(Boolean);
    if (parts.length > 1) {
      const d = deviceRegistry.getByPrefix(parts[0]);
      if (d) return "/" + parts.slice(1).join("/");
    }
    return raw;
  }

  function deviceShareUrl(relPath, mode = "view", segmentOverride = null) {
    const devSegment = segmentOverride || deviceDevSegment();
    if (!devSegment || !relPath) return "";
    const full = `/p2p/${devSegment}` + encodeURI(relPath);
    const vs = getViewerSettings();
    if (mode === "download") return `${full}?download=true`;
    if (mode === "thumb") return vs.generateThumbs ? `${full}?resize=true&width=360&height=360` : deviceShareUrl(relPath, "view", segmentOverride);
    return vs.downscaleImages ? `${full}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}` : full;
  }

  function resolveEffectiveStorage(fallback = DEFAULT_STORAGE_PATH) {
    return (
      assistantSettingsData?.defaultStorage ||
      defaultStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
      (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
      fallback
    );
  }

  // Общая папка назначения для сгенерированных файлов на устройстве:
  // <defaultStorage>/generated, путь чистится от username/home-префиксов.
  // Единый источник и для пуша (buildDevicePush), и для отображения.
  function generatedFolder() {
    const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
    const { folder } = parseStoragePath(storage);
    let cleanStorage = folder.replace(/^\/+|\/+$/g, "");
    if (userName && (cleanStorage === userName || cleanStorage.startsWith(userName + "/"))) {
      cleanStorage = cleanStorage.slice(userName.length).replace(/^\/+/, "");
    }
    cleanStorage = cleanStorage.replace(/^(home|Users)\/[^\/]+(\/|$)/, "").replace(/^private\//i, "").replace(/^\/+|\/+$/g, "");
    return cleanStorage
      ? (cleanStorage.endsWith("generated") ? `/${cleanStorage}` : `/${cleanStorage}/generated`)
      : `${DEFAULT_STORAGE_PATH}/generated`;
  }

  // Абсолютный путь сгенерированного фото в шаре: <generatedFolder>/<path>.
  function generatedSharePath(data) {
    const path = (data?.path || "").replace(/^\/+/, "");
    if (!path) return "";
    return `${generatedFolder()}/${path}`;
  }

  // /p2p-URL сгенерированного файла: сегмент локального устройства (пуш шёл через
  // него), при его отсутствии — прежний дефолт через resolveP2pDevice.
  function generatedDeviceShareUrl(relPath, mode = "view") {
    if (!relPath) return "";
    const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
    const { device } = parseStoragePath(storage);
    const seg = device ? (device.prefix ? device.prefix.replace(/^\//, "") : device.name) : (localDeviceSegment() || null);
    return deviceShareUrl(relPath, mode, seg) || deviceShareUrl(relPath, mode);
  }

  function isStorageOnAiNode() {
    try {
      const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
      const { device } = parseStoragePath(storage);
      const targetDev = device || storageDevice || null;
      const aiNodeId = targetLinkId || targetDevice?.linkId || httpDrive?.targetLinkId || "";
      if (targetDev && aiNodeId && (targetDev.linkId === aiNodeId || targetDev.isAiNode || (targetDev.name && targetDevice?.name && targetDev.name === targetDevice.name))) {
        return true;
      }
      return false;
    } catch (_) {
      return false;
    }
  }

  // Одноразовый consent-грант на generated-папку + дескриптор для бэкенда:
  // {linkId, consentToken, folder}. Токен single-use, поэтому дескриптор
  // создаётся заново под каждый запрос (buildBody/регенерация/аватар).
  function buildDevicePush() {
    try {
      if (isStorageOnAiNode()) {
        console.log("[Chat] buildDevicePush: storage is on the AI generation node itself, skipping P2P push");
        return null;
      }
      const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
      const { device } = parseStoragePath(storage);
      const targetDev = device || storageDevice || null;
      const aiNodeId = targetLinkId || targetDevice?.linkId || httpDrive?.targetLinkId || "";

      const server = getClientDeviceServer();
      const linkId = (targetDev && !targetDev.local && targetDev.linkId) ? targetDev.linkId : server?.linkId;
      if (!linkId || (aiNodeId && linkId === aiNodeId)) {
        console.log("[Chat] buildDevicePush: target linkId is the AI node itself or unavailable, skipping P2P push");
        return null;
      }
      const folder = generatedFolder().replace(/^\/+/, "");
      if (!folder) return null;
      const grant = issueConsent(folder, { writable: true, openWindow: false });
      if (!grant?.token) return null;
      const didPub = (typeof window !== "undefined" && (window.identity?.mainPub || window.identity?.pair?.pub)) || "";
      const pushObj = { linkId, consentToken: grant.token, folder, didPub };
      console.log("[Chat] buildDevicePush created for", targetDev?.name || linkId, pushObj);
      return pushObj;
    } catch (e) {
      console.warn("[Chat] buildDevicePush error:", e);
      return null;
    }
  }

  function generatedDirectUrl(data) {
    const path = (data?.path || "").replace(/^\/+/, "");
    if (data?.url && !data.url.startsWith("/generated/")) return data.url;
    return path ? `/chat/image/${path}` : (data?.url || "");
  }

  // Отображаемый src сгенерированного фото в чате: тот же /p2p+ресайз, что и у
  // приаттаченных, либо blob/full из /chat/image/ когда устройства нет или p2p ошибся.
  function getImageSrc(data, _loadingState = null, _p2pFailed = null, _blobCache = null) {
    if (!data) return "";
    const key = data.path || data.url;
    if (!key || imgLoadingState[key] === "error") return "";
    if (imageBlobCache[key]) return imageBlobCache[key];
    if (data.path && imageBlobCache[data.path]) return imageBlobCache[data.path];
    if (data.url && imageBlobCache[data.url]) return imageBlobCache[data.url];
    const p2p = generatedDeviceShareUrl(generatedSharePath(data));
    if (p2p && !p2pImgFailed[key]) return p2p;
    if (imgLoadingState[key] !== "loading") loadImageBlob(data);
    return "";
  }

  function handleImageSrcError(data, ev = null) {
    if (!data) return;
    const key = data.path || data.url;
    if (!key) return;
    const currentSrc = ev?.target?.src || "";
    const isBlobSrc = currentSrc.startsWith("blob:") || (!currentSrc.includes("/p2p/") && imageBlobCache[key]);

    if (isBlobSrc) {
      delete imageBlobCache[key];
      if (data.path) delete imageBlobCache[data.path];
      if (data.url) delete imageBlobCache[data.url];
      imageBlobCache = { ...imageBlobCache };

      const p2p = generatedDeviceShareUrl(generatedSharePath(data));
      if (p2p && !p2pImgFailed[key]) {
        return;
      }
      imgLoadingState[key] = "error";
      if (data.path) imgLoadingState[data.path] = "error";
      if (data.url) imgLoadingState[data.url] = "error";
      imgLoadingState = { ...imgLoadingState };
    } else {
      p2pImgFailed[key] = true;
      if (data.path) p2pImgFailed[data.path] = true;
      if (data.url) p2pImgFailed[data.url] = true;
      p2pImgFailed = { ...p2pImgFailed };

      if (!imageBlobCache[key]) {
        loadImageBlob(data);
      } else {
        imgLoadingState[key] = "error";
        if (data.path) imgLoadingState[data.path] = "error";
        if (data.url) imgLoadingState[data.url] = "error";
        imgLoadingState = { ...imgLoadingState };
      }
    }
  }

  // Полный URL распознанной с устройства картинки: /p2p+ресайз либо base64-превью.
  function deviceImageSrc(data) {
    if (!data || !data.docId) return data?.imagePreview || "";
    const cleanPath = String(docIdToSharePath(data.docId)).replace(/^\/+/, "");
    if (!cleanPath) return data.imagePreview || "";
    return deviceShareUrl("/" + cleanPath) || data.imagePreview || "";
  }

  let importedImgFailed = {};

  function getImportedImageKey(data) {
    if (!data) return "";
    return data.docId || data.id || data.title || data.imagePreview || "";
  }

  function isImportedImageFailed(data, _failedMap = null) {
    if (!data) return false;
    const doc = data.docId ? String(data.docId).replace(/^\/+/, "") : "";
    return !!(
      (doc && (importedImgFailed[doc] || importedImgFailed["/" + doc])) ||
      (data.docId && importedImgFailed[data.docId]) ||
      (data.id && importedImgFailed[data.id]) ||
      (data.title && importedImgFailed[data.title]) ||
      (data.imagePreview && importedImgFailed[data.imagePreview])
    );
  }

  function markImportedImageFailed(data, ev = null) {
    if (ev && ev.target) {
      ev.target.style.display = "none";
    }
    if (!data) return;
    if (data.docId) importedImgFailed[data.docId] = true;
    if (data.id) importedImgFailed[data.id] = true;
    if (data.title) importedImgFailed[data.title] = true;
    if (data.imagePreview) importedImgFailed[data.imagePreview] = true;
    const cleanDoc = String(data.docId || "").replace(/^\/+/, "");
    if (cleanDoc) {
      importedImgFailed[cleanDoc] = true;
      importedImgFailed["/" + cleanDoc] = true;
    }
    importedImgFailed = { ...importedImgFailed };
  }

  // Id'шники юзер-cообщений с /learn|/recognize, у которых уже есть сформированная
  // карточка (data-imported) — такие пузыри в треде скрываем, т.к. оригинальный
  // запрос живёт в свёрнутом блоке внутри карточки.
  $: hiddenCommandIds = (() => {
    const set = new Set();
    if (!chat || !chat.messages) return set;
    const cardDocs = new Set();
    const cardBases = new Set();
    for (const m of chat.messages) {
      for (const p of m.parts || []) {
        if (p.type === "data-imported" && p.data?.docId) {
          const d = String(p.data.docId);
          cardDocs.add(d);
          cardBases.add(d.split("/").pop());
        }
      }
    }
    for (const m of chat.messages) {
      if (!m || m.role !== "user") continue;
      const content = (m.parts || []).filter((p) => p.type === "text").map((p) => p.text || "").join(" ").trim();
      if (!content || !isLearnRecognizeCommand(content)) continue;
      const src = commandSourceOf(content);
      if (!src) continue;
      const srcBase = String(src).split("/").pop() || src;
      if (cardDocs.has(src) || cardBases.has(srcBase)) set.add(m.id);
    }
    return set;
  })();

  // Вставляет карточку (assistant, data-imported) НАД /learn-промптом с этим docId.
  function insertCardAbovePrompt(cardMsg, docId) {
    const list = [...chat.messages];
    const idx = findLearnPromptIndex(docId);
    if (idx >= 0) list.splice(idx, 0, cardMsg);
    else list.push(cardMsg);
    chat.state.setMessages(list);
    chat = chat;
  }

  function toggleAttached(docId) {
    const idx = attachedDocs.findIndex((d) => (d.docId || d.document_id) === docId);
    if (idx < 0) return;
    attachedDocs = attachedDocs.map((d, i) => i === idx ? { ...d, active: !d.active } : d);
    attachedDocs = [...attachedDocs];
    saveAttachedDocs();
    // Обновляем данные карточки в чате (in-place)
    if (chat && chat.messages) {
      chat.state.setMessages(chat.messages.map((m) => {
        if (m.role !== "assistant" || !m.parts?.some((p) => p.type === "data-imported")) return m;
        return {
          ...m,
          parts: m.parts.map((p) => {
            if (p.type !== "data-imported") return p;
            if ((p.data?.docId || "") !== docId) return p;
            const updated = { ...p.data, attached: attachedDocs.find((d) => (d.docId || d.document_id) === docId)?.active ?? false };
            return { ...p, data: updated };
          }),
        };
      }));
    }
    if (chat) chat = chat;
  }

  // ─── Placeholder card for in-progress import ─────────────────────────────

  function appendPlaceholderCard(title, anchorMsgId = null) {
    const placeholderId = "import_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7);
    const msg = {
      id: placeholderId,
      role: "assistant",
      parts: [{ type: "data-imported", data: { title, annotation: "", tags: [], loading: true } }],
    };
    const list = [...chat.messages];
    if (anchorMsgId) {
      const anchorIdx = list.findIndex((m) => m.id === anchorMsgId);
      if (anchorIdx >= 0) list.splice(anchorIdx, 0, msg);
      else list.push(msg);
    } else {
      list.push(msg);
    }
    chat.state.setMessages(list);
    chat = chat;
    return placeholderId;
  }

  function updatePlaceholderCard(msgId, data, attached = false) {
    if (!chat || !chat.messages) return;
    chat.state.setMessages(chat.messages.map((m) => {
      if (m.id !== msgId) return m;
      return { ...m, parts: [{ type: "data-imported", data: { ...data, loading: false, attached } }] };
    }));
    chat = chat;
  }

  function removePlaceholderCard(msgId) {
    if (!chat || !chat.messages) return;
    chat.state.setMessages(chat.messages.filter((m) => m.id !== msgId));
    chat = chat;
  }

  // Полный /learn флоу при отправке
  async function handleLearnSend(text) {
    const parsed = parseLearnCommand(text);
    if (!httpDrive) {
      activePhaseStatus = "";
      isStreaming = false;
      chatAppend("assistant", [{ type: "text", text: "Недоступна AI-нода для импорта документов." }]);
      return;
    }

    // persist user message (без /learn-локального блока сверху)
    const currentId = chatId;
    const userNonce = "msg_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7);
    const nowIso = new Date().toISOString();
    chatStore
      .addMessage(currentId, {
        nonce: userNonce,
        timestamp: nowIso,
        role: "user",
        content: text,
      })
      .catch((e) => console.warn("[chat] persist user msg error:", e));

    // Показываем сам промпт /learn в треде (аргоритмически это юзер-сообщение
    // и есть «реплика, породившая карточку» — карточка встанет над ним).
    if (chat && chat.messages) {
      chat.state.setMessages([...chat.messages, { id: userNonce, role: "user", parts: [{ type: "text", text }] }]);
      chat = chat;
    }

    shouldAutoScroll = true;
    scrollToBottom(true);

    if (parsed.source) {
      activePhaseStatus = "learning";
      const isPathSource = parsed.source.startsWith("/") || parsed.source.startsWith("http");
      let placeholderId = null;
      try {
        if (isPathSource) {
          // Владелец/путь на ноде: нода индексирует файл в ChromaDB (или device-upload fallback),
          // создаём карточку, показываем аннотацию. Файл остаётся на файловой системе.
          learnAbortController = new AbortController();
          isStreaming = true;
          shouldAutoScroll = true;
          placeholderId = appendPlaceholderCard(parsed.source.split("/").pop(), userNonce);
          const data = await runLearnImport(parsed.source, parsed.tags, learnAbortController.signal);
          const refPath = data.path || data.docId || data.documentId || parsed.source;
          // Владелец: /rag/import/local уже записал аннотацию+теги в ChromaDB (memory_card),
          // карточка в «Знаниях» собирается из группы file_chunk — GunDB-карточку не плодим.
          if (!data.indexed) {
            await saveLearnCard(data, refPath, parsed.tags);
          } else {
            // Убираем устаревшие GunDB-карточки этого документа, чтобы в знаниях
            // осталась одна (ChromaDB с аннотацией), а не дубль.
            const stale = (chatStore.listKnowledge() || []).filter(
              (c) => c.document_id === refPath || c.document_id?.split("/").pop() === refPath.split("/").pop()
            );
            for (const c of stale) {
              await chatStore.deleteKnowledge(c._id || c.memory_id || c.id);
            }
          }
          updatePlaceholderCard(placeholderId, {
            title: data.title || refPath,
            annotation: data.annotation || "",
            tags: data.tags || [],
            docId: refPath,
            command: "learn",
            request: text,
            requestId: userNonce,
            attached: true,
          });
          placeholderId = null;
          // Приаттачиваем к чату: факты документа инжектятся во все последующие ответы
          await attachDoc(refPath, data.title || refPath, data.tags || [], data.annotation || "");

          if (parsed.question) {
            await sendLearnQuestion(currentId, parsed.question, refPath, data);
          } else {
            chatAppend("assistant", [
              { type: "text", text: `📚 Импортировано: **${data.title || refPath}** — карточка сохранена в знаниях (${(data.tags || []).length} тегов).` },
            ]);
          }
        } else {
          // Гость: карточка уже создана при выборе файла (файл не хранится на ноде)
          const guestData = lastGuestDoc && lastGuestDoc.name === parsed.source ? lastGuestDoc.payload : null;
          const guestDocId = (guestData && (guestData.docId || guestData.documentId)) || parsed.source;
          await attachDoc(guestDocId, guestData ? (guestData.title || parsed.source) : parsed.source, guestData ? (guestData.tags || []) : []);
          markCardAttached(guestDocId);
          cardSetMeta(guestDocId, { command: "learn", request: text, requestId: userNonce });
          if (parsed.question) {
            await sendLearnQuestion(currentId, parsed.question, guestDocId, guestData && guestData.payload ? guestData.payload : data);
          } else {
            const title = guestData ? (guestData.title || parsed.source) : parsed.source;
            chatAppend("assistant", [
              { type: "text", text: `📚 Импортировано: **${title}** — карточка сохранена в знаниях (${guestData ? (guestData.tags || []).length : 0} тегов).` },
            ]);
          }
        }
      } catch (e) {
        activePhaseStatus = "";
        isStreaming = false;
        if (placeholderId) removePlaceholderCard(placeholderId);
        const aborted = e && (e.name === "AbortError" || (learnAbortController && learnAbortController.signal.aborted));
        learnAbortController = null;
        if (aborted) {
          if (chat) chat = chat;
          return;
        }
        console.error("[chat] /learn error:", e);
        chatAppend("assistant", [{ type: "text", text: "⚠️ Ошибка импорта: " + (e.message || e) }]);
      } finally {
        activePhaseStatus = "";
        isStreaming = false;
        if (chat) chat = chat;
      }
    } else if (parsed.tags.length > 0) {
      // /learn без файла — только тег-фильтр для RAG-поиска
      activePhaseStatus = "searching";
      try {
        const query = parsed.question || text.replace(/^\/learn\b/i, "").trim();
        const res = await httpDrive.fetch("/rag/search", {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            Accept: "application/json",
          },
          body: JSON.stringify({ query, tags: parsed.tags, scope: ragScope === "all" ? "all" : "public", topK: 6 }),
        });
        let data = null;
        try {
          data = await res.json();
        } catch (_) {}
        const chunks = (data && data.chunks) || [];
        if (chunks.length === 0) {
          chatAppend("assistant", [{ type: "text", text: "По этим тегам ничего не найдено." }]);
        } else {
          const context = chunks
            .map((c) => `[#${(c.metadata?.tags || []).join(", #") || "doc"} | ${c.metadata?.title || c.metadata?.source || ""}\n${c.chunk}`)
            .join("\n\n---\n\n");
          const loadedSettings = await loadAssistantSettingsForSend();
          const qBody = {
            content_mode: contentMode,
            settings: Object.assign({}, loadedSettings || {}, {}),
          };
          const relevantKnowledge = await selectRelevantForMessage(query);
          qBody.knowledge = [
            ...relevantKnowledge,
            { title: "RAG context", text: context, tags: parsed.tags, id: "rag:" + Date.now() },
          ];
          await chat.sendMessage(
            { role: "user", id: userNonce, parts: [{ type: "text", text: query }] },
            { body: qBody }
          );
        }
      } catch (e) {
        console.error("[chat] /learn search error:", e);
        chatAppend("assistant", [{ type: "text", text: "⚠️ Ошибка поиска: " + (e.message || e) }]);
      } finally {
        activePhaseStatus = "";
        isStreaming = false;
        if (chat) chat = chat;
      }
    } else {
      // нет ни файла, ни тегов — обычная отправка
      activePhaseStatus = "";
      isStreaming = false;
      const plain = { role: "user", id: userNonce, parts: [{ type: "text", text: text.replace(/^\/learn\b/i, "").trim() || text }] };
      await chat.sendMessage(plain, {});
    }
  }

  // Полный /recognize флоу при отправке — та же логика, что /learn, но вход — картинка.
  async function handleRecognizeSend(text) {
    const parsed = parseRecognizeCommand(text);
    if (!httpDrive) {
      activePhaseStatus = "";
      isStreaming = false;
      chatAppend("assistant", [{ type: "text", text: "Недоступна AI-нода для распознавания изображений." }]);
      return;
    }

    // persist user message
    const currentId = chatId;
    const userNonce = "msg_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7);
    const nowIso = new Date().toISOString();
    chatStore
      .addMessage(currentId, {
        nonce: userNonce,
        timestamp: nowIso,
        role: "user",
        content: text,
      })
      .catch((e) => console.warn("[chat] persist user msg error:", e));

    // Показываем сам промпт /recognize в треде (карточка встанет над ним).
    if (chat && chat.messages) {
      chat.state.setMessages([...chat.messages, { id: userNonce, role: "user", parts: [{ type: "text", text }] }]);
      chat = chat;
    }

    shouldAutoScroll = true;
    scrollToBottom(true);

    if (parsed.source) {
      activePhaseStatus = "recognizing";
      const isPathSource = parsed.source.startsWith("/") || parsed.source.startsWith("http");
      let placeholderId = null;
      try {
        if (isPathSource) {
          // Владелец/путь на ноде: нода распознаёт картинку, индекс в ChromaDB,
          // создаём карточку, показываем аннотацию.
          learnAbortController = new AbortController();
          isStreaming = true;
          shouldAutoScroll = true;
          placeholderId = appendPlaceholderCard(parsed.source.split("/").pop(), userNonce);
          const data = await runRecognizeImport(parsed.source, parsed.tags, learnAbortController.signal, parsed.question);
          const refPath = data.path || data.docId || data.documentId || parsed.source;
          // Владелец: /rag/recognize/local уже записал аннотацию+теги в ChromaDB (memory_card),
          // GunDB-карточку не плодим.
          if (!data.indexed) {
            await saveLearnCard(data, refPath, parsed.tags);
          } else {
            // Убираем устаревшие GunDB-карточки этого изображения.
            const stale = (chatStore.listKnowledge() || []).filter(
              (c) => c.document_id === refPath || c.document_id?.split("/").pop() === refPath.split("/").pop()
            );
            for (const c of stale) {
              await chatStore.deleteKnowledge(c._id || c.memory_id || c.id);
            }
          }
          updatePlaceholderCard(placeholderId, {
            title: data.title || refPath,
            annotation: data.annotation || "",
            tags: data.tags || [],
            docId: refPath,
            imagePreview: data.imagePreview || "",
            isImage: !!data.imagePreview,
            isDeviceImage: !String(refPath).startsWith("http"),
            command: "recognize",
            request: text,
            requestId: userNonce,
            attached: true,
          });
          placeholderId = null;
          await attachDoc(refPath, data.title || refPath, data.tags || [], data.annotation || "", data.imagePreview || "", true);

          if (parsed.question) {
            await sendLearnQuestion(currentId, parsed.question, refPath, data);
          } else {
            chatAppend("assistant", [
              { type: "text", text: `🖼️ Распознано: **${data.title || refPath}** — карточка сохранена в знаниях (${(data.tags || []).length} тегов).` },
            ]);
          }
        } else {
          // Гость: карточка уже создана при выборе файла (stateless).
          const guestData = typeof lastGuestRecognized !== "undefined" && lastGuestRecognized && lastGuestRecognized.name === parsed.source ? lastGuestRecognized.payload : null;
          const guestDocId = (guestData && (guestData.docId || guestData.documentId)) || parsed.source;
          await attachDoc(guestDocId, guestData ? (guestData.title || parsed.source) : parsed.source, guestData ? (guestData.tags || []) : [], "", guestData ? (guestData.imagePreview || "") : "", false);
          markCardAttached(guestDocId);
          cardSetMeta(guestDocId, { command: "recognize", request: text, requestId: userNonce });
          if (parsed.question) {
            await sendLearnQuestion(currentId, parsed.question, guestDocId, guestData && guestData.payload ? guestData.payload : data);
          } else {
            const title = guestData ? (guestData.title || parsed.source) : parsed.source;
            chatAppend("assistant", [
              { type: "text", text: `🖼️ Распознано: **${title}** — карточка сохранена в знаниях (${guestData ? (guestData.tags || []).length : 0} тегов).` },
            ]);
          }
        }
      } catch (e) {
        activePhaseStatus = "";
        isStreaming = false;
        if (placeholderId) removePlaceholderCard(placeholderId);
        const aborted = e && (e.name === "AbortError" || (learnAbortController && learnAbortController.signal.aborted));
        learnAbortController = null;
        if (aborted) {
          if (chat) chat = chat;
          return;
        }
        console.error("[chat] /recognize error:", e);
        chatAppend("assistant", [{ type: "text", text: "⚠️ Ошибка распознавания: " + (e.message || e) }]);
      } finally {
        activePhaseStatus = "";
        isStreaming = false;
        if (chat) chat = chat;
      }
    } else {
      // нет файла — обычная отправка
      activePhaseStatus = "";
      isStreaming = false;
      const plain = { role: "user", id: userNonce, parts: [{ type: "text", text: text.replace(/^\/recognize\b/i, "").trim() || text }] };
      await chat.sendMessage(plain, {});
    }
  }

  // Отправка вопроса после /learn с rag_focus: бэкенд граничит факты только
  // этим путём/файлом/карточкой (document_id фильтр в ChromaDB + GunDB-карточки).
  // importPayload — результат импорта (/rag/import/*): аннотация карточки явно
  // едет в knowledge, семантический фильтр для явно импоpтedского документа
  // не валиден ("нормально ли получилось" по эмбеддингу далёко от текста).
  async function sendLearnQuestion(currentId, question, focusDocId, importPayload = null) {
    activePhaseStatus = "processing";
    shouldAutoScroll = true;
    scrollToBottom(true);
    const questionNonce = "msg_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7);
    await chatStore
      .addMessage(currentId, {
        nonce: questionNonce,
        timestamp: new Date().toISOString(),
        role: "user",
        content: question,
      })
      .catch(() => {});
    const effectiveStorage = resolveEffectiveStorage("");
    const loadedSettings = await loadAssistantSettingsForSend();
    const sendBody = {
      content_mode: contentMode,
      settings: Object.assign({}, loadedSettings || {}, effectiveStorage ? { defaultStorage: effectiveStorage } : {}),
    };
    const relevantKnowledge = await selectRelevantForMessage(question);
    let knowledge = relevantKnowledge;
    if (focusDocId) {
      let docById = (importPayload && (importPayload.annotation || importPayload.text)) ? importPayload : null;
      if (!docById) {
        //Payload импорта не передан — попробуем карточку знаний по document_id/файлу.
        const focusFn = focusDocId.split("/").pop();
        const card = (chatStore.listKnowledge() || []).find(
          (m) => (m.document_id || m.docId) === focusDocId ||
                 ((m.document_id || m.docId || "").split("/").pop() === focusFn && (m.text || m.annotation))
        );
        if (card) {
          docById = card;
        }
      }
      if (docById) {
        // Аннотация + полный текст чанков из payload импорта: для больших
        // документов аннотации мало, а чанки пришли готовыми из /rag/import/*.
        const chunkTexts = ((docById.chunks || []) || [])
          .map((c) => (c && typeof c.text === "string") ? c.text.trim() : "")
          .filter(Boolean);
        let bodyText = [docById.text || docById.annotation || "", ...chunkTexts]
          .filter(Boolean)
          .join("\n\n")
          .slice(0, 12000);
        const forceCard = {
          id: docById.docId || docById.documentId || focusDocId,
          title: docById.title || focusDocId.split("/").pop(),
          text: bodyText,
          document_id: focusDocId,
          tags: Array.isArray(docById.tags) ? docById.tags : [],
          score: 1.0,
        };
        if (forceCard.text) {
          knowledge = [forceCard, ...relevantKnowledge.filter(
            (k) => (k.id || k.document_id) !== forceCard.id && k.document_id !== focusDocId
          )];
          console.info(`[rag] /learn focus: forcing knowledge card for ${focusDocId} (${String(forceCard.text).length} chars)`);
        }
      }
    }
    if (knowledge.length > 0) {
      sendBody.knowledge = knowledge;
    }
    await chat.sendMessage(
      {
        role: "user",
        id: questionNonce,
        parts: [{ type: "text", text: question }],
      },
      {
        body: Object.assign({}, sendBody, {
          extra: { rag_focus: { docId: focusDocId, query: question } },
        }),
      }
    );
    await persistAssistantReply(currentId);
  }

  // Сохраняет ответ ассистента (текст/изображение/факты/источники) в чат,
  // вытаскивая последнее assistant-сообщение из AI SDK после завершения стрима.
  // Используется и в обычной отправке, и в sendLearnQuestion.
  async function persistAssistantReply(currentId) {
    if (!chatStore || !currentId || !chat || !chat.messages) return;
    const lastAssistant = [...chat.messages].reverse().find((m) => m.role === "assistant");
    if (!lastAssistant) return;
    const full = lastAssistant.parts
      .filter((p) => p.type === "text")
      .map((p) => p.text)
      .join("");
    const imagePart = lastAssistant.parts.find((p) => p.type === "data-image");
    const factsPart = lastAssistant.parts.find((p) => p.type === "data-facts");
    const newFactPart = lastAssistant.parts.find((p) => p.type === "data-new-fact");
    const savedFacts = (factsPart && factsPart.data?.facts) || (lastAssistant.metadata && lastAssistant.metadata.facts) || "";
    const savedNewFact = (newFactPart && newFactPart.data?.fact) || "";
    const savedSources = [];
    const seenSourceUrls = new Set();
    for (const p of lastAssistant.parts) {
      if (p.type !== "source-url") continue;
      const url = p.url || p.path || p.fullPath || p.document_id;
      if (!url || seenSourceUrls.has(url)) continue;
      seenSourceUrls.add(url);
      savedSources.push({
        url,
        title: p.title || p.path || p.url || p.fullPath,
        mimetype: p.mimetype || "",
        owner: p.owner || "",
        clickable: p.clickable ?? true,
        fullPath: p.fullPath || p.path || p.document_id || "",
        path: p.path || p.fullPath || p.document_id || "",
        document_id: p.document_id || p.fullPath || "",
        linkId: p.linkId || "",
        size: p.size || 0,
      });
    }
    if (full || imagePart || savedFacts || savedNewFact || savedSources.length) {
      const asstNonce = lastAssistant.id || ("asst_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7));
      await chatStore
        .addMessage(currentId, {
          nonce: asstNonce,
          timestamp: new Date().toISOString(),
          role: "assistant",
          content: full,
          image: imagePart ? imagePart.data : undefined,
          ...(savedFacts ? { facts: savedFacts } : {}),
          ...(savedNewFact ? { newFact: savedNewFact } : {}),
          ...(savedSources.length ? { sources: savedSources } : {}),
        })
        .catch((e) => console.warn("[chat] persist asst msg error:", e));

      chatStore.saveChat({
        _id: currentId,
        updated: new Date().toISOString(),
      }).catch(() => {});
    }
    if (imagePart && imagePart.data) {
      saveImageToDrive(imagePart.data);
    }
  }
  let showAvatarModal = false;
  let selectedKnowledgeContext = [];
  let isStreaming = false;
  let activePhaseStatus = "";
  let expandedSources = {};
  let expandedReasoning = {};
  let expandedPlan = {};
  let sourcePreviewFailed = {};
  let assistantCapabilities = { image_generation: false, chat: true, vision: false, krea2: false };
  let lgChatInstance = null;
  let copiedPromptId = null;
  let imgLoadingState = {};
  let imageBlobCache = {};
  let p2pImgFailed = {};
  let regeneratingImages = {};

  // id последнего сообщения в chat.messages на момент старта стрима.
  // Позволяет отличать «новый» ассистент-ответ текущей сессии от старых сообщений.
  let streamStartMark = "";
  $: if (isStreaming) {
    if (!streamStartMark && chat && chat.messages && chat.messages.length) {
      streamStartMark = chat.messages[chat.messages.length - 1].id;
    }
  } else {
    streamStartMark = "";
  }

  // Статусная плашка («Минутку…») показывается ТОЛЬКО на новом ответе текущего
  // хода (placeholder или стримящийся ассистент), никогда — на бывших сообщениях.
  function isNewTurnMsg(m) {
    if (m && m.id === "__pending_assistant__") return true;
    if (!m || m.role !== "assistant") return false;
    if (!chat || !chat.messages) return false;
    // Пустой стрим-марк возможен только если чат был пуст на момент старта —
    // тогда любые ассистент-сообщения принадлежат текущему ходу.
    if (!streamStartMark) return true;
    const startIdx = chat.messages.findIndex((x) => x.id === streamStartMark);
    if (startIdx < 0) return false;
    const idx = chat.messages.findIndex((x) => x.id === m.id);
    return idx > startIdx;
  }

  // Image Generation Controls & Prompt Library
  let showImageToolpanel = false;
  let paintingStyles = ["Realistic", "Fantasy", "Perfect", "Tooned"];
  let selectedStyle = "realistic";
  let withCharacters = true;
  let continueGenerating = false;
  let availableLoras = [];
  let lorasLoaded = false;
  let showPromptsPopup = false;
  let recentPrompts = [];
  let promptsSearchQuery = "";
  let historyPromptIndex = -1;
  let unsubPrompts = null;
  let unsubKnowledge = null;

  async function loadImageBlob(data) {
    const key = data?.path || data?.url;
    if (!key) return null;
    if (imageBlobCache[key]) return imageBlobCache[key];
    if (imgLoadingState[key] === "loading") return null;

    imgLoadingState[key] = "loading";
    imgLoadingState = { ...imgLoadingState };

    let rawEndpoint = data.url && data.url.startsWith("/chat/image/")
      ? data.url
      : `/chat/image/${(data.path || "").replace(/^\/+/, "")}`;
    if (!rawEndpoint.includes("size=")) {
      rawEndpoint += (rawEndpoint.includes("?") ? "&" : "?") + "size=600";
    }

    try {
      let res;
      if (httpDrive && typeof httpDrive.fetch === "function") {
        res = await httpDrive.fetch(rawEndpoint);
      } else {
        res = await fetch(rawEndpoint);
      }
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const contentType = res.headers?.get("content-type") || "";
      if (contentType && !contentType.startsWith("image/") && !contentType.startsWith("application/octet-stream")) {
        throw new Error(`Invalid content-type: ${contentType}`);
      }
      const blob = await res.blob();
      if (!blob || blob.size === 0 || (blob.type && !blob.type.startsWith("image/") && !blob.type.startsWith("application/octet-stream"))) {
        throw new Error(`Invalid image blob: type=${blob?.type}, size=${blob?.size}`);
      }
      const objectUrl = URL.createObjectURL(blob);
      imageBlobCache[key] = objectUrl;
      if (data.path) imageBlobCache[data.path] = objectUrl;
      if (data.url) imageBlobCache[data.url] = objectUrl;
      imageBlobCache = { ...imageBlobCache };
      delete p2pImgFailed[key];
      if (data.path) delete p2pImgFailed[data.path];
      if (data.url) delete p2pImgFailed[data.url];
      p2pImgFailed = { ...p2pImgFailed };
      imgLoadingState[key] = "loaded";
      if (data.path) imgLoadingState[data.path] = "loaded";
      if (data.url) imgLoadingState[data.url] = "loaded";
      imgLoadingState = { ...imgLoadingState };
      // Если хранилище на AI-ноде, она сохраняет картинку локально — не показываем предупреждение P2P
      if (isStorageOnAiNode()) {
        markImageSaved(data);
      } else {
        tempShownImages[key] = true;
        if (data.path) tempShownImages[data.path] = true;
        if (data.url) tempShownImages[data.url] = true;
        tempShownImages = { ...tempShownImages };
      }
      // Note: saveImageToDrive is intentionally NOT called here.
      // Images from chat history already exist on disk; calling it here
      // would trigger a spurious HomeMkDir RPC every time the chat is opened.
      // Saving is only done in handleSend() right after a new image is generated.
      return objectUrl;
    } catch (err) {
      console.warn("[Chat] Image API fetch failed:", err);
      const fallbackUrl = resolveImageUrl(data);
      if (fallbackUrl && fallbackUrl !== rawEndpoint) {
        try {
          const res2 = await fetch(fallbackUrl);
          const ct2 = res2.headers?.get("content-type") || "";
          if (res2.ok && (!ct2 || ct2.startsWith("image/") || ct2.startsWith("application/octet-stream"))) {
            const blob2 = await res2.blob();
            if (blob2 && blob2.size > 0 && (!blob2.type || blob2.type.startsWith("image/") || blob2.type.startsWith("application/octet-stream"))) {
              const objectUrl2 = URL.createObjectURL(blob2);
              imageBlobCache[key] = objectUrl2;
              if (data.path) imageBlobCache[data.path] = objectUrl2;
              if (data.url) imageBlobCache[data.url] = objectUrl2;
              imageBlobCache = { ...imageBlobCache };
              imgLoadingState[key] = "loaded";
              if (data.path) imgLoadingState[data.path] = "loaded";
              if (data.url) imgLoadingState[data.url] = "loaded";
              imgLoadingState = { ...imgLoadingState };
              return objectUrl2;
            }
          }
        } catch (_) {}
      }
      delete imageBlobCache[key];
      if (data.path) delete imageBlobCache[data.path];
      if (data.url) delete imageBlobCache[data.url];
      imageBlobCache = { ...imageBlobCache };
      imgLoadingState[key] = "error";
      if (data.path) imgLoadingState[data.path] = "error";
      if (data.url) imgLoadingState[data.url] = "error";
      imgLoadingState = { ...imgLoadingState };
      return null;
    }
  }

  function retryImageLoad(key, data) {
    const targetKey = key || data?.path || data?.url;
    if (!targetKey) return;
    delete imageBlobCache[targetKey];
    imageBlobCache = { ...imageBlobCache };
    if (p2pImgFailed[targetKey]) {
      delete p2pImgFailed[targetKey];
      p2pImgFailed = { ...p2pImgFailed };
    }
    imgLoadingState[targetKey] = "loading";
    imgLoadingState = { ...imgLoadingState };
    loadImageBlob(data || { path: targetKey });
  }

  async function regenerateImage(imageData, messageId) {
    if (!imageData) return;
    const oldKey = imageData.path || imageData.url;
    if (!oldKey || regeneratingImages[oldKey]) return;

    regeneratingImages[oldKey] = true;
    regeneratingImages = { ...regeneratingImages };

    try {
      const effectiveStorage = (await loadAssistantSettingsForSend())?.defaultStorage || resolveEffectiveStorage("");
      const loadedSettings = await loadAssistantSettingsForSend();
      const sendSettings = Object.assign(
        {},
        loadedSettings || {},
        effectiveStorage ? { defaultStorage: effectiveStorage } : {},
        { content_mode: contentMode }
      );

      const promptToUse = imageData.prompt || imageData.description || imageData.title || "";
      const promptId = "p_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 6);

      const payload = {
        omd_key: omdKey,
        chat: chatId,
        prompt: promptToUse,
        prompt_id: promptId,
        settings: sendSettings,
      };
      const regenPush = buildDevicePush();
      if (regenPush) payload.device_push = regenPush;

      let res;
      if (httpDrive && typeof httpDrive.fetch === "function") {
        res = await httpDrive.fetch("/generate/image/character", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
      } else {
        res = await fetch("/generate/image/character", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
      }

      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`);
      }

      const data = await res.json();
      const newFilename = data.path || data.image;
      if (!newFilename) throw new Error("No image returned");

      const newUrl = data.url || `/chat/image/${newFilename.replace(/^\/+/, "")}`;

      // Clean old entries and error states
      delete imageBlobCache[oldKey];
      delete imgLoadingState[oldKey];
      delete p2pImgFailed[oldKey];
      delete imageBlobCache[newFilename];
      delete imgLoadingState[newFilename];
      delete p2pImgFailed[newFilename];
      delete imageBlobCache[newUrl];
      delete imgLoadingState[newUrl];
      delete p2pImgFailed[newUrl];
      p2pImgFailed = { ...p2pImgFailed };
      imgLoadingState = { ...imgLoadingState };
      imageBlobCache = { ...imageBlobCache };

      imageData.path = newFilename;
      imageData.url = newUrl;
      if (data.title) imageData.title = data.title;
      if (data.description) imageData.description = data.description;

      await loadImageBlob(imageData);

      if (chat && chat.messages) {
        const msgIdx = chat.messages.findIndex((m) => m.id === messageId);
        if (msgIdx >= 0) {
          const targetMsg = chat.messages[msgIdx];
          let exactTimestamp = targetMsg.timestamp;
          if (!exactTimestamp && chatStore) {
            try {
              const cached = chatStore._getCachedMessages ? chatStore._getCachedMessages(chatId) : [];
              const found = cached.find((c) => String(c.nonce) === String(messageId) || String(c.id) === String(messageId));
              if (found && found.timestamp) exactTimestamp = found.timestamp;
            } catch (_) {}
          }
          if (!exactTimestamp) exactTimestamp = targetMsg.timestamp || new Date().toISOString();

          const newParts = (targetMsg.parts || []).map((p) => {
            if (p.type === "data-image") {
              return { ...p, data: { ...imageData } };
            }
            return p;
          });

          const updatedMsg = {
            ...targetMsg,
            parts: newParts,
            timestamp: exactTimestamp,
          };

          const newMessages = [...chat.messages];
          newMessages[msgIdx] = updatedMsg;

          if (chat.state && typeof chat.state.setMessages === "function") {
            chat.state.setMessages(newMessages);
          }

          if (chatStore && chatId) {
            const textContent = (newParts || [])
              .filter((p) => p.type === "text")
              .map((p) => p.text)
              .join("");
            chatStore.addMessage(chatId, {
              nonce: targetMsg.id,
              timestamp: exactTimestamp,
              role: "assistant",
              content: textContent,
              image: { ...imageData },
            }).catch((err) => console.warn("[chat] failed to save regenerated image to store:", err));
          }
        }
        chat = chat;
      }

      // Save regenerated image to drive if available
      saveImageToDrive(imageData).catch(() => {});
    } catch (err) {
      console.error("[Chat] Regenerate image failed:", err);
      const currKey = imageData?.path || imageData?.url || oldKey;
      imgLoadingState[currKey] = "error";
      imgLoadingState = { ...imgLoadingState };
    } finally {
      delete regeneratingImages[oldKey];
      if (imageData?.path) delete regeneratingImages[imageData.path];
      if (imageData?.url) delete regeneratingImages[imageData.url];
      regeneratingImages = { ...regeneratingImages };
    }
  }

  $: displayMessages = (() => {
    if (!chat || !chat.messages) return [];
    const list = [...chat.messages];
    // Плейсхолдер ответа добавляем только когда реплика юзера уже в списке
    // (иначе на старте стрима он оказывается над юзер-сообщением, под старым ответом)
    if (isStreaming && (list.length === 0 || list[list.length - 1].role === "user")) {
      list.push({
        id: "__pending_assistant__",
        role: "assistant",
        parts: [],
        isPending: true,
      });
    }
    return list;
  })();

  $: if (displayMessages) {
    for (const m of displayMessages) {
      for (const p of m.parts || []) {
        if (p.type === "data-image" && p.data) {
          const k = p.data.path || p.data.url;
          if (k && !imageBlobCache[k] && imgLoadingState[k] !== "loading" && imgLoadingState[k] !== "error") {
            loadImageBlob(p.data);
          }
        }
      }
    }
  }
  $: if (displayMessages) {
    for (const m of displayMessages) {
      for (const p of m.parts || []) {
        if (p.type === "data-image" && p.data) {
          const k = p.data.path || p.data.url;
          if (k && !imageBlobCache[k] && imgLoadingState[k] !== "loading" && imgLoadingState[k] !== "error") {
            loadImageBlob(p.data);
          }
        }
      }
    }
  }

  // Backend-generated chat titles: "newchat" (new chat) and "rename" events
  // (mid-conversation) arrive as data-chatinfo / data-rename parts. Without
  // this the UI kept the user's first phrase as the chat title forever.
  let lastAppliedServerTitle = "";
  function safeJsonParseValue(v) {
    if (typeof v !== "string") return v;
    try { return JSON.parse(v); } catch (_) { return null; }
  }
  $: if (displayMessages && chatStore && chatId) {
    for (const m of displayMessages) {
      for (const p of m.parts || []) {
        let serverTitle = "";
        if (p.type === "data-chatinfo" && p.data) {
          const info = typeof p.data === "string" ? safeJsonParseValue(p.data) : p.data;
          // capture the backend chat name for subsequent requests, but never
          // use the chat id as a display title (ensure_chat falls back to name)
          if (info?.name && info?.name !== chatId) {
            const rec = chatStore.listChats().find((c) => c._id === chatId);
            if (rec && rec.server_name !== info.name) {
              chatStore.saveChat({ _id: chatId, server_name: info.name }).catch(() => {});
            }
          }
          const t = (info?.title || "").toString().trim();
          serverTitle = (t && t !== info.name && t !== chatId) ? t : "";
        } else if (p.type === "data-rename" && p.data && typeof p.data.title === "string") {
          serverTitle = p.data.title.trim();
        }
        const metaSummary = (m.metadata && typeof m.metadata.summary === "string") ? m.metadata.summary : "";
        if (metaSummary) {
          const rec = chatStore.listChats().find((c) => c._id === chatId);
          if (rec && rec.summary !== metaSummary) {
            chatStore.saveChat({ _id: chatId, summary: metaSummary }).catch(() => {});
          }
        }
        if (serverTitle && serverTitle !== lastAppliedServerTitle) {
          lastAppliedServerTitle = serverTitle;
          chatStore
            .saveChat({ _id: chatId, title: serverTitle, name: serverTitle.slice(0, 60), updated: new Date().toISOString() })
            .then(() => dispatch("title-change", { chatId, title: serverTitle }))
            .catch(() => {});
        }
      }
    }
  }

  function extractPlanSteps(text) {
    if (!text || typeof text !== "string") return [];
    const lines = text.split("\n");
    const steps = [];
    let currentStep = null;
    let hadBlankLine = false;

    const headerRegex = /^\s*\[?(?:PLAN|CHECKLIST|ПЛАН|ШАГИ)\]?:?\s*$/i;
    const stepStartRegex = /^\s*(?:[-*•]\s*)?(?:(?:Step|Шаг)\s*\d+[:.\s-]+|\d+\.\s+|\[[ xX]\]\s*)(.+)/i;

    for (let rawLine of lines) {
      const line = rawLine.trim();
      if (!line) {
        if (currentStep) hadBlankLine = true;
        continue;
      }
      if (headerRegex.test(line)) continue;
      if (/^\[?\/?(PLAN|THOUGHT|CHECKLIST)\]?/i.test(line)) continue;
      if (/^(Thinking Process|Thought Process|Thinking|Ход мыслей|Размышления):?/i.test(line)) continue;

      const match = line.match(stepStartRegex);
      if (match) {
        if (currentStep) {
          steps.push(currentStep.trim());
        }
        currentStep = match[1].trim();
        hadBlankLine = false;
      } else if (currentStep) {
        if (hadBlankLine) {
          steps.push(currentStep.trim());
          currentStep = null;
          break;
        } else if (/^#{1,4}\s+/.test(line) || /^---+$/.test(line) || /^\[?[A-Z_]+\]?:/.test(line)) {
          steps.push(currentStep.trim());
          currentStep = null;
          break;
        } else {
          currentStep += " " + line;
        }
      }
    }
    if (currentStep) {
      steps.push(currentStep.trim());
    }

    return steps;
  }

  function buildBlocks(msg) {
    const blocks = [];
    let md = "";
    let reasoning = "";
    let statusState = "";
    let planData = null;
    const sources = [];

    const flushMd = () => {
      if (md.trim()) blocks.push({ kind: "md", text: md });
      md = "";
    };

    for (const part of msg.parts || []) {
      if (part.type === "text") {
        md += part.text;
      } else if (part.type === "reasoning") {
        reasoning += part.text || "";
      } else if (part.type === "data-plan") {
        planData = part.data || planData;
      } else if (part.type === "source-url") {
        // Дедуп источников по url (некоторые пайплайны могли прислать их дважды)
        if (!sources.some((s) => s.url === part.url)) {
          sources.push(part);
        }
      } else if (part.type === "data-status") {
        statusState = part.data?.state || statusState;
      } else if (part.type === "data-question") {
        flushMd();
        blocks.push({ kind: "question", data: part.data });
      } else if (part.type === "data-permission") {
        flushMd();
        blocks.push({ kind: "permission", data: part.data });
      } else if (part.type === "data-knowledge-update") {
        flushMd();
        blocks.push({ kind: "knowledge", data: part.data });
      } else if (part.type === "data-facts") {
        flushMd();
        blocks.push({ kind: "facts", facts: part.data?.facts || "" });
      } else if (part.type === "data-new-fact") {
        flushMd();
        blocks.push({ kind: "new_fact", fact: part.data?.fact || "" });
      } else if (part.type === "data-image") {
        flushMd();
        blocks.push({ kind: "image", data: part.data });
      } else if (part.type === "data-imported") {
        flushMd();
        blocks.push({ kind: "imported", data: part.data });
      }
    }
    flushMd();

    const isLastMessage = displayMessages.length > 0 && displayMessages[displayMessages.length - 1].id === msg.id;
    const isMsgStreaming = isStreaming && isLastMessage && isNewTurnMsg(msg);
    const hasFinalText = md.trim().length > 0;

    let cleanReasoning = "";
    if (reasoning.trim()) {
      cleanReasoning = reasoning
        .replace(/^(\s*(\*{1,2}|#{1,3})?\s*(Thinking Process|Thought Process|Thinking|Ход мыслей|Размышления)[:\s]*(\*{1,2})?\s*\n*)/i, "")
        .replace(/\n{3,}/g, "\n\n")
        .trim();
    }

    const extractedSteps = extractPlanSteps(cleanReasoning);
    const hasExplicitPlan = !!(planData && Array.isArray(planData.todos) && planData.todos.length > 0);
    const hasExtractedSteps = extractedSteps.length > 0 && (
      extractedSteps.length >= 2 || /^(?:Step|Шаг|\[[ xX]\])/i.test(cleanReasoning)
    );

    if (hasExplicitPlan || hasExtractedSteps) {
      let steps = [];
      const isPlanRunning = isMsgStreaming && !hasFinalText && planData?.status !== "success";

      if (hasExplicitPlan) {
        let firstPendingFound = false;
        steps = planData.todos.map((t, idx) => {
          let st = t.status || "pending";
          if (planData.status === "error") {
            st = t.status || "pending";
          } else if (planData.status === "success") {
            st = "completed";
          } else if (!isMsgStreaming || hasFinalText) {
            st = (t.status === "completed" || t.status === "running") ? "completed" : "pending";
          } else if (isPlanRunning) {
            if (st === "pending" && !firstPendingFound) {
              st = "running";
              firstPendingFound = true;
            }
          }
          return {
            content: t.content || t.title || `Шаг ${idx + 1}`,
            status: st
          };
        });
      } else {
        steps = extractedSteps.map((content, idx) => {
          let st = "completed";
          if (isPlanRunning) {
            st = (idx === 0) ? "running" : "pending";
          }
          return { content, status: st };
        });
      }

      blocks.unshift({
        kind: "plan",
        steps,
        isExplicit: hasExplicitPlan,
        isRunning: isPlanRunning,
        rawText: cleanReasoning
      });
    } else if (cleanReasoning) {
      blocks.unshift({ kind: "reasoning", text: cleanReasoning });
    }

    const effectiveStatus = (isStreaming && isLastMessage && isNewTurnMsg(msg) && activePhaseStatus) ? activePhaseStatus : statusState;

    // Do not push status into blocks here.
    // The bottom badge will handle displaying the status if md, plan and reasoning are empty.
    
    return blocks;
  }

  let currentSessionLoadId = 0;
  let chatLoadingTimeout = null;

  $: if (isChatLoading) {
    if (chatLoadingTimeout) clearTimeout(chatLoadingTimeout);
    chatLoadingTimeout = setTimeout(() => {
      isChatLoading = false;
    }, 12000);
  } else if (chatLoadingTimeout) {
    clearTimeout(chatLoadingTimeout);
    chatLoadingTimeout = null;
  }

  async function initChatSession(id) {
    if (!id || !chatStore || !httpDrive) {
      isChatLoading = false;
      return;
    }
    const sessionLoadId = ++currentSessionLoadId;
    isChatLoading = true;

    try {
      await chatStore.ready;
      if (sessionLoadId !== currentSessionLoadId) return;

      isStreaming = false;
      activePhaseStatus = "";

      // Загружаем приаттаченные документы (персистятся в записи чата)
      await loadAttachedDocs(id);
      if (sessionLoadId !== currentSessionLoadId) return;

      const history = await chatStore.getMessages(id);
      if (sessionLoadId !== currentSessionLoadId) return;

      if (chatStore) {
        if (unsubPrompts) unsubPrompts();
        if (typeof chatStore.onPromptsChanged === "function") {
          unsubPrompts = chatStore.onPromptsChanged((list) => {
            recentPrompts = list || [];
          });
        }
        // Клиентская инъекция фактов из карточек памяти (RAG 3.0)
        if (typeof chatStore.onKnowledgeChanged === "function") {
          if (unsubKnowledge) unsubKnowledge();
          unsubKnowledge = chatStore.onKnowledgeChanged((list) => {
            selectedKnowledgeContext = (list || []).map((m) => ({
              id: m._id || m.memory_id || m.id || "",
              title: m.title || "",
              text: m.text || "",
              tags: Array.isArray(m.tags) ? m.tags : [],
            }));
            // Самолечка: старые карточки без annotation_embedding добираем
            // через /embed и записываем в GunDB.
            ensureKnowledgeEmbeddings(list || []);
          });
        }
      }
      const transport = new OnmyChatTransport({
        httpDrive,
        chatId: id,
        getOmdKey: () => omdKey,
        // Context compaction: reuse the last backend summary so the AI node
        // receives summary + short tail instead of the full message history.
        buildBody: async () => {
          try {
            const rec = chatStore.listChats().find((c) => c._id === id);
            const summary = rec && typeof rec.summary === "string" ? rec.summary : "";
            const total = chat.messages ? chat.messages.length : 0;
            const serverName = rec && typeof rec.server_name === "string" ? rec.server_name : "";
            const hasTitle = rec && !!rec.title;
            const out = { chat_summary: summary || null, total_message_count: total || null };
            if ($ttsState.voiceStreamEnabled) {
              out.voice_mode = true;
            }
            if (serverName) out.chat = serverName;
            else if (!hasTitle) out.chat = "newchat";
            if (attachedDocs.length > 0) {
              out.attached_docs = attachedDocs
                .filter((d) => d.active !== false)
                .map((d) => ({ docId: d.docId || d.document_id || "", title: d.title || "" }));
            }
            // Авто-пуш сгенерированного фото на устройство: одноразовый
            // consent-грант под этот запрос (см. buildDevicePush).
            const devicePush = buildDevicePush();
            if (devicePush) out.device_push = devicePush;

            // MCP application keys (Plane, Grade.app, etc.) from DiD graph
            if (typeof window !== "undefined" && typeof window.__omdGetMcpPayload === "function") {
              try {
                const mcpPayload = window.__omdGetMcpPayload();
                if (mcpPayload && Object.keys(mcpPayload).length > 0) {
                  out.mcp_keys = mcpPayload;
                }
              } catch (_) {}
            }
            return out;
          } catch (_) { return {}; }
        },
        onStatus: (st) => {
          activePhaseStatus = st;
          if (st) {
            isStreaming = true;
            shouldAutoScroll = true;
            scrollToBottom(true);
          } else {
            isStreaming = false;
          }
          if (chat) chat = chat;
        },
        onUpdate: () => {
          if (chat) chat = chat;
          if (shouldAutoScroll) scrollToBottom(true);
        },
        onChunk: (chunk) => {
          if (chunk.type === "data-image" && chunk.data) {
            saveImageToDrive(chunk.data).catch((e) => console.warn("[Chat] onChunk saveImageToDrive error:", e));
          }
          if ($ttsState.voiceStreamEnabled && chunk.type === "text-delta" && chunk.delta) {
            ttsPlayer.enqueueStreamDelta(chunk.delta);
          }
        },
        onFinish: () => {
          if ($ttsState.voiceStreamEnabled) {
            ttsPlayer.endStreamingSession();
          }
        },
      });

      chat = new Chat({ id, transport });

      if (history && history.length) {
        chat.state.setMessages(
          history.map((m) => {
            const parts = [
              {
                type: "text",
                text: typeof m.content === "string" ? m.content : JSON.stringify(m.content || ""),
              },
            ];
            if (m.facts) {
              parts.unshift({ type: "data-facts", data: { facts: m.facts } });
            }
            if (m.newFact) {
              parts.unshift({ type: "data-new-fact", data: { fact: m.newFact } });
            }
            if (Array.isArray(m.sources) && m.sources.length) {
              const seenSourceUrls = new Set();
              for (const s of m.sources) {
                if (!s) continue;
                const isStr = typeof s === "string";
                let rawVal = isStr ? s : (s.fullPath || s.path || s.document_id || s.url || "");
                if (rawVal.startsWith("http://") || rawVal.startsWith("https://")) {
                  try {
                    const u = new URL(rawVal);
                    rawVal = decodeURIComponent(u.pathname);
                  } catch (_) {
                    rawVal = rawVal.replace(/^https?:\/\/[^\/]+/, "");
                  }
                }
                const url = isStr ? rawVal : (s.url && !s.url.includes("onmydisk.net") ? s.url : rawVal);
                if (seenSourceUrls.has(url || rawVal)) continue;
                seenSourceUrls.add(url || rawVal);
                const fullPath = isStr ? rawVal : (s.fullPath || s.path || s.document_id || rawVal);
                const path = isStr ? rawVal : (s.path || s.fullPath || s.document_id || rawVal);
                const title = isStr ? rawVal.split("/").pop() : (s.title || path.split("/").pop() || rawVal);
                parts.push({
                  type: "source-url",
                  sourceId: isStr ? rawVal : (s.path || s.url || s.fullPath || Math.random().toString()),
                  url,
                  title,
                  mimetype: isStr ? "" : s.mimetype,
                  owner: isStr ? "" : s.owner,
                  fullPath,
                  path,
                  document_id: isStr ? rawVal : (s.document_id || fullPath || path),
                  linkId: isStr ? "" : (s.linkId || ""),
                  size: isStr ? 0 : (s.size || 0),
                });
              }
            }
            if (m.image) {
              parts.push({ type: "data-image", data: m.image });
            }
            return {
              id: String(m.nonce || m.timestamp || Math.random().toString(36).slice(2)),
              nonce: m.nonce ? String(m.nonce) : null,
              timestamp: m.timestamp || null,
              role: m.role === "assistant" ? "assistant" : "user",
              parts,
            };
          })
        );

        // Восстанавливаем карточки приаттаченных документов
        reconstructAttachedCards();
      }

      await tick();
      scrollToBottom(false);
    } catch (err) {
      console.warn("[chat] initChatSession error:", err);
    } finally {
      if (sessionLoadId === currentSessionLoadId) {
        isChatLoading = false;
      }
    }
  }

  let assistantName = "June";
  let assistantTitle = "Junior assistant";
  let assistantUnsub = null;

  let lastSessionChatId = null;
  $: if (chatId && chatId !== lastSessionChatId) {
    if (!chatStore || !httpDrive) {
      isChatLoading = true;
    }
  }
  $: if (chatId && chatStore && httpDrive && chatId !== lastSessionChatId) {
    lastSessionChatId = chatId;
    initChatSession(chatId);
  }

  let assistantSettingsData = null;
  let lastAssistantLoadedKey = "";
  let lastAssistantDriveId = "";
  $: if (chatStore || httpDrive) {
    const key = (chatStore && chatStore.tokenHash) || "";
    const driveId = httpDrive?.targetLinkId || "";
    if (!assistantSettingsData || (key && key !== lastAssistantLoadedKey) || (driveId && driveId !== lastAssistantDriveId) || (!assistantAvatarUrl && (httpDrive || !lastAssistantLoadedKey))) {
      lastAssistantLoadedKey = key;
      lastAssistantDriveId = driveId;
      loadAssistantInfo();
    }
  }

  async function loadAssistantInfo() {
    // 1. Check user-configured assistant settings in GunDB
    if (chatStore) {
      try {
        const settings = await chatStore.getAssistantSettings();
        if (settings) {
          // Имя для контекста: сохранённое в GunDB, либо из DID-профиля (полное → короткое)
          assistantSettingsData = { ...settings, name: resolveChatUserName($profileStore, settings.name) };
          if (settings.assistant_name) assistantName = settings.assistant_name;
          if (settings.title) assistantTitle = settings.title;
          if (settings.assistant_avatar && typeof settings.assistant_avatar === "string") {
            const rawAvatar = settings.assistant_avatar.trim();
            if (rawAvatar.startsWith("blob:") || rawAvatar.includes("blob:")) {
              assistantAvatarUrl = "";
              if (chatStore && chatStore._writeSettingsLocal) {
                chatStore._writeSettingsLocal({ ...settings, assistant_avatar: "" });
              }
            } else if (rawAvatar.startsWith("data:image/")) {
              assistantAvatarUrl = rawAvatar;
            } else if (rawAvatar.length > 50 && /^[A-Za-z0-9+/=]+$/.test(rawAvatar.replace(/\s+/g, ""))) {
              assistantAvatarUrl = "data:image/jpeg;base64," + rawAvatar;
            } else {
              assistantAvatarUrl = "";
            }
          }
        }
      } catch (e) {
        console.warn("[chat] error fetching GunDB assistant settings:", e);
      }

      if (!assistantUnsub && chatStore.subscribeAssistantSettings) {
        assistantUnsub = chatStore.subscribeAssistantSettings((settings) => {
          if (!settings) return;
          assistantSettingsData = { ...settings, name: resolveChatUserName($profileStore, settings.name) };
          if (settings.assistant_name) assistantName = settings.assistant_name;
          if (settings.title) assistantTitle = settings.title;
          if (settings.assistant_avatar && typeof settings.assistant_avatar === "string") {
            const rawAvatar = settings.assistant_avatar.trim();
            if (rawAvatar.startsWith("blob:") || rawAvatar.includes("blob:")) {
              assistantAvatarUrl = "";
              if (chatStore && chatStore._writeSettingsLocal) {
                chatStore._writeSettingsLocal({ ...settings, assistant_avatar: "" });
              }
            } else if (rawAvatar.startsWith("data:image/")) {
              assistantAvatarUrl = rawAvatar;
            } else if (rawAvatar.length > 50 && /^[A-Za-z0-9+/=]+$/.test(rawAvatar.replace(/\s+/g, ""))) {
              assistantAvatarUrl = "data:image/jpeg;base64," + rawAvatar;
            } else {
              assistantAvatarUrl = "";
            }
          }
        });
      }
    }

    // Имя для контекста: даже если настроек в GunDB ещё нет — берём из DID-профиля
    const resolvedName = resolveChatUserName($profileStore, assistantSettingsData?.name || userName);
    if (resolvedName) {
      assistantSettingsData = { ...(assistantSettingsData || {}), name: resolvedName };
    }

    // 2. Query inference node via httpDrive /assistant, or same-origin fallback
    let info = null;
    if (httpDrive) {
      try {
        const res = await httpDrive.fetch("/assistant");
        if (res.ok) {
          const data = await res.json();
          info = data.result || data;
        }
      } catch (_) {}
    }
    if (!info && typeof window !== "undefined") {
      try {
        const res = await window.fetch("/assistant");
        if (res.ok) {
          const data = await res.json();
          info = data.result || data;
        }
      } catch (_) {}
    }

    if (info) {
      if (info.assistant_name && (!assistantName || assistantName === "June")) {
        assistantName = info.assistant_name;
      }
      if (info.title && (!assistantTitle || assistantTitle === "Junior assistant")) {
        assistantTitle = info.title;
      }
      if (info.capabilities) {
        assistantCapabilities = {
          ...assistantCapabilities,
          ...info.capabilities,
        };
      }
    }

    const defaultPrompt = (info && info.system_prompt) || DEFAULT_JUNE_SYSTEM_PROMPT;
    const defaultFun = (info && info.fun_system_prompt) || DEFAULT_JUNE_FUN_PROMPT;
    if (!assistantSettingsData?.system_prompt) {
      assistantSettingsData = {
        ...(assistantSettingsData || {}),
        system_prompt: defaultPrompt,
        fun_system_prompt: assistantSettingsData?.fun_system_prompt || defaultFun,
        assistant_name: assistantName,
        title: assistantTitle,
      };
      if (chatStore && typeof chatStore.updateAssistantSettings === "function") {
        chatStore.updateAssistantSettings({
          system_prompt: defaultPrompt,
          fun_system_prompt: defaultFun,
          assistant_name: assistantName,
          title: assistantTitle,
        }).catch(() => {});
      }
    }

    // Load avatar blob if not already set from custom base64
    if (!assistantAvatarUrl) {
      let blob = null;
      if (httpDrive) {
        try {
          const res = await httpDrive.fetch("/assistant/avatar?size=160");
          if (res.ok) blob = await res.blob();
        } catch (_) {}
      }
      if ((!blob || blob.size === 0) && typeof window !== "undefined") {
        try {
          const res = await window.fetch("/assistant/avatar?size=160");
          if (res.ok) blob = await res.blob();
        } catch (_) {}
      }
      if ((!blob || blob.size === 0) && typeof window !== "undefined") {
        try {
          const res = await window.fetch("/img/june.png");
          if (res.ok) blob = await res.blob();
        } catch (_) {}
      }
      if (blob && blob.size > 0) {
        if (assistantAvatarUrl && assistantAvatarUrl.startsWith("blob:")) {
          try { URL.revokeObjectURL(assistantAvatarUrl); } catch (_) {}
        }
        assistantAvatarUrl = URL.createObjectURL(blob);
        const dataUrl = await blobToDataUrl(blob);
        if (dataUrl) {
          assistantSettingsData = {
            ...(assistantSettingsData || {}),
            assistant_avatar: dataUrl,
          };
          if (chatStore && typeof chatStore.updateAssistantSettings === "function") {
            chatStore.updateAssistantSettings({ assistant_avatar: dataUrl, avatar_source: "node" }).catch(() => {});
          }
        }
      }
    }
  }

  let avatarErrorOccurred = false;
  async function handleAvatarError() {
    if (avatarErrorOccurred) {
      assistantAvatarUrl = "";
      return;
    }
    avatarErrorOccurred = true;
    const oldUrl = assistantAvatarUrl;
    assistantAvatarUrl = "";
    if (oldUrl && oldUrl.startsWith("blob:")) {
      try { URL.revokeObjectURL(oldUrl); } catch (_) {}
    }
    if (httpDrive) {
      try {
        const res = await httpDrive.fetch("/assistant/avatar?size=160");
        if (res.ok) {
          const blob = await res.blob();
          if (blob && blob.size > 0) {
            assistantAvatarUrl = URL.createObjectURL(blob);
          }
        }
      } catch (_) {}
    }
  }

  // Загрузка настроек персоны к моменту отправки:
  // GunDB-чтение может прийти позже onMount (или отвалиться по таймауту),
  // поэтому перед сборкой sendBody дожимаем getAssistantSettings —
  // внутри уже ретраи и fallback на localStorage-зеркало (chat-store.js).
  async function loadAssistantSettingsForSend() {
    if (assistantSettingsData && (assistantSettingsData.fun_system_prompt || assistantSettingsData.system_prompt)) {
      return assistantSettingsData;
    }
    if (chatStore && typeof chatStore.getAssistantSettings === "function") {
      try {
        const settings = await chatStore.getAssistantSettings();
        if (settings) {
          assistantSettingsData = { ...settings, name: resolveChatUserName($profileStore, settings.name) };
          if (settings.assistant_name) assistantName = settings.assistant_name;
          if (settings.title) assistantTitle = settings.title;
        }
      } catch (e) {
        console.warn("[chat] send-time assistant settings load failed:", e);
      }
    }
    const resolvedName = resolveChatUserName($profileStore, assistantSettingsData?.name || userName);
    assistantSettingsData = {
      ...(assistantSettingsData || {}),
      language: $locale || "en",
      ...(resolvedName ? { name: resolvedName } : {})
    };
    return assistantSettingsData;
  }

  let mutationObserver = null;

  function setupScrollObserver(node) {
    if (typeof MutationObserver === "undefined" || !node) return;
    if (mutationObserver) mutationObserver.disconnect();
    mutationObserver = new MutationObserver(() => {
      if (shouldAutoScroll && messagesContainerEl) {
        messagesContainerEl.scrollTop = messagesContainerEl.scrollHeight;
      }
    });
    mutationObserver.observe(node, {
      childList: true,
      subtree: true,
      characterData: true,
    });
  }

  onMount(() => {
    loadAssistantInfo();
    resizeTextarea();
    if (typeof localStorage !== "undefined") {
      continueGenerating = localStorage.getItem("omc_continue_generating") === "true";
      const savedWithChars = localStorage.getItem("omc_gen_characters");
      withCharacters = savedWithChars !== null ? savedWithChars === "true" : true;
    }
    if (messagesContainerEl) {
      setupScrollObserver(messagesContainerEl);
      handleScroll();
    }
  });

  onDestroy(() => {
    ttsPlayer.stop();
    if (unsubPrompts) {
      unsubPrompts();
      unsubPrompts = null;
    }
    if (mutationObserver) {
      mutationObserver.disconnect();
      mutationObserver = null;
    }
    if (assistantUnsub) assistantUnsub();
    if (assistantAvatarUrl && assistantAvatarUrl.startsWith("blob:")) {
      URL.revokeObjectURL(assistantAvatarUrl);
    }
    if (lgChatInstance) {
      try {
        lgChatInstance.destroy(true);
      } catch (_) {}
      lgChatInstance = null;
    }
    if (copiedTimer) clearTimeout(copiedTimer);
    isChatLoading = false;
    if (chatLoadingTimeout) {
      clearTimeout(chatLoadingTimeout);
      chatLoadingTimeout = null;
    }
  });

  function handleScroll() {
    if (!messagesContainerEl) return;
    const { scrollTop, scrollHeight, clientHeight } = messagesContainerEl;
    const distanceToBottom = scrollHeight - (scrollTop + clientHeight);
    shouldAutoScroll = distanceToBottom < 100;
    showScrollTop = scrollTop > 160;
    showScrollBottom = distanceToBottom > 160;
  }

  function scrollToTop(smooth = true) {
    if (messagesContainerEl) {
      messagesContainerEl.scrollTo({ top: 0, behavior: smooth ? "smooth" : "auto" });
      setTimeout(() => handleScroll(), 300);
    }
  }

  async function scrollToBottom(smooth = false) {
    await tick();
    if (messagesContainerEl) {
      if (smooth) {
        messagesContainerEl.scrollTo({
          top: messagesContainerEl.scrollHeight,
          behavior: "smooth",
        });
        setTimeout(() => handleScroll(), 300);
      } else {
        messagesContainerEl.scrollTop = messagesContainerEl.scrollHeight;
      }
    }
  }

function lastFreshTitle(messages) {
    for (let i = messages.length - 1; i >= 0; i--) {
      const m = messages[i];
        if (m && m.role === "assistant" && m.metadata) {
        if (typeof m.metadata.newTitle === "string" && m.metadata.newTitle) return m.metadata.newTitle;
        const ci = m.metadata.chatinfo;
        if (ci && typeof ci === "object") {
          // ignore id-fallback titles (ensure_chat returns title === name)
          const t = (ci.title || "").toString();
          const nm = (ci.name || "").toString();
          if (t && t !== nm) return t;
        }
      }
    }
    return "";
  }
  $: lastTitleUpdate = chat?.messages?.length ? lastFreshTitle(chat.messages) : "";

  let lastDispatchedTitle = "";
  $: if (lastTitleUpdate && chatId && lastTitleUpdate !== lastDispatchedTitle) {
    lastDispatchedTitle = lastTitleUpdate;
    dispatch("title-change", { chatId, title: lastTitleUpdate });
  }

  // ─── Attached cards menu for the header (paperclip → dropdown navigation) ──
  $: attachedCardList = attachedDocs
    .filter((d) => d.active !== false)
    .map((d) => ({ docId: d.docId || d.document_id || "", title: (d.title || "").trim() || String(d.docId || d.document_id || "").split("/").pop() || "Document" }))
    .filter((d) => d.docId);

  let lastAttachedChangeJson = "";
  $: if (chatId) {
    const sig = JSON.stringify(attachedCardList);
    if (sig !== lastAttachedChangeJson) {
      lastAttachedChangeJson = sig;
      dispatch("attached-change", attachedCardList);
    }
  }

  // Скролл к карточке приаттаченного документа по клику в дропдауне тулбара.
  export function scrollToAttachedCard(docId) {
    if (!chat || !chat.messages || !docId || !messagesContainerEl) return;
    const base = String(docId).split("/").pop();
    const msg = chat.messages.find((m) =>
      (m.parts || []).some((p) => p.type === "data-imported" && p.data &&
        (p.data.docId === docId || (base && String(p.data.docId || "").split("/").pop() === base))
      )
    );
    if (!msg) return;
    const el = messagesContainerEl.querySelector(`[data-mid="${CSS.escape(String(msg.id))}"]`);
    if (!el) return;
    el.scrollIntoView({ behavior: "smooth", block: "center" });
    el.classList.add("card-focus-flash");
    setTimeout(() => el.classList.remove("card-focus-flash"), 1800);
  }

  // Владелец — только явный флаг. `isOwner !== false` проходил и для
  // undefined, из-за чего гость публичной ноды получал ragScope="all",
  // private-скоуп и пикер файлов на чужой ноде.
  $: ragScope = isPrivateMode === true || (isPrivateMode === undefined && targetDevice?.isOwner === true)
    ? "all"
    : "public";

  $: filteredPrompts = recentPrompts
    .filter((p) => {
      if (!promptsSearchQuery.trim()) return true;
      const q = promptsSearchQuery.toLowerCase();
      return (p.text || "").toLowerCase().includes(q) || (p.style || "").toLowerCase().includes(q);
    })
    .slice(0, 15);

  async function loadAvailableLoras() {
    if (lorasLoaded && availableLoras.length > 0) return;
    try {
      const ep = "/assistant/loras";
      const res = httpDrive ? await httpDrive.fetch(ep) : await fetch(ep);
      if (res && res.ok) {
        const data = await res.json();
        const list = Array.isArray(data) ? data : (data.loras || data.data || []);
        availableLoras = list.map((l) => ({
          name: l.name || l,
          type: l.type || "character",
          mode: l.mode || (l.nsfw ? "fun" : "work") || "work",
        }));
        lorasLoaded = true;
      }
    } catch (e) {
      console.warn("[chat] failed to load LoRAs:", e);
    }
  }

  function formatPromptTime(iso) {
    if (!iso) return "";
    try {
      const d = new Date(iso);
      if (isNaN(d.getTime())) return "";
      const now = new Date();
      const diffMs = now - d;
      const diffSec = Math.floor(diffMs / 1000);
      if (diffSec < 60) return "just now";
      const diffMin = Math.floor(diffSec / 60);
      if (diffMin < 60) return `${diffMin}m ago`;
      const diffHours = Math.floor(diffMin / 60);
      if (diffHours < 24) return `${diffHours}h ago`;
      const diffDays = Math.floor(diffHours / 24);
      if (diffDays < 7) return `${diffDays}d ago`;
      return d.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    } catch (_) {
      return "";
    }
  }

  function initGeneratorPrompt() {
    showImageToolpanel = true;
    loadAvailableLoras();
    const model = assistantSettingsData?.assistant_model || "Domi";
    const st = selectedStyle || "realistic";
    let text = input.trim();
    if (!text.startsWith("/generate")) {
      text = "/generate " + text;
    }
    const hasAnyChar = availableLoras.filter((l) => l.type === "character").some((l) =>
      text.toLowerCase().includes("<" + l.name.toLowerCase() + ">")
    );
    if (withCharacters && !hasAnyChar) {
      text += " <" + model + ">";
    }
    if (st && !text.toLowerCase().includes("<" + st.toLowerCase() + ">")) {
      text += " <" + st.toLowerCase() + ">";
    }
    input = text.trim() + " ";
  }

  function onComposerInput() {
    resizeTextarea();
    const trimmed = input.trim();
    if (trimmed.startsWith("/generate")) {
      if (!showImageToolpanel) {
        showImageToolpanel = true;
        loadAvailableLoras();
      }
      syncTagsFromInput();
    } else if (showImageToolpanel && !trimmed.startsWith("/")) {
      showImageToolpanel = false;
      showPromptsPopup = false;
    }
  }

  function syncTagsFromInput() {
    const val = input.toLowerCase();
    for (const s of paintingStyles) {
      if (val.includes("<" + s.toLowerCase() + ">")) {
        selectedStyle = s.toLowerCase();
        break;
      }
    }
  }

  function isLoraActive(loraName) {
    if (!input) return false;
    return input.includes("<" + loraName + ">") || input.toLowerCase().includes("<" + loraName.toLowerCase() + ">");
  }

  function isStyleActive(styleName) {
    const target = styleName.toLowerCase();
    const text = (input || "").toLowerCase();
    const hasAnyStyleTag = paintingStyles.some((s) => text.includes("<" + s.toLowerCase() + ">"));
    if (hasAnyStyleTag) {
      return text.includes("<" + target + ">");
    }
    return (selectedStyle || "realistic").toLowerCase() === target;
  }

  function selectPaintingStyle(styleName) {
    const target = styleName.toLowerCase();
    selectedStyle = target;
    let text = input;
    let replaced = false;
    for (const s of paintingStyles) {
      const tagRegex = new RegExp("<" + s + ">", "gi");
      if (tagRegex.test(text)) {
        text = text.replace(tagRegex, "<" + target + ">");
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      if (!text.trim().startsWith("/generate")) {
        text = "/generate <" + target + "> " + text.trim();
      } else {
        const withoutGen = text.trim().slice("/generate".length).trim();
        text = "/generate <" + target + "> " + withoutGen;
      }
    }
    input = text.trim() + " ";
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function toggleLoraChip(loraName) {
    const tag = "<" + loraName + ">";
    let text = input;
    const regex = new RegExp("<" + loraName + ">", "gi");
    if (regex.test(text)) {
      text = text.replace(regex, "").replace(/\s{2,}/g, " ").trim() + " ";
    } else {
      if (!text.trim().startsWith("/generate")) {
        text = "/generate " + text.trim();
      }
      const st = selectedStyle || "realistic";
      const hasStyle = paintingStyles.some((s) => text.toLowerCase().includes("<" + s.toLowerCase() + ">"));
      if (!hasStyle) {
        text = text.trim() + " <" + st + ">";
      }
      text = text.trim() + " " + tag + " ";
    }
    input = text;
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function toggleWithCharacters() {
    withCharacters = !withCharacters;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omc_gen_characters", String(withCharacters));
    }
    const defaultChar = assistantSettingsData?.assistant_model || "Domi";
    if (withCharacters) {
      const hasAnyChar = availableLoras.filter((l) => l.type === "character").some((l) => isLoraActive(l.name));
      if (!hasAnyChar) {
        toggleLoraChip(defaultChar);
      }
    } else {
      let text = input;
      availableLoras.filter((l) => l.type === "character").forEach((l) => {
        text = text.replace(new RegExp("<" + l.name + ">", "gi"), "");
      });
      input = text.replace(/\s{2,}/g, " ").trim() + " ";
      if (textareaEl) setTimeout(resizeTextarea, 0);
    }
  }

  function toggleContinueGenerating() {
    continueGenerating = !continueGenerating;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omc_continue_generating", String(continueGenerating));
    }
  }

  function closeImageGenerator() {
    showImageToolpanel = false;
    showPromptsPopup = false;
    let text = input.replace(/^\/generate\b\s*/i, "").trim();
    if (/^(<[^>]+>\s*)*$/.test(text)) {
      text = "";
    }
    input = text;
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function selectPrompt(p) {
    const txt = typeof p === "string" ? p : p.text;
    let full = txt.startsWith("/generate") ? txt : "/generate " + txt;
    if (p.style && !full.toLowerCase().includes("<" + p.style.toLowerCase() + ">")) {
      full = full.trim() + " <" + p.style.toLowerCase() + ">";
      selectedStyle = p.style.toLowerCase();
    }
    input = full.trim() + " ";
    showPromptsPopup = false;
    if (!showImageToolpanel) {
      showImageToolpanel = true;
      loadAvailableLoras();
    }
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function handleDeletePrompt(e, p) {
    e.stopPropagation();
    if (chatStore && typeof chatStore.deletePrompt === "function") {
      chatStore.deletePrompt(p._id || p.id);
    }
  }

  let showToolsMenu = false;

  function closeToolsMenu(e) {
    if (showToolsMenu && !e.target.closest(".composer-action-wrap") && !e.target.closest(".tools-popup-menu")) {
      showToolsMenu = false;
    }
    if (showPromptsPopup && !e.target.closest(".prompts-popup") && !e.target.closest(".prompts-history-btn")) {
      showPromptsPopup = false;
    }
  }

  function applySlashCommand(cmd) {
    showToolsMenu = false;
    const regex = /^\/(explore|research|think|search|learn|recognize|detect|import|imagine|generate|show)\b\s*/i;
    let current = input.trim();
    if (regex.test(current)) {
      input = current.replace(regex, cmd + " ");
    } else if (current) {
      input = cmd + " " + current;
    } else {
      input = cmd + " ";
    }
    if (cmd === "/generate") {
      initGeneratorPrompt();
    }
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function resizeTextarea() {
    if (!textareaEl) return;
    textareaEl.style.height = "auto";
    const minH = 36;
    const maxH = 180; // Allow up to ~7 lines of text before scrolling
    const scrollH = textareaEl.scrollHeight;
    if (!input || !input.trim()) {
      textareaEl.style.height = minH + "px";
      textareaEl.style.overflowY = "hidden";
      return;
    }
    if (scrollH > maxH) {
      textareaEl.style.height = maxH + "px";
      textareaEl.style.overflowY = "auto";
    } else {
      textareaEl.style.height = Math.max(minH, scrollH) + "px";
      textareaEl.style.overflowY = "hidden";
    }
  }

  function handleKeydown(e) {
    if (showPromptsPopup && e.key === "Escape") {
      e.preventDefault();
      showPromptsPopup = false;
      return;
    }
    if (e.ctrlKey && (e.key === "ArrowUp" || e.key === "ArrowDown")) {
      if (recentPrompts.length > 0 && showImageToolpanel) {
        e.preventDefault();
        if (e.key === "ArrowUp") {
          historyPromptIndex = Math.min(historyPromptIndex + 1, recentPrompts.length - 1);
        } else {
          historyPromptIndex = Math.max(historyPromptIndex - 1, -1);
        }
        if (historyPromptIndex === -1) {
          input = "/generate ";
        } else {
          const p = recentPrompts[historyPromptIndex];
          const txt = typeof p === "string" ? p : p.text;
          input = txt.startsWith("/") ? txt : "/generate " + txt;
          if (p.style) selectedStyle = p.style.toLowerCase();
        }
        if (textareaEl) setTimeout(resizeTextarea, 0);
        return;
      }
    }
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  }

  async function handleSend() {
    const text = input.trim();
    if (!text || !chat || isStreaming) return;

    if (/^\/learn\b/i.test(text)) {
      input = "";
      if (textareaEl) {
        textareaEl.style.height = "36px";
        textareaEl.style.overflowY = "hidden";
      }
      showPromptsPopup = false;
      await handleLearnSend(text);
      return;
    }

    if (/^\/recognize\b/i.test(text)) {
      input = "";
      if (textareaEl) {
        textareaEl.style.height = "36px";
        textareaEl.style.overflowY = "hidden";
      }
      showPromptsPopup = false;
      await handleRecognizeSend(text);
      return;
    }

    const isGen = text.startsWith("/generate");
    if (isGen && chatStore && typeof chatStore.savePrompt === "function") {
      const promptOnly = text.replace(/^\/generate\s*/i, "").replace(/<[^>]+>/g, "").trim();
      if (promptOnly) {
        chatStore.savePrompt(promptOnly, selectedStyle).catch((e) => console.warn("[chat] savePrompt error:", e));
      }
    }

    if (!isGen || !continueGenerating) {
      input = "";
      if (textareaEl) {
        textareaEl.style.height = "36px";
        textareaEl.style.overflowY = "hidden";
      }
      if (!isGen && showImageToolpanel) {
        showImageToolpanel = false;
      }
    }
    showPromptsPopup = false;

    const currentId = chatId;
    const userNonce = "msg_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7);
    const nowIso = new Date().toISOString();

    chatStore
      .addMessage(currentId, {
        nonce: userNonce,
        timestamp: nowIso,
        role: "user",
        content: text,
      })
      .catch((e) => console.warn("[chat] persist user msg error:", e));

    shouldAutoScroll = true;
    scrollToBottom(true);
    isStreaming = true;
    const trimmed = text.trim();
    if (trimmed.startsWith('/generate') || trimmed.startsWith('/show') || trimmed.startsWith('/view') || trimmed.startsWith('/imagine')) {
      activePhaseStatus = "generating";
    } else {
      activePhaseStatus = "processing";
    }

    try {
      const effectiveStorage = resolveEffectiveStorage("");
      const loadedSettings = await loadAssistantSettingsForSend();
      const sendBody = {
        content_mode: contentMode,
        settings: Object.assign(
          {},
          loadedSettings || {},
          effectiveStorage ? { defaultStorage: effectiveStorage } : {},
          $ttsState.voiceStreamEnabled ? { voice_mode: true, voice_stream: true } : {}
        ),
      };
      if ($ttsState.voiceStreamEnabled) {
        sendBody.voice_mode = true;
      }
      const relevantKnowledge = await selectRelevantForMessage(text);
      if (relevantKnowledge.length > 0) {
        sendBody.knowledge = relevantKnowledge;
      }

      if (typeof chat.clearError === "function") {
        chat.clearError();
      }

      if ($ttsState.voiceStreamEnabled) {
        ttsPlayer.startStreamingSession("live-" + Date.now(), httpDrive);
      }

      await chat.sendMessage(
        {
          role: "user",
          id: userNonce,
          parts: [{ type: "text", text }],
        },
        { body: sendBody }
      );

      await persistAssistantReply(currentId);
    } catch (e) {
      console.error("[chat] send error:", e);
    } finally {
      if ($ttsState.voiceStreamEnabled) {
        ttsPlayer.endStreamingSession();
      }
      isStreaming = false;
      activePhaseStatus = "";
      if (chat) chat = chat;
    }
  }

  function stopGeneration() {
    ttsPlayer.stop();
    if (learnAbortController) {
      try { learnAbortController.abort(); } catch (_) {}
    }
    if (chat) {
      try { chat.stop(); } catch (_) {}
    }
    isStreaming = false;
    activePhaseStatus = "";
    if (chat) chat = chat;
  }

  async function postAction(path, body) {
    if (!httpDrive) return;
    try {
      const res = await httpDrive.fetch(path, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!res.ok) console.warn("[chat] action failed:", path, res.status);
    } catch (e) {
      console.error("[chat] action error:", path, e);
    }
  }

  function answerQuestion(requestID, option) {
    answeredActions = new Set([...answeredActions, "q:" + requestID]);
    postAction(`/ai/question/${requestID}/reply`, { answers: [[option]] });
  }

  function rejectQuestion(requestID) {
    answeredActions = new Set([...answeredActions, "q:" + requestID]);
    postAction(`/ai/question/${requestID}/reject`, {});
  }

  function answerPermission(permissionID, allow) {
    answeredActions = new Set([...answeredActions, "p:" + permissionID]);
    postAction(`/ai/permission/${permissionID}/reply`, { reply: allow ? "allow" : "reject" });
  }

  function copyText(text, id) {
    if (!navigator.clipboard) return;
    navigator.clipboard.writeText(text).then(() => {
      copiedId = id;
      if (copiedTimer) clearTimeout(copiedTimer);
      copiedTimer = setTimeout(() => {
        copiedId = null;
      }, 2000);
    });
  }

  let savedImagesSet = new Set();
  // Ключи картинок, показанных из временной /chat/image/-копии (TTL 5 мин), и
  // подтверждённые записи на устройство: пока подтверждения нет — под
  // кнопками Download/Regenerate висит temp_image_hint.
  let tempShownImages = {};
  let savedToDevice = {};

  function markImageSaved(imgObj) {
    const key = imgObj?.path || imgObj?.url || "";
    if (!key) return;
    savedToDevice[key] = true;
    tempShownImages[key] = false;
    if (imgObj?.path) {
      savedToDevice[imgObj.path] = true;
      tempShownImages[imgObj.path] = false;
    }
    if (imgObj?.url) {
      savedToDevice[imgObj.url] = true;
      tempShownImages[imgObj.url] = false;
    }
    savedToDevice = { ...savedToDevice };
    tempShownImages = { ...tempShownImages };
  }

  function blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onloadend = () => {
        const res = reader.result;
        const commaIdx = res ? res.indexOf(",") : -1;
        resolve(commaIdx !== -1 ? res.slice(commaIdx + 1) : (res || ""));
      };
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  async function fetchLargeImageBlob(fetchFn, url) {
    const CHUNK_SIZE = 128 * 1024; // 128 KB safe chunk size for WebRTC SCTP
    let offset = 0;
    let totalLength = null;
    const chunks = [];

    while (totalLength === null || offset < totalLength) {
      const end = totalLength !== null ? Math.min(offset + CHUNK_SIZE - 1, totalLength - 1) : offset + CHUNK_SIZE - 1;
      const resp = await fetchFn(url, {
        headers: {
          Range: `bytes=${offset}-${end}`
        }
      });
      if (!resp.ok && resp.status !== 206) {
        if (offset === 0) {
          return await resp.blob();
        }
        throw new Error(`Range fetch failed: HTTP ${resp.status}`);
      }

      const cr = resp.headers.get("Content-Range") || resp.headers.get("content-range");
      if (cr) {
        const match = /\/(\d+)$/.exec(cr);
        if (match) totalLength = parseInt(match[1], 10);
      }
      const blob = await resp.blob();
      chunks.push(blob);
      offset += blob.size;
      if (blob.size === 0 || (totalLength !== null && offset >= totalLength)) {
        break;
      }
    }

    return new Blob(chunks, { type: "image/png" });
  }

  // Байты сгенерированного фото из временной /chat/image/ копии (без size= —
  // нужен оригинал, а не ресайз). Источник для дозаписи в saveImageToDrive.
  async function fetchGeneratedImageBlob(imgObj) {
    const cleanPath = ((imgObj?.path || "") + "").replace(/^\/+/, "");
    const endpoint = imgObj?.url && imgObj.url.startsWith("/chat/image/")
      ? imgObj.url
      : (cleanPath ? `/chat/image/${cleanPath}` : "");
    if (!endpoint) return null;
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);
      return await fetchLargeImageBlob(fetchFn, endpoint);
    } catch (_) {
      return null;
    }
  }

  async function saveImageToDrive(imgObj, blob = null) {
    if (!imgObj) return;
    if (isStorageOnAiNode()) {
      markImageSaved(imgObj);
      return;
    }
    try {
      const filename = imgObj.filename || imgObj.path || `IMG_${Date.now()}.png`;
      // Тот же путь, куда пушит бэкенд и что показывает generatedSharePath.
      const folder = generatedFolder();
      const saveKey = `${folder}/${filename}`;

      let b64 = "";
      if (blob) {
        b64 = await blobToBase64(blob);
      } else if (imgObj.data) {
        const commaIdx = imgObj.data.indexOf(",");
        b64 = commaIdx !== -1 ? imgObj.data.slice(commaIdx + 1) : imgObj.data;
      } else if (imgObj.path || imgObj.url) {
        // Стрим отдаёт только path — добираем байты из временной /chat/image/
        // копии, пока её TTL не истёк (в момент ответа он ещё действует).
        blob = await fetchGeneratedImageBlob(imgObj);
        if (blob) b64 = await blobToBase64(blob);
      }

      if (!b64) return;
      if (savedImagesSet.has(saveKey)) return;
      savedImagesSet.add(saveKey);

      const title = imgObj.title || "Generated Image";
      const desc = imgObj.description || imgObj.prompt || "";
      const readmeContent = `# ${title}\n\n${desc}\n`;
      const readmeB64 = btoa(unescape(encodeURIComponent(readmeContent)));
      const readmeFilename = filename.replace(/\.[^/.]+$/, "") + ".Readme.md";
      const fullImagePath = `${folder}/${filename}`;
      const fullReadmePath = `${folder}/${readmeFilename}`;

      const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
      const { device } = parseStoragePath(storage);

      const targetDrive = device?.drive
        || deviceDrive
        || storageDevice?.drive
        || (deviceRegistry.list().find((x) => x && x.local && x.drive)?.drive)
        || (deviceRegistry.list().find((x) => x && !x.isAiNode && (x.isOwner || (ownLinkIds && ownLinkIds.includes(x.linkId))) && x.drive)?.drive);

      if (targetDrive) {
        const folderParts = folder.split("/").filter(Boolean);
        let cur = "";
        for (const seg of folderParts) {
          cur += "/" + seg;
          if (typeof targetDrive.homeMkDir === "function") {
            await targetDrive.homeMkDir(cur).catch(() => {});
          } else if (typeof targetDrive.mkDir === "function") {
            await targetDrive.mkDir(cur).catch(() => {});
          }
        }
        if (typeof targetDrive.homeWrite === "function") {
          await targetDrive.homeWrite(fullImagePath, b64);
          await targetDrive.homeWrite(fullReadmePath, readmeB64);
        } else if (typeof targetDrive.writeFileChunked === "function") {
          await targetDrive.writeFileChunked(fullImagePath, b64);
          await targetDrive.writeFileChunked(fullReadmePath, readmeB64);
        }
        console.log("[Chat] Saved generated image and Readme to targetDrive:", fullImagePath);
        markImageSaved(imgObj);
        return;
      }

      if (isPrivateMode && httpDrive && typeof httpDrive.fetch === "function") {
        await httpDrive.fetch(folder, { method: "MKCOL" }).catch(() => {});
        const imageBlob = blob || new Blob([Uint8Array.from(atob(b64), c => c.charCodeAt(0))], { type: "image/png" });
        await httpDrive.fetch(fullImagePath, {
          method: "PUT",
          headers: { "Content-Type": "image/png" },
          body: imageBlob
        }).catch((e) => console.warn("[Chat] httpDrive save image error:", e));
        await httpDrive.fetch(fullReadmePath, {
          method: "PUT",
          headers: { "Content-Type": "text/markdown" },
          body: readmeContent
        }).catch((e) => console.warn("[Chat] httpDrive save readme error:", e));
        console.log("[Chat] Saved generated image and Readme via httpDrive:", fullImagePath);
        markImageSaved(imgObj);
      }

      if (chatStore && typeof chatStore.associateFileWithPrompt === "function") {
        chatStore.associateFileWithPrompt(imgObj.prompt_id || "", filename, desc).catch(() => {});
      }
    } catch (err) {
      console.warn("[Chat] Failed saving generated image to drive:", err);
    }
  }

  function escapeHtml(str) {
    if (!str) return "";
    return String(str)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#039;");
  }

  function formatDisplayPrompt(rawPrompt) {
    if (!rawPrompt) return "";
    if (rawPrompt.includes("Image:")) {
      return rawPrompt.split("Image:", 2)[1].trim();
    }
    return rawPrompt.trim();
  }

  function resolveImageUrl(img) {
    if (!img) return "";
    if (typeof img === "string") return img;
    if (img.data) {
      if (img.data.startsWith("data:")) return img.data;
      return img.data;
    }
    const path = img.path || "";
    if (path.startsWith("http://") || path.startsWith("https://")) {
      return path;
    }
    const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
    if (storage && path) {
      let cleanStorage = storage.replace(/^\/+|\/+$/g, "");
      const knownUsers = [userName, typeof window !== "undefined" && window.__omdProfile?.user, typeof window !== "undefined" && window.__omdProfile?.name].filter(Boolean);
      for (const u of knownUsers) {
        const uLower = u.toLowerCase();
        if (cleanStorage.toLowerCase() === uLower) {
          cleanStorage = DEFAULT_STORAGE_PATH.replace(/^\/+/, "");
          break;
        } else if (cleanStorage.toLowerCase().startsWith(uLower + "/")) {
          cleanStorage = cleanStorage.slice(u.length).replace(/^\/+/, "");
        }
      }
      cleanStorage = cleanStorage.replace(/^(home|Users)\/[^\/]+(\/|$)/i, "").replace(/^\/+|\/+$/g, "");
      if (!cleanStorage) {
        cleanStorage = DEFAULT_STORAGE_PATH.replace(/^\/+/, "");
      }
      const cleanPath = path.replace(/^\/+/, "");
      return cleanStorage ? `/${cleanStorage}/generated/${cleanPath}` : `/generated/${cleanPath}`;
    }
    if (img.url && typeof img.url === "string" && !img.url.startsWith("/generated/")) {
      return img.url;
    }
    return img.url || (path ? `/generated/${path.replace(/^\/+/, "")}` : "");
  }

  function resolveP2pDevice() {
    const storage = resolveEffectiveStorage(DEFAULT_STORAGE_PATH);
    const { device } = parseStoragePath(storage);
    if (device && !device.isAiNode) return device;
    if (storageDevice) return storageDevice;
    if (deviceDrive) {
      const d = deviceRegistry.list().find((x) => x.drive === deviceDrive && !x.isAiNode);
      if (d) return d;
    }
    if (targetDevice && !targetDevice.isAiNode) return targetDevice;
    if (targetLinkId) {
      const d = deviceRegistry.get(targetLinkId) || deviceRegistry.getByPrefix(targetLinkId) || deviceRegistry.getByName(targetLinkId);
      if (d && !d.isAiNode) return d;
    }
    const connectedRpc = deviceRegistry.list().find((d) => !d.isAiNode && d.drive && d.drive.isConnected && typeof d.drive.sendRPC === "function");
    if (connectedRpc) return connectedRpc;
    const connected = deviceRegistry.list().find((d) => !d.isAiNode && d.drive && d.drive.isConnected);
    if (connected) return connected;
    return deviceRegistry.list()[0] || null;
  }

  function getChatImages() {
    const images = [];
    const vs = getViewerSettings();

    for (const m of chat?.messages || []) {
      for (const p of m.parts || []) {
        if (p.type === "data-image" && p.data && (p.data.path || p.data.url || p.data.data)) {
          const path = (p.data.path || "").replace(/^\/+/, "");
          const key = p.data.path || p.data.url;
          const rawTitle = (p.data.title || "").trim();
          const rawDesc = (p.data.description || "").trim();
          const title = isJustFileName(rawTitle, path, key) ? "" : rawTitle;
          const desc = isJustFileName(rawDesc, path, key) ? "" : rawDesc;
          let subHtml = "";
          if (title && desc) {
            subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4><p>${escapeHtml(desc)}</p></div>`;
          } else if (title) {
            subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4></div>`;
          } else if (desc) {
            subHtml = `<div class="omd-gallery-caption"><p>${escapeHtml(desc)}</p></div>`;
          }

          // Единый путь как у приаттаченных: /p2p/{устройство}{путь шара} + ресайз.
          // p2p приоритетен; если устройства нет/p2p ошибся — blob или /chat/image/.
          const sharePath = generatedSharePath(p.data);
          const p2pFailed = p2pImgFailed[key];
          const blobUrl = imageBlobCache[key] || resolveImageUrl(p.data);
          const directUrl = generatedDirectUrl(p.data);

          const srcUrl = (!p2pFailed && generatedDeviceShareUrl(sharePath)) || blobUrl
            || (directUrl ? (vs.downscaleImages ? `${directUrl}?size=${vs.scaleTo}` : directUrl) : "");
          const thumbUrl = (!p2pFailed && generatedDeviceShareUrl(sharePath, "thumb")) || blobUrl
            || (directUrl ? `${directUrl}?size=360` : srcUrl);
          const downloadUrl = (!p2pFailed && generatedDeviceShareUrl(sharePath, "download")) || blobUrl
            || directUrl || srcUrl;

          images.push({
            name: path || key,
            src: srcUrl,
            thumb: thumbUrl,
            downloadUrl,
            subHtml,
          });
        } else if (p.type === "data-imported" && p.data?.docId) {
          // Распознанная/приаттаченная картинка тоже участвует в общей галерее чата:
          // файл с устройства — тот же /p2p+ресайз, гость/фолбэк — base64-превью.
          const deviceImg = isDeviceImageCard(p.data);
          if (!deviceImg && !p.data.imagePreview) continue;
          if (isImportedImageFailed(p.data)) continue;
          const cleanPath = String(docIdToSharePath(p.data.docId)).replace(/^\/+/, "");
          const rawTitle = (p.data.title || "").trim();
          const rawDesc = (p.data.description || p.data.annotation || "").trim();
          const title = isJustFileName(rawTitle, p.data.docId) ? "" : rawTitle;
          const desc = isJustFileName(rawDesc, p.data.docId) ? "" : rawDesc;
          let subHtml = "";
          if (title && desc) {
            subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4><p>${escapeHtml(desc)}</p></div>`;
          } else if (title) {
            subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4></div>`;
          } else if (desc) {
            subHtml = `<div class="omd-gallery-caption"><p>${escapeHtml(desc)}</p></div>`;
          }
          const sharePath = deviceImg ? "/" + cleanPath : "";
          const srcUrl = (deviceImg && deviceShareUrl(sharePath)) || p.data.imagePreview || "";
          const thumbUrl = (deviceImg && deviceShareUrl(sharePath, "thumb")) || p.data.imagePreview || "";
          const downloadUrl = (deviceImg && deviceShareUrl(sharePath, "download")) || p.data.imagePreview || "";
          images.push({ name: p.data.docId, src: srcUrl, thumb: thumbUrl, downloadUrl, subHtml });
        }
      }
    }
    return images;
  }

  // Открывает lightGallery с готовым списком картинок ({src, thumb, downloadUrl, subHtml}).
  function openLightbox(images, startIndex) {
    if (lgChatInstance) {
      try {
        lgChatInstance.destroy();
      } catch (_) {}
      lgChatInstance = null;
    }

    const dynamicEl = images.map((item) => ({
      src: item.src,
      thumb: item.thumb,
      downloadUrl: item.downloadUrl,
      subHtml: item.subHtml,
    }));

    const galleryContainer = document.createElement("div");
    document.body.appendChild(galleryContainer);

    const lgChatPagination = setupGalleryPagination(galleryContainer, dynamicEl, startIndex);

    lgChatInstance = lightGallery(galleryContainer, {
      licenseKey: "GPLv3",
      dynamic: true,
      dynamicEl,
      download: true,
      counter: true,
      closable: true,
      showCloseIcon: true,
      escKey: true,
      swipeToClose: true,
      slideEndAnimation: true,
      mobileSettings: {
        controls: false,
        showCloseIcon: true,
        download: true,
      },
    });
    lgChatPagination.setInstance(lgChatInstance);

    galleryContainer.addEventListener("lgAfterClose", () => {
      try {
        if (lgChatPagination) lgChatPagination.cleanup();
        if (lgChatInstance) lgChatInstance.destroy();
      } catch (_) {}
      lgChatInstance = null;
      if (galleryContainer.parentNode) {
        galleryContainer.parentNode.removeChild(galleryContainer);
      }
    });

    lgChatInstance.openGallery(startIndex);
    return lgChatPagination;
  }

  async function openChatGallery(activePath) {
    if (activePath) {
      for (const m of chat?.messages || []) {
        for (const p of m.parts || []) {
          if (p.type === "data-image" && p.data) {
            const key = p.data.path || p.data.url;
            if (key === activePath || p.data.path === activePath || (p.data.path && p.data.path.endsWith(activePath))) {
              if (!imageBlobCache[key]) {
                await loadImageBlob(p.data);
              }
            }
          }
        }
      }
    }

    const images = getChatImages();
    if (!images.length) return;

    let startIndex = 0;
    if (activePath) {
      const cleanActive = (activePath || "").replace(/^\/+/, "");
      const idx = images.findIndex((img) => img.name === cleanActive || img.name === activePath);
      if (idx !== -1) startIndex = idx;
    }

    openLightbox(images, startIndex);
  }

  // Галерея по фотографиям из блока источников сообщения: список — только картинки
  // этого блока, а не все фото папки, в которой лежит выбранная.
  function openSourcesGallery(activeSrc, msg) {
    let sources = (msg ? (getMessageSources(msg) || []).filter(isImageSource) : []);
    const activeKey = (activeSrc && (activeSrc.fullPath || activeSrc.path || activeSrc.document_id || activeSrc.url || "")) || "";
    if (activeSrc && !sources.some((s) => (s.fullPath || s.path || s.document_id || s.url || "") === activeKey)) {
      sources = [activeSrc, ...sources];
    }
    if (!sources.length) {
      if (activeSrc) dispatch("open-file", { source: activeSrc });
      return;
    }

    const images = sources.map(sourceGalleryItem);
    let startIndex = 0;
    if (activeKey) {
      const idx = images.findIndex((img) => img.name === activeKey);
      if (idx !== -1) startIndex = idx;
    }

    const pagination = openLightbox(images, startIndex);

    // Догрузка описаний из соседнего Readme.md — как в файловой галерее App.svelte,
    // чтобы блок подписи был абсолютно таким же.
    (async () => {
      for (let i = 0; i < sources.length; i++) {
        // Галерея уже закрылась — догрузку описаний прекращаем
        if (!document.querySelector(".lg-container.lg-show")) return;
        const src = sources[i];
        const raw = src.fullPath || src.path || src.document_id || src.url || "";
        if (!raw || !raw.startsWith("/") || images[i].subHtml) continue;
        const base = sourceShareBaseUrl(src);
        if (!base) continue;
        const seg = base.slice(0, base.length - encodeURI(raw).length);
        const noExt = raw.replace(/\.[^.]+$/, "");
        try {
          let res = await fetch(`${seg}${encodeURI(noExt)}.Readme.md`);
          if (!res.ok) res = await fetch(`${seg}${encodeURI(raw)}.Readme.md`);
          if (res.ok) {
            const md = await res.text();
            if (md && md.trim()) {
              const lines = md.trim().split("\n");
              let title = "";
              const descLines = [];
              for (const rawLine of lines) {
                const l = rawLine.trim();
                if (!l) continue;
                if (!title && l.startsWith("#")) {
                  title = l.replace(/^#+\s*/, "").trim();
                } else if (!title) {
                  title = l;
                } else {
                  descLines.push(l);
                }
              }
              const desc = descLines.join(" ").trim();
              const validTitle = title && !isJustFileName(title, raw);
              const validDesc = desc && !isJustFileName(desc, raw);
              let subHtml = "";
              if (validTitle && validDesc) {
                subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4><p>${escapeHtml(desc)}</p></div>`;
              } else if (validTitle) {
                subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4></div>`;
              } else if (validDesc) {
                subHtml = `<div class="omd-gallery-caption"><p>${escapeHtml(desc)}</p></div>`;
              }
              if (subHtml) {
                images[i].subHtml = subHtml;
                if (pagination) pagination.updateItems(images);
              }
            }
          }
        } catch (_) {}
      }
    })();
  }

  function downloadImage(imgObj) {
    if (!imgObj) return;
    const key = imgObj.path || imgObj.url;
    const filename = (imgObj.path || "generated-image.png").split("/").pop();
    const cleanPath = (imgObj.path || "").replace(/^\/+/, "");

    // Приоритет: файл на устройстве → уже загруженный blob → временная копия.
    let downloadUrl = generatedDeviceShareUrl(generatedSharePath(imgObj), "download")
      || imageBlobCache[key]
      || imgObj.url
      || (cleanPath ? `/chat/image/${cleanPath}` : "");

    const link = document.createElement("a");
    link.href = downloadUrl;
    link.download = filename;
    link.target = "_blank";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }

  function openGeneratedFolder(path) {
    const folderPath = "/generated";
    dispatch("open-file", { source: { path: folderPath, fullPath: folderPath, isDir: true } });
  }

  function copyPrompt(text, id) {
    if (!navigator.clipboard || !text) return;
    navigator.clipboard.writeText(text).then(() => {
      copiedPromptId = id;
      setTimeout(() => { copiedPromptId = null; }, 2000);
    });
  }

  async function setQuickPrompt(text) {
    input = text;
    if (textareaEl) {
      textareaEl.value = text;
    }
    await tick();
    if (textareaEl) {
      textareaEl.focus();
      resizeTextarea();
    }
  }

  function preloadSourceThumb(src) {
    if (!src || !thumbFetcher) return;
    const docId = (typeof src === "string") ? src : (src.document_id || src.path || src.fullPath || "");
    if (docId && isImageSource(src) && isSourceAccessible(src) && !sourceThumbUrls[docId] && !sourceThumbLoading.has(docId)) {
      sourceThumbLoading.add(docId);
      thumbFetcher(docId, 240)
        .then((url) => {
          if (url) {
            sourceThumbUrls = { ...sourceThumbUrls, [docId]: url };
          }
        })
        .catch(() => {})
        .finally(() => sourceThumbLoading.delete(docId));
    }
  }

  function getMessageSources(m) {
    const parts = m.parts || [];
    const fromParts = parts.filter((p) => p.type === "source-url");
    let res = [];
    if (fromParts.length) res = fromParts;
    else if (Array.isArray(m.sources)) res = m.sources;
    for (const s of res) preloadSourceThumb(s);
    return res;
  }

  function cleanSourcePath(p) {
    if (!p) return "";
    let s = String(p);
    if (!isPrivateMode && !isOwnerMode) {
      s = s.replace(/^\/[^\/]+(?=\/)/, "");
    }
    return s;
  }

  // Решает доступность источника для ТЕКУЩЕГО пользователя:
  // Если устройство в списке устройств пользователя (или пользователь владелец / private mode) -> доступно (кликабельно, с превью).
  function isSourceAccessible(src) {
    if (!src) return false;
    if (src.clickable === false) return false;
    let raw = (typeof src === "string") ? src : (src.fullPath || src.path || src.realPath || src.document_id || src.url || "");
    if (!raw) return false;
    if (raw.startsWith("http://") || raw.startsWith("https://")) {
      try {
        const u = new URL(raw);
        if (u.pathname && u.pathname !== "/") {
          raw = decodeURIComponent(u.pathname);
        } else {
          return true;
        }
      } catch (_) {
        return true;
      }
    }

    // В приватном режиме или режиме владельца всё содержимое ноды доступно
    if (isPrivateMode || isOwnerMode) return true;

    const parts = raw.split("/").filter(Boolean);
    if (!parts.length) return false;
    const devSegment = parts[0].toLowerCase();

    // 1. Проверяем storageDevice и targetDevice
    for (const d of [storageDevice, targetDevice]) {
      if (!d) continue;
      const pfx = (d.prefix ? d.prefix.replace(/^\/+/, "") : "").toLowerCase();
      const name = (d.name || "").toLowerCase();
      const linkId = (d.linkId || "").toLowerCase();
      const devId = (d.deviceId || "").toLowerCase();
      const host = (d.hostname || "").toLowerCase();
      const rootName = (d.drive?.rootName || "").toLowerCase();
      if (devSegment === pfx || devSegment === name || devSegment === linkId || devSegment === devId || devSegment === host || devSegment === rootName) return true;
    }

    // 2. Проверяем defaultStorage и resolveEffectiveStorage()
    try {
      const effStorage = resolveEffectiveStorage("");
      if (effStorage) {
        const parsed = parseStoragePath(effStorage);
        const parsedDev = (parsed.deviceName || "").toLowerCase();
        const stParts = effStorage.split("/").filter(Boolean);
        const stFirst = stParts[0] ? stParts[0].toLowerCase() : "";
        if (devSegment === parsedDev || devSegment === stFirst) return true;
      }
    } catch (_) {}

    // 3. Проверяем ownLinkIds
    const ownSet = new Set((ownLinkIds || []).map((x) => String(x).toLowerCase()));
    if (ownSet.has(devSegment)) return true;

    // 4. Проверяем deviceRegistry: если устройство есть в списке устройств пользователя — доступно!
    const d = deviceRegistry.getByPrefix(devSegment) || deviceRegistry.getByName(devSegment) || deviceRegistry.get(devSegment);
    if (d) return true;

    for (const dev of deviceRegistry.list()) {
      if (!dev) continue;
      const pfx = (dev.prefix ? dev.prefix.replace(/^\/+/, "") : "").toLowerCase();
      const name = (dev.name || "").toLowerCase();
      const linkId = (dev.linkId || "").toLowerCase();
      const devId = (dev.deviceId || "").toLowerCase();
      const host = (dev.hostname || "").toLowerCase();
      const rootName = (dev.drive?.rootName || "").toLowerCase();
      if (devSegment === pfx || devSegment === name || devSegment === linkId || devSegment === devId || devSegment === host || devSegment === rootName) {
        return true;
      }
      if (dev.linkId && ownSet.has(dev.linkId.toLowerCase())) {
        if (devSegment === pfx || devSegment === name || devSegment === host) return true;
      }
    }

    // 5. Проверяем identity.devices (если объект identity доступен в window)
    if (typeof window !== "undefined" && window.identity && window.identity.devices) {
      try {
        const devs = window.identity.devices;
        const entries = typeof devs.values === "function" ? Array.from(devs.values()) : Object.values(devs);
        for (const dev of entries) {
          if (!dev) continue;
          const name = (dev.name || "").toLowerCase();
          const devId = (dev.deviceId || dev.linkId || "").toLowerCase();
          const pfx = (dev.prefix ? dev.prefix.replace(/^\/+/, "") : "").toLowerCase();
          const host = (dev.hostname || "").toLowerCase();
          if (devSegment === name || devSegment === devId || devSegment === pfx || devSegment === host) return true;
        }
      } catch (_) {}
    }

    // 6. Локальное устройство и хост текущей ноды
    const locSeg = localDeviceSegment() ? localDeviceSegment().toLowerCase() : "";
    if (devSegment === "local" || devSegment === "this device" || (locSeg && devSegment === locSeg)) return true;

    if (typeof window !== "undefined") {
      const locHost = (window.location.hostname || "").toLowerCase();
      const shortHost = locHost.split(".")[0];
      if (devSegment === locHost || devSegment === shortHost) return true;
      if (locHost === "localhost" || locHost === "127.0.0.1") return true;
    }

    return false;
  }

  // Превью источника-изображения
  const IMAGE_SRC_EXT = /\.(jpe?g|png|gif|webp|heic|heif|svg|bmp|avif)(\?.*)?$/i;

  function isImageSource(src) {
    if (!src) return false;
    if (typeof src === "string") return IMAGE_SRC_EXT.test(src.split("?")[0]);
    const mime = (src.mimetype || src.mimeType || src.contentType || "").toLowerCase();
    if (mime.startsWith("image/")) return true;
    const p = src.fullPath || src.path || src.realPath || src.document_id || src.url || src.title || src.name || "";
    return IMAGE_SRC_EXT.test(String(p).split("?")[0]);
  }

  // Базовый URL источника-изображения с устройства: /p2p/{устройство}{путь шары}.
  // Полностью согласован с логикой создания ссылок в галерее App.svelte (resolveSourceFileModel).
  function sourceShareBaseUrl(src) {
    if (!src) return "";
    let raw = "";
    if (typeof src === "string") {
      raw = src;
    } else {
      raw = src.fullPath || src.path || src.realPath || src.document_id || src.id || src.url || "";
    }
    if (!raw || typeof raw !== "string") return "";

    // Если уже /p2p/ путь
    if (raw.startsWith("/p2p/")) {
      return raw.split("?")[0];
    }
    if (raw.startsWith("/chat/")) return raw;

    // Если полный http(s) URL (напр. https://onmydisk.net/beelink/...), извлекаем pathname
    if (raw.startsWith("http://") || raw.startsWith("https://")) {
      try {
        const u = new URL(raw);
        raw = decodeURIComponent(u.pathname);
      } catch (_) {
        raw = raw.replace(/^https?:\/\/[^\/]+/, "");
      }
    } else {
      try {
        raw = decodeURIComponent(raw);
      } catch (_) {}
    }

    if (!raw.startsWith("/")) raw = "/" + raw;

    let segments = raw.split("/").filter(Boolean);
    if (!segments.length) return "";

    // 1. Убираем имя владельца / пользователя, если оно в первом сегменте
    const owner = (typeof src === "object" ? src.owner : "") || (typeof window !== "undefined" && window.identity && window.identity.user) || "";
    if (segments.length > 0 && owner && segments[0].toLowerCase() === owner.toLowerCase()) {
      segments.shift();
    }
    if (!segments.length) return "";

    // 2. Резолвим устройство по первому сегменту (prefix, linkId, или hostname)
    let dev = (typeof src === "object" && src.device && src.device.drive) ? src.device : null;
    let devPrefix = "";
    if (segments.length > 0) {
      const candidate = segments[0];
      const matchedDev = deviceRegistry.getByPrefix(candidate) ||
                         deviceRegistry.get(candidate) ||
                         deviceRegistry.getByName(candidate);
      if (matchedDev) {
        if (!dev) dev = matchedDev;
        segments.shift();
      } else if (dev) {
        const dp = (dev.prefix || dev.linkId || "").replace(/^\//, "");
        if (dp && candidate.toLowerCase() === dp.toLowerCase()) {
          segments.shift();
        }
      }
    }

    // Фолбэки поиска устройства
    if (!dev && typeof src === "object" && src.linkId) {
      dev = deviceRegistry.get(src.linkId) || deviceRegistry.getByPrefix(src.linkId) || deviceRegistry.getByName(src.linkId);
    }
    if (!dev && targetLinkId) {
      dev = deviceRegistry.get(targetLinkId) || deviceRegistry.getByPrefix(targetLinkId) || deviceRegistry.getByName(targetLinkId);
    }
    if (!dev) {
      const allDevs = deviceRegistry.list();
      if (allDevs.length > 0) {
        const candidate = segments.length > 1 ? segments[0] : "";
        if (candidate) {
          dev = allDevs.find((d) => {
            const dp = (d.prefix ? d.prefix.replace(/^\/+/, "") : "").toLowerCase();
            const dn = (d.name || "").toLowerCase();
            const dl = (d.linkId || "").toLowerCase();
            return dp === candidate.toLowerCase() || dn === candidate.toLowerCase() || dl === candidate.toLowerCase();
          });
          if (dev) segments.shift();
        }
      }
    }

    if (dev) {
      devPrefix = dev.prefix || ("/" + (dev.linkId || dev.name || ""));
    } else if (segments.length > 1) {
      const firstSeg = segments.shift();
      devPrefix = "/" + firstSeg;
    } else {
      const firstDev = deviceRegistry.list()[0];
      devPrefix = (firstDev && (firstDev.prefix || ("/" + (firstDev.linkId || firstDev.name)))) || ("/" + (segments[0] || ""));
    }

    const cleanPrefix = devPrefix.startsWith("/") ? devPrefix : ("/" + devPrefix);
    const realPath = "/" + segments.join("/");

    return `/p2p${cleanPrefix}` + encodeURI(realPath);
  }

  let sourceThumbUrls = {};
  const sourceThumbLoading = new Set();

  function sourcePreviewKey(src) {
    if (!src) return "";
    if (typeof src === "string") return src;
    return src.fullPath || src.path || src.realPath || src.document_id || src.url || src.sourceId || src.title || "";
  }

  function sourcePreviewUrl(src, size = 240) {
    if (!isImageSource(src)) return "";
    if (!isSourceAccessible(src)) return "";
    const k = sourcePreviewKey(src);
    if (k && sourceThumbUrls[k]) return sourceThumbUrls[k];
    const docId = (typeof src === "object") ? (src.document_id || src.fullPath || src.path) : src;
    if (docId && sourceThumbUrls[docId]) return sourceThumbUrls[docId];

    const base = sourceShareBaseUrl(src);
    if (base) {
      return `${base}?resize=true&width=${size}&height=${size}`;
    }
    return "";
  }

  // Элемент галереи по источнику-картинке: полноразмерный src, миниатюра, download.
  function sourceGalleryItem(src) {
    const raw = (typeof src === "string") ? src : (src.fullPath || src.path || src.realPath || src.document_id || src.url || "");
    const vs = getViewerSettings();
    const fallbackThumb = sourceThumbUrls[raw] || "";
    const base = sourceShareBaseUrl(src);
    const thumbUrl = fallbackThumb || (base ? `${base}?resize=true&width=360&height=360` : "") || sourcePreviewUrl(src, 360);
    const directApiUrl = `/api/device_memory_thumb?path=${encodeURIComponent(raw)}&size=1600`;

    let viewUrl = "";
    if (base) {
      viewUrl = (vs.downscaleImages && !base.startsWith("http"))
        ? `${base}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}`
        : base;
    }

    const srcUrl = viewUrl || directApiUrl || thumbUrl;
    const downloadUrl = (base && base.startsWith("http")) ? base : (base || directApiUrl);
    const rawTitle = (typeof src === "object" && src.title ? src.title : "").trim();
    const title = isJustFileName(rawTitle, raw, raw) ? "" : rawTitle;
    let subHtml = "";
    if (title) {
      subHtml = `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4></div>`;
    }
    return { name: raw, src: srcUrl, thumb: thumbUrl, downloadUrl, subHtml };
  }

  function markSourcePreviewFailed(src) {
    const k = sourcePreviewKey(src);
    if (!k) return;
    const docId = (typeof src === "object") ? (src.document_id || src.path || src.fullPath) : src;
    if (docId && thumbFetcher && !sourceThumbUrls[docId] && !sourceThumbLoading.has(docId)) {
      sourceThumbLoading.add(docId);
      thumbFetcher(docId, 240)
        .then((url) => {
          if (url) {
            sourceThumbUrls = { ...sourceThumbUrls, [docId]: url, [k]: url };
            sourcePreviewFailed = { ...sourcePreviewFailed, [k]: false };
          } else {
            sourcePreviewFailed = { ...sourcePreviewFailed, [k]: true };
          }
        })
        .catch(() => {
          sourcePreviewFailed = { ...sourcePreviewFailed, [k]: true };
        })
        .finally(() => sourceThumbLoading.delete(docId));
      return;
    }
    sourcePreviewFailed = { ...sourcePreviewFailed, [k]: true };
  }

  function toggleSources(msgId) {
    expandedSources = {
      ...expandedSources,
      [msgId]: !expandedSources[msgId],
    };
  }

  function handleSourceClick(src, msg) {
    if (!src) return;
    if (!isSourceAccessible(src)) return;
    let path = (typeof src === "string") ? src : (src.path || src.fullPath || src.realPath || src.document_id || src.url || "");
    if (!path) return;
    if (path.startsWith("http://") || path.startsWith("https://")) {
      try {
        const u = new URL(path);
        path = decodeURIComponent(u.pathname);
      } catch (_) {
        path = path.replace(/^https?:\/\/[^\/]+/, "");
      }
    }
    if (!path.startsWith("/")) path = "/" + path;
    const cleanSrc = typeof src === "object"
      ? { ...src, path, fullPath: path }
      : { path, fullPath: path, document_id: path, title: path.split("/").pop() };
    dispatch("open-file", { source: cleanSrc });
  }

  // ─── Inline clickable chips for source files mentioned in the answer ─────
  const INLINE_SKIP_TAGS = new Set(["A", "PRE", "CODE", "KBD", "SAMP", "SCRIPT", "STYLE", "TABLE", "TH", "TD"]);
  const INLINE_CHIP_CLS = "inline-file-chip";

  const KNOWN_FILE_EXTENSIONS = new Set([
    "docx", "doc", "odt", "xlsx", "xls", "pptx", "ppt", "pdf",
    "txt", "md", "csv", "json", "jpg", "jpeg", "png", "gif",
    "webp", "heic", "heif", "svg", "bmp", "mp4", "webm", "mkv",
    "avi", "mov", "mp3", "wav", "ogg", "flac", "m4a", "aac", "opus"
  ]);

  const FILE_PATH_RE = /(?:^|(?<=[\s`"'(<«»]))(\/[\w\p{L}\-\.@~]+(?:\/[\w\p{L}\-\.@~]+)*\.(?:[a-zA-Z0-9]{2,5}))(?=$|[\s`"')>».,:;!?])/gui;
  const FILE_NAME_RE = /(?:^|(?<=[\s`"'(<«»]))([\w\p{L}\-\.@~]+\.(?:docx|doc|odt|xlsx|xls|pptx|ppt|pdf|txt|md|csv|json|jpg|jpeg|png|gif|webp|heic|heif|svg|bmp|mp4|webm|mkv|avi|mov|mp3|wav|ogg|flac|m4a|aac|opus))(?=$|[\s`"')>».,:;!?])/gui;

  function isPathWithKnownExt(pathStr) {
    if (!pathStr || typeof pathStr !== "string") return false;
    const clean = pathStr.trim();
    const dot = clean.lastIndexOf(".");
    if (dot <= 0) return false;
    const ext = clean.substring(dot + 1).toLowerCase();
    return KNOWN_FILE_EXTENSIONS.has(ext);
  }

  function inlineSourceName(src) {
    const raw = src.fullPath || src.path || src.document_id || src.url || "";
    return String(src.title || raw.split("/").pop() || "").trim();
  }

  function escapeRegExp(s) {
    return String(s).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  }

  function makeInlineChip(source, onopen, name) {
    const accessible = isSourceAccessible(source);
    const a = document.createElement(accessible ? "a" : "span");
    if (accessible) {
      a.href = "javascript:void(0)";
    }
    a.className = INLINE_CHIP_CLS + (accessible ? "" : " static");
    const fullPath = (source && (source.fullPath || source.path || source.document_id)) || (name && name.startsWith("/") ? name : "");
    a.title = fullPath || name || "";

    // Отображаем только имя файла (как в источниках), а полный путь — в тултипе
    const rawName = name || (source && (source.title || source.name)) || fullPath || "";
    const displayName = rawName ? rawName.split("/").pop() : "";

    if (accessible && isImageSource(source)) {
      const raw = source.fullPath || source.path || source.document_id || "";
      const preview = sourcePreviewUrl(source, 120);
      const img = document.createElement("img");
      img.className = "inline-file-thumb";
      img.alt = "";
      img.loading = "lazy";

      let iconFallback = null;
      function ensureFallbackIcon() {
        img.style.display = "none";
        if (!iconFallback && !a.querySelector(".inline-file-icon")) {
          iconFallback = document.createElement("span");
          iconFallback.className = "inline-file-icon";
          const dot = displayName.lastIndexOf(".");
          if (dot > 0) {
            const ext = displayName.substring(dot + 1).trim();
            if (ext && ext.length <= 4) iconFallback.textContent = ext.toUpperCase();
          }
          const nameSpan = a.querySelector(".inline-file-name");
          if (nameSpan) {
            a.insertBefore(iconFallback, nameSpan);
          } else {
            a.appendChild(iconFallback);
          }
        }
      }

      function applyThumb(url) {
        if (!url) return;
        img.src = url;
        img.style.display = "";
        if (iconFallback) {
          iconFallback.remove();
          iconFallback = null;
        }
        const existingIcon = a.querySelector(".inline-file-icon");
        if (existingIcon) existingIcon.remove();
      }

      if (raw && sourceThumbUrls[raw]) {
        applyThumb(sourceThumbUrls[raw]);
      } else if (preview) {
        applyThumb(preview);
      } else {
        ensureFallbackIcon();
      }

      // Проактивно получаем blob через thumbFetcher для гарантии превью
      if (raw && thumbFetcher && !sourceThumbUrls[raw]) {
        thumbFetcher(raw, 120)
          .then((blobUrl) => {
            if (blobUrl) {
              sourceThumbUrls[raw] = blobUrl;
              applyThumb(blobUrl);
            }
          })
          .catch(() => {});
      }

      img.addEventListener("error", () => {
        // Если картинка не смогла загрузиться напрямую, пробуем thumbFetcher
        if (raw && thumbFetcher && !img.dataset.retried) {
          img.dataset.retried = "1";
          thumbFetcher(raw, 120)
            .then((blobUrl) => {
              if (blobUrl) {
                sourceThumbUrls[raw] = blobUrl;
                applyThumb(blobUrl);
              } else {
                ensureFallbackIcon();
              }
            })
            .catch(() => {
              ensureFallbackIcon();
            });
        } else {
          ensureFallbackIcon();
        }
      });

      a.appendChild(img);
    } else {
      const icon = document.createElement("span");
      icon.className = "inline-file-icon";
      const dot = displayName.lastIndexOf(".");
      if (dot > 0) {
        const ext = displayName.substring(dot + 1).trim();
        if (ext && ext.length <= 4) icon.textContent = ext.toUpperCase();
      }
      a.appendChild(icon);
    }
    const span = document.createElement("span");
    span.className = "inline-file-name";
    span.textContent = displayName;
    a.appendChild(span);
    if (accessible) {
      a.addEventListener("click", (e) => {
        e.preventDefault();
        e.stopPropagation();
        onopen(source);
      });
    }
    return a;
  }

  // Заменяет упоминания имён файлов-источников и путей в отрендеренном markdown на кликабельные чипсы.
  function enhanceInlineSources(container, sources, onopen) {
    if (!container || typeof onopen !== "function") return false;
    const sourceList = Array.isArray(sources) ? sources.filter((s) => s && s.clickable !== false) : [];

    const matchers = [];
    for (const s of sourceList) {
      const name = inlineSourceName(s);
      if (name) {
        matchers.push({
          source: s,
          name,
          re: new RegExp("(?<![\\w.\\-])" + escapeRegExp(name) + "(?![\\w])", "g"),
        });
      }
      const full = s.fullPath || s.path || s.document_id || "";
      if (full && full.startsWith("/") && full !== name) {
        matchers.push({
          source: s,
          name: full,
          re: new RegExp("(?<![\\w])" + escapeRegExp(full) + "(?![\\w])", "g"),
        });
      }
    }
    if (matchers.length) {
      matchers.sort((a, b) => b.name.length - a.name.length);
    }

    let changed = false;

    // 1. Обрабатываем инлайн-теги <code>, если в них упомянут путь к файлу или имя источника
    const codeElements = Array.from(container.querySelectorAll("code"));
    for (const code of codeElements) {
      if (code.closest("pre") || code.closest("." + INLINE_CHIP_CLS)) continue;
      const codeText = (code.textContent || "").trim();
      if (!codeText) continue;

      let matchedSource = null;
      let displayName = codeText;
      for (const m of matchers) {
        if (codeText === m.name || codeText.endsWith("/" + m.name)) {
          matchedSource = m.source;
          break;
        }
      }
      if (!matchedSource && isPathWithKnownExt(codeText)) {
        const cleanCode = codeText.trim();
        const parts = cleanCode.split("/").filter(Boolean);
        if (parts.length >= 2) {
          const fullPath = cleanCode.startsWith("/") ? cleanCode : ("/" + cleanCode);
          matchedSource = {
            title: parts[parts.length - 1],
            fullPath,
            path: fullPath,
            clickable: true
          };
        }
      }

      if (matchedSource) {
        changed = true;
        const chip = makeInlineChip(matchedSource, onopen, displayName);
        code.parentNode.replaceChild(chip, code);
      }
    }

    // 1b. Обрабатываем теги <strong> и <b>, если в них путь или имя файла (с возможным дублем в скобках следом)
    const strongElements = Array.from(container.querySelectorAll("strong, b"));
    for (const st of strongElements) {
      if (st.closest("pre") || st.closest("." + INLINE_CHIP_CLS)) continue;
      const stText = (st.textContent || "").trim();
      if (!stText) continue;

      let matchedSource = null;
      let displayName = stText;
      for (const m of matchers) {
        if (stText === m.name || stText.endsWith("/" + m.name)) {
          matchedSource = m.source;
          break;
        }
      }
      if (!matchedSource && isPathWithKnownExt(stText)) {
        const cleanSt = stText.trim();
        const parts = cleanSt.split("/").filter(Boolean);
        if (parts.length >= 2) {
          const fullPath = cleanSt.startsWith("/") ? cleanSt : ("/" + cleanSt);
          matchedSource = {
            title: parts[parts.length - 1],
            fullPath,
            path: fullPath,
            clickable: true
          };
        }
      }

      if (matchedSource) {
        changed = true;
        // Если за <strong> идёт скобка с дублирующим путем " (/path/file.ext)", убираем её
        const next = st.nextSibling;
        if (next && next.nodeType === Node.TEXT_NODE) {
          const nextVal = next.nodeValue || "";
          const parenMatch = /^\s*\(([\w\p{L}\-\.@~/]+\.(?:[a-zA-Z0-9]{2,5}))\)/u.exec(nextVal);
          if (parenMatch) {
            const parenPath = parenMatch[1];
            const cleanFn = stText.split("/").pop();
            if (parenPath === stText || parenPath.endsWith("/" + cleanFn) || parenPath.split("/").pop() === cleanFn) {
              next.nodeValue = nextVal.slice(parenMatch[0].length);
              if (!matchedSource.fullPath || matchedSource.fullPath === cleanFn) {
                matchedSource.fullPath = parenPath.startsWith("/") ? parenPath : ("/" + parenPath);
                matchedSource.path = matchedSource.fullPath;
              }
            }
          }
        }

        const chip = makeInlineChip(matchedSource, onopen, displayName);
        st.parentNode.replaceChild(chip, st);
      }
    }

    // 2. Обрабатываем текстовые узлы
    const walker = document.createTreeWalker(container, NodeFilter.SHOW_TEXT, {
      acceptNode(node) {
        let p = node.parentNode;
        while (p && p !== container) {
          if (p.nodeType === 1) {
            if (INLINE_SKIP_TAGS.has(p.tagName)) return NodeFilter.FILTER_REJECT;
            if ((p.className || "").split(" ").indexOf(INLINE_CHIP_CLS) !== -1) return NodeFilter.FILTER_REJECT;
            if (p.closest && p.closest("." + INLINE_CHIP_CLS)) return NodeFilter.FILTER_REJECT;
          }
          p = p.parentNode;
        }
        return NodeFilter.FILTER_ACCEPT;
      },
    });

    const textNodes = [];
    while (walker.nextNode()) textNodes.push(walker.currentNode);

    for (const node of textNodes) {
      const text = node.nodeValue || "";
      if (!text) continue;

      const hits = [];

      for (const m of matchers) {
        m.re.lastIndex = 0;
        let mm;
        while ((mm = m.re.exec(text)) !== null) {
          hits.push({
            start: mm.index,
            end: mm.index + mm[0].length,
            source: m.source,
            name: mm[0]
          });
          if (mm.index === m.re.lastIndex) m.re.lastIndex++;
        }
      }

      const DUP_RE = /([\w\p{L}\-\.@~]+\.(?:[a-zA-Z0-9]{2,5}))\s*\(([\w\p{L}\-\.@~/]+\.(?:[a-zA-Z0-9]{2,5}))\)/gui;
      let dm;
      while ((dm = DUP_RE.exec(text)) !== null) {
        const fn = dm[1];
        const fp = dm[2];
        if (isPathWithKnownExt(fn) && (fp.endsWith("/" + fn) || fp === fn || fp.split("/").pop() === fn)) {
          const matchStart = dm.index;
          const matchEnd = dm.index + dm[0].length;
          const fullPath = fp.startsWith("/") ? fp : ("/" + fp);
          const foundSrc = sourceList.find(s => (s.fullPath === fullPath || s.path === fullPath || s.document_id === fullPath));
          hits.push({
            start: matchStart,
            end: matchEnd,
            source: foundSrc || { title: fn, fullPath, path: fullPath, clickable: true },
            name: fn
          });
        }
        if (dm.index === DUP_RE.lastIndex) DUP_RE.lastIndex++;
      }

      FILE_PATH_RE.lastIndex = 0;
      let pm;
      while ((pm = FILE_PATH_RE.exec(text)) !== null) {
        const fullMatch = pm[1];
        if (fullMatch && isPathWithKnownExt(fullMatch)) {
          const parts = fullMatch.split("/").filter(Boolean);
          if (parts.length >= 2) {
            const matchStart = pm.index + (pm[0].indexOf(fullMatch));
            const matchEnd = matchStart + fullMatch.length;
            if (!hits.some(h => Math.max(h.start, matchStart) < Math.min(h.end, matchEnd))) {
              const foundSrc = sourceList.find(s => (s.fullPath === fullMatch || s.path === fullMatch));
              hits.push({
                start: matchStart,
                end: matchEnd,
                source: foundSrc || {
                  title: parts[parts.length - 1],
                  fullPath: fullMatch,
                  path: fullMatch,
                  clickable: true
                },
                name: fullMatch
              });
            }
          }
        }
        if (pm.index === FILE_PATH_RE.lastIndex) FILE_PATH_RE.lastIndex++;
      }

      FILE_NAME_RE.lastIndex = 0;
      let fnm;
      while ((fnm = FILE_NAME_RE.exec(text)) !== null) {
        const fullMatch = fnm[1];
        if (fullMatch && isPathWithKnownExt(fullMatch)) {
          const matchStart = fnm.index + (fnm[0].indexOf(fullMatch));
          const matchEnd = matchStart + fullMatch.length;
          if (!hits.some(h => Math.max(h.start, matchStart) < Math.min(h.end, matchEnd))) {
            const foundSrc = sourceList.find(s => 
              inlineSourceName(s) === fullMatch || 
              s.title === fullMatch || 
              (s.fullPath && s.fullPath.endsWith('/' + fullMatch)) ||
              (s.path && s.path.endsWith('/' + fullMatch))
            );
            if (foundSrc) {
              hits.push({
                start: matchStart,
                end: matchEnd,
                source: foundSrc,
                name: fullMatch
              });
            }
          }
        }
        if (fnm.index === FILE_NAME_RE.lastIndex) FILE_NAME_RE.lastIndex++;
      }

      if (!hits.length) continue;

      hits.sort((a, b) => a.start - b.start);

      const nonOverlapping = [];
      let lastEnd = -1;
      for (const h of hits) {
        if (h.start >= lastEnd) {
          nonOverlapping.push(h);
          lastEnd = h.end;
        }
      }
      if (!nonOverlapping.length) continue;

      changed = true;
      const frag = document.createDocumentFragment();
      let last = 0;
      for (const h of nonOverlapping) {
        if (h.start > last) {
          frag.appendChild(document.createTextNode(text.slice(last, h.start)));
        }
        frag.appendChild(makeInlineChip(h.source, onopen, h.name));
        last = h.end;
      }
      if (last < text.length) {
        frag.appendChild(document.createTextNode(text.slice(last)));
      }
      node.parentNode.replaceChild(frag, node);
    }

    return changed;
  }

  // Svelte action: вешается на контейнер {@html renderMd(block.text)} и после
  // каждого обновления контента (в т.ч. стриминга) заново превращает упоминания
  // источников в чипсы. Собственные вставки не триггерят повторный проход.
  function inlineSourceChips(node, params) {
    let sources = [];
    let onopen = null;
    let ownFlag = false;

    const run = () => {
      ownFlag = true;
      if (!enhanceInlineSources(node, sources, onopen)) ownFlag = false;
    };

    const mo = new MutationObserver(() => {
      if (ownFlag) { ownFlag = false; return; }
      run();
    });
    mo.observe(node, { childList: true, subtree: true, characterData: true });

    const update = (p) => {
      sources = (p && p.sources) || [];
      onopen = (p && p.onopen) || null;
      run();
    };

    update(params);
    return {
      update,
      destroy() {
        mo.disconnect();
      },
    };
  }

  async function handleRework(m) {
    if (!m || !chat) return;
    const userText = (m.parts || [])
      .filter((p) => p.type === "text")
      .map((p) => p.text)
      .join("\n") || m.content || "";

    input = userText;
    if (userText.trim().startsWith("/generate")) {
      showImageToolpanel = true;
      loadAvailableLoras();
      syncTagsFromInput();
    }
    if (textareaEl) {
      textareaEl.value = userText;
    }
    await tick();
    if (textareaEl) {
      textareaEl.focus();
      resizeTextarea();
    }

    if (isStreaming) {
      try { chat.stop(); } catch (_) {}
      isStreaming = false;
      activePhaseStatus = "";
    }

    if (typeof chat.clearError === "function") {
      chat.clearError();
    }

    let idx = chat.messages.findIndex((item) => item.id === m.id);
    if (idx === -1) {
      idx = chat.messages.indexOf(m);
    }
    if (idx !== -1) {
      chat.state.setMessages(chat.messages.slice(0, idx));
      chat = chat;
      if (chatStore && chatId) {
        chatStore.purgeMessagesFrom(chatId, m.id, userText).catch(() => {});
      }
    }
  }

  // Rework карточки /learn|/recognize: удаляем карточку и весь чат до породившего
  // её юзер-cообщения, копируем сырой запрос (команда + путь) в композер.
  function handleCardRework(data) {
    if (!chat || !data) return;
    // Гостевой флоу: карточка могла быть отрисована раньше команды — удаляем её сразу.
    if (data.docId) {
      const list = chat.messages.filter((m) => !(m && m.role === "assistant" && (m.parts || []).some((p) => p.type === "data-imported" && (p.data?.docId || "") === data.docId)));
      if (list.length !== chat.messages.length) {
        chat.state.setMessages(list);
        chat = chat;
      }
    }
    let target = null;
    if (data.requestId) {
      target = chat.messages.find((m) => m && m.id === data.requestId);
    }
    if (!target && data.docId) {
      const idx = findLearnPromptIndex(data.docId);
      if (idx >= 0) target = chat.messages[idx];
    }
    if (target) {
      handleRework(target);
      return;
    }
    // Fallback: без найденного сообщения просто кладём запрос в композер.
    input = data.request || (data.docId ? `/learn ${data.docId} ` : "");
    if (input.trim().startsWith("/")) {
      if (typeof syncTagsFromInput === "function") syncTagsFromInput();
    }
    tick().then(() => {
      if (textareaEl) {
        textareaEl.value = input;
        textareaEl.focus();
        resizeTextarea();
      }
    });
  }

  async function handleDeleteMessage(m) {
    if (!m || !chat) return;
    let idx = chat.messages.findIndex((item) => item.id === m.id);
    if (idx === -1) {
      idx = chat.messages.indexOf(m);
    }
    if (idx !== -1) {
      const newMessages = [...chat.messages];
      newMessages.splice(idx, 1);
      chat.state.setMessages(newMessages);
      chat = chat;
      if (chatStore && chatId) {
        chatStore.deleteMessage(chatId, m.id).catch(() => {});
      }
    }
  }

  function getStatusBadge(st) {
    if (!st) return null;
    switch (st) {
      case "generating_image":
      case "generating":
      case "image":
        return { icon: "🌀", text: $_("generating_image", {}, "Creating image…"), cls: "generating" };
      case "thinking":
        return { icon: "💡", text: $_("thinking", {}, "Thinking…"), cls: "thinking" };
      case "summarizing":
      case "analyzing":
      case "synthesizing":
        return { icon: "📑", text: $_("summarizing", {}, "Analyzing sources…"), cls: "summarizing" };
      case "searching":
        return { icon: "🧭", text: $_("searching", {}, "Searching knowledge & web…"), cls: "searching" };
      case "learning":
        return { icon: "📚", text: $_("learning", {}, "Learning documents…"), cls: "learning" };
      case "recognizing":
        return { icon: "🖼️", text: $_("recognizing", {}, "Recognizing image…"), cls: "recognizing" };
      case "performing":
        return { icon: "⚡", text: $_("performing", {}, "Performing…"), cls: "performing" };
      case "executing":
        return { icon: "⚙️", text: $_("executing", {}, "Executing tool…"), cls: "executing" };
      case "processing":
      case "sending":
      case "submitted":
        return { icon: "✨", text: $_("processing", {}, "Just a moment…"), cls: "processing" };
      case "typing":
        return { icon: "✨", text: $_("typing", {}, "Typing…"), cls: "typing" };
      default:
        return { icon: "✨", text: $_("processing", {}, "Just a moment…"), cls: "processing" };
    }
  }
</script>

<svelte:window on:click={closeToolsMenu} on:keydown={(e) => e.key === "Escape" && (showToolsMenu = false, showPromptsPopup = false)} />

<div class="chat-container {isDarkTheme ? 'dark-theme' : 'light-theme'}">
  <!-- Conversation Scroll View -->
  <div class="messages-viewport" bind:this={messagesContainerEl} on:scroll={handleScroll}>
    {#if !chat || displayMessages.length === 0}
      <!-- Gemini Welcome Hero State -->
      <div class="gemini-hero">
        <!-- Assistant Persona Badge (Avatar on left, Name & Title on right, matching legacy screen) -->
        <div class="hero-assistant-badge">
          <!-- svelte-ignore a11y-click-events-have-key-events -->
          <!-- svelte-ignore a11y-no-static-element-interactions -->
          <div
            class="hero-avatar clickable"
            on:click={() => (showAvatarModal = true)}
            role="button"
            tabindex="0"
            title={$_("customize_appearance", {}, "Customize appearance and avatar")}
          >
            {#if assistantAvatarUrl}
              <img src={assistantAvatarUrl} alt={assistantName} class="avatar-hero-img" on:error={handleAvatarError} />
            {:else}
              <img src="/img/june.png" alt={assistantName} class="avatar-hero-img" on:error={handleAvatarError} />
            {/if}
          </div>
          <div class="hero-assistant-meta">
            <h2 class="hero-assistant-name">{assistantName}</h2>
            <span class="hero-assistant-title">{assistantTitle}</span>
          </div>
        </div>

        <h1 class="hero-greeting">
          <span class="gradient-text">
            {userName ? `${$_("hello", {}, "Hello")}, ${userName}` : $_("welcome_to_chat", {}, "Welcome to OnMyChat")}
          </span>
        </h1>
        <p class="hero-sub">
          {$_("hero_subtext", {}, "How can I help you today with your private documents and ideas?")}
        </p>

        <!-- Suggestion Prompt Cards -->
        <div class="suggestion-grid">
          <button
            class="suggestion-card"
            on:click={() => setQuickPrompt($_("prompt_rag_sample", {}, "Explain how RAG is implemented in OnMyChat?"))}
          >
            <div class="suggestion-icon">💡</div>
            <div class="suggestion-text">
              <strong>{$_("prompt_rag_title", {}, "Explore RAG Knowledge")}</strong>
              <span>{$_("prompt_rag_desc", {}, "Explain how private document retrieval works")}</span>
            </div>
          </button>

          <button
            class="suggestion-card"
            on:click={() => setQuickPrompt($_("prompt_doc_sample", {}, "Summarize the key decisions and notes from my indexed files"))}
          >
            <div class="suggestion-icon">📄</div>
            <div class="suggestion-text">
              <strong>{$_("prompt_doc_title", {}, "Summarize Documents")}</strong>
              <span>{$_("prompt_doc_desc", {}, "Extract facts and key takeaways")}</span>
            </div>
          </button>

          <button
            class="suggestion-card"
            on:click={() => setQuickPrompt($_("prompt_p2p_sample", {}, "How does P2P inference preserve my data privacy?"))}
          >
            <div class="suggestion-icon">🛡️</div>
            <div class="suggestion-text">
              <strong>{$_("prompt_p2p_title", {}, "Private & Decentralized")}</strong>
              <span>{$_("prompt_p2p_desc", {}, "Data never leaves your sovereign control")}</span>
            </div>
          </button>

          <button
            class="suggestion-card"
            on:click={() => setQuickPrompt($_("prompt_draft_sample", {}, "Help me draft a concise project review update for the team"))}
          >
            <div class="suggestion-icon">✍️</div>
            <div class="suggestion-text">
              <strong>{$_("prompt_draft_title", {}, "Draft & Collaborate")}</strong>
              <span>{$_("prompt_draft_desc", {}, "Create outlines, letters, and reviews")}</span>
            </div>
          </button>
        </div>
      </div>
    {:else}
      <!-- Active Message Thread -->
      <div class="conversation-thread">
        {#each displayMessages as m (m.id)}
          {@const blocks = buildBlocks(m)}
          <div class="thread-row {m.role}" data-mid={m.id}>
            {#if m.role === "assistant"}
              <!-- Assistant Message (Gemini style: borderless, airy, circular June avatar on left) -->
              <div class="assistant-entry">
                <!-- svelte-ignore a11y-click-events-have-key-events -->
                <!-- svelte-ignore a11y-no-static-element-interactions -->
                <div
                  class="entry-avatar clickable"
                  on:click={() => (showAvatarModal = true)}
                  role="button"
                  tabindex="0"
                  title={$_("customize_appearance", {}, "Customize appearance and avatar")}
                >
                  {#if assistantAvatarUrl}
                    <img src={assistantAvatarUrl} alt={assistantName} class="avatar-chat-img" on:error={handleAvatarError} />
                  {:else}
                    <img src="/img/june.png" alt={assistantName} class="avatar-chat-img" on:error={handleAvatarError} />
                  {/if}
                </div>

                <div class="entry-content">
                  <div class="entry-header">
                    <span class="assistant-name">{assistantName}</span>
                    <span class="assistant-tag">{assistantTitle}</span>
                  </div>

                  {#each blocks as block, bi}
                    {#if block.kind === "reasoning"}
                      <details
                        class="gemini-reasoning"
                        open={expandedReasoning[m.id] !== undefined ? expandedReasoning[m.id] : false}
                        on:toggle={(e) => {
                          expandedReasoning[m.id] = e.currentTarget.open;
                        }}
                      >
                        <summary>
                          <span class="reasoning-summary-left">
                            <span class="reasoning-icon">💡</span>
                            <span class="reasoning-title">
                              {#if isStreaming && isNewTurnMsg(m) && activePhaseStatus === "thinking"}
                                {$_("thinking", {}, "Thinking…")}
                              {:else}
                                {$_("thinking_process", {}, "Thinking process")}
                              {/if}
                            </span>
                            {#if isStreaming && isNewTurnMsg(m) && activePhaseStatus === "thinking"}
                              <span class="typing-dots">
                                <span></span><span></span><span></span>
                              </span>
                            {/if}
                          </span>
                          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" class="accordion-chevron">
                            <polyline points="6 9 12 15 18 9" />
                          </svg>
                        </summary>
                        <div class="reasoning-inner markdown-body">
                          {@html renderMd(block.text)}
                        </div>
                      </details>
                    {:else if block.kind === "plan"}
                      <details
                        class="plan-card"
                        class:running={block.isRunning}
                        class:completed={!block.isRunning}
                        open={expandedPlan[m.id] !== undefined ? expandedPlan[m.id] : (block.isExplicit && block.isRunning)}
                        on:toggle={(e) => {
                          expandedPlan[m.id] = e.currentTarget.open;
                        }}
                      >
                        <summary class="plan-summary">
                          <span class="plan-summary-left">
                            {#if block.isRunning}
                              <span class="status-icon-bubble plan-icon executing">⚙️</span>
                              <span class="plan-title">{$_("executing", {}, "Executing...")}</span>
                              <span class="typing-dots">
                                <span></span><span></span><span></span>
                              </span>
                            {:else}
                              <span class="plan-header-checkbox completed">
                                <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round">
                                  <polyline points="20 6 9 17 4 12" />
                                </svg>
                              </span>
                              <span class="plan-title">
                                {#if block.steps.length > 0 && block.steps.every(s => s.status === 'completed')}
                                  {$_("completed", {}, "Completed")}
                                {:else}
                                  {$_("execution_plan", {}, "Execution")}
                                {/if}
                              </span>
                              {#if block.steps.length > 0}
                                <span class="plan-counter">({block.steps.filter(s => s.status === 'completed').length}/{block.steps.length})</span>
                              {/if}
                            {/if}
                          </span>
                          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" class="accordion-chevron">
                            <polyline points="6 9 12 15 18 9" />
                          </svg>
                        </summary>
                        <div class="plan-inner">
                          <div class="plan-steps-list">
                            {#each block.steps as step, si}
                              <div
                                class="plan-step-item"
                                class:completed={step.status === "completed"}
                                class:running={step.status === "running"}
                              >
                                <button
                                  type="button"
                                  class="step-checkbox {step.status}"
                                  title={step.status === "completed" ? $_("completed", {}, "Completed") : step.status === "running" ? $_("executing", {}, "Executing...") : $_("pending", {}, "Pending")}
                                  on:click|stopPropagation={() => {
                                    step.status = step.status === "completed" ? "pending" : "completed";
                                    block.steps = block.steps;
                                  }}
                                >
                                  {#if step.status === "completed"}
                                    <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round">
                                      <polyline points="20 6 9 17 4 12" />
                                    </svg>
                                  {:else if step.status === "running"}
                                    <span class="step-mini-spinner"></span>
                                  {/if}
                                </button>
                                <div
                                  class="step-text markdown-body"
                                  use:inlineSourceChips={{ sources: getMessageSources(m), onopen: (src) => handleSourceClick(src, m) }}
                                >
                                  {@html renderMd(step.content)}
                                </div>
                              </div>
                            {/each}
                          </div>
                        </div>
                      </details>
                    {:else if block.kind === "md"}
                      <div
                        class="markdown-body"
                        use:inlineSourceChips={{ sources: getMessageSources(m), onopen: (src) => handleSourceClick(src, m) }}
                      >
                        {@html renderMd(block.text)}
                      </div>
                    {:else if block.kind === "facts"}
                      <details class="rag-facts-accordion" open={false}>
                        <summary>
                          <span class="facts-summary-left">
                            <span class="facts-icon">📚</span>
                            <span class="facts-title">{$_("known_facts", {}, "Known facts")}</span>
                          </span>
                          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" class="accordion-chevron">
                            <polyline points="6 9 12 15 18 9" />
                          </svg>
                        </summary>
                        <div class="facts-inner markdown-body">
                          {@html renderMd(sanitizeFactsMarkdown(block.facts))}
                        </div>
                      </details>
                    {:else if block.kind === "new_fact"}
                      <details class="rag-facts-accordion new-fact" open={true}>
                        <summary>
                          <span class="facts-summary-left">
                            <span class="facts-icon">📌</span>
                            <span class="facts-title">{$_("new_fact", {}, "New fact")}</span>
                          </span>
                          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" class="accordion-chevron">
                            <polyline points="6 9 12 15 18 9" />
                          </svg>
                        </summary>
                        <div class="facts-inner markdown-body">
                          {@html renderMd(sanitizeFactsMarkdown(block.fact))}
                        </div>
                      </details>
                    {:else if block.kind === "knowledge"}
                      <div class="knowledge-card">
                        <div class="knowledge-header">
                          <span>📌 {$_("new_fact_learned", {}, "New knowledge captured")}</span>
                        </div>
                        <div class="knowledge-body">
                          {block.data.fact || block.data.knowledge}
                        </div>
                      </div>
                    {:else if block.kind === "question"}
                      {@const answered = answeredActions.has("q:" + (block.data?.requestID || ""))}
                      <div class="interactive-action-card">
                        <p class="action-title">{block.data?.question}</p>
                        <div class="action-buttons">
                          {#each block.data?.options || [] as opt}
                            {@const val = typeof opt === "string" ? opt : opt?.label || JSON.stringify(opt)}
                            <button
                              class="action-choice-btn"
                              disabled={answered}
                              on:click={() => answerQuestion(block.data.requestID, val)}
                            >
                              {typeof opt === "string" ? opt : opt?.label}
                            </button>
                          {/each}
                          <button
                            class="action-reject-btn"
                            disabled={answered}
                            on:click={() => rejectQuestion(block.data.requestID)}
                          >
                            {$_("reject", {}, "Reject")}
                          </button>
                          {#if answered}
                            <span class="action-submitted">✓ {$_("sent", {}, "Sent")}</span>
                          {/if}
                        </div>
                      </div>
                    {:else if block.kind === "permission"}
                      {@const pid = block.data?.permissionID || ""}
                      {@const answered = answeredActions.has("p:" + pid)}
                      <div class="interactive-action-card perm-card">
                        <p class="action-title">
                          🛡️ {block.data?.permission?.title || $_("permission_requested", {}, "Permission requested")}
                        </p>
                        {#if block.data?.permission?.patterns?.length}
                          <code class="perm-code">{block.data.permission.patterns.join(", ")}</code>
                        {/if}
                        <div class="action-buttons">
                          <button
                            class="action-allow-btn"
                            disabled={answered}
                            on:click={() => answerPermission(pid, true)}
                          >
                            {$_("allow", {}, "Allow")}
                          </button>
                          <button
                            class="action-deny-btn"
                            disabled={answered}
                            on:click={() => answerPermission(pid, false)}
                          >
                            {$_("deny", {}, "Deny")}
                          </button>
                          {#if answered}
                            <span class="action-submitted">✓ {$_("submitted", {}, "Submitted")}</span>
                          {/if}
                        </div>
                      </div>
                    {:else if block.kind === "image"}
                      {@const imgKey = block.data.path || block.data.url}
                      {@const isFailed = (imgLoadingState[imgKey] === 'error' || (p2pImgFailed[imgKey] && !imageBlobCache[imgKey] && imgLoadingState[imgKey] !== 'loading')) && !regeneratingImages[imgKey]}
                      {@const imgSrc = !isFailed ? getImageSrc(block.data, imgLoadingState, p2pImgFailed, imageBlobCache) : ""}
                      <div class="chat-image-card">
                        <div
                          class="chat-image-wrap"
                          class:has-error={isFailed}
                          role="button"
                          tabindex="0"
                          on:click={() => {
                            if (!isFailed) openChatGallery(block.data.path);
                          }}
                          on:keydown={(e) => {
                            if (e.key === "Enter" && !isFailed) openChatGallery(block.data.path);
                          }}
                        >
                          {#if imgSrc && !isFailed && !regeneratingImages[imgKey]}
                            <img
                              src={imgSrc}
                              alt={block.data.title || "Generated image"}
                              class="chat-generated-img"
                              on:error={(e) => handleImageSrcError(block.data, e)}
                              on:load={(e) => {
                                // /p2p-путь означает: файл реально лежит на
                                // устройстве — hint о временной картинке снимаем.
                                const src = e?.target?.src || "";
                                if (src.includes("/p2p/")) markImageSaved(block.data);
                              }}
                            />
                          {:else}
                            <div class="chat-image-placeholder" class:has-error={isFailed}>
                              {#if isFailed}
                                <div class="another-device-placeholder">
                                  <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.8" class="another-device-svg">
                                    <circle cx="12" cy="12" r="10" />
                                    <polyline points="12 6 12 12 16 14" />
                                  </svg>
                                  <span class="another-device-hint">{$_("image_expired", {}, "Image expired")}</span>
                                </div>
                              {:else}
                                <img src="/img/loader.svg" alt="Loading..." class="chat-placeholder-svg spinner-pulse" />
                                <span class="chat-placeholder-text">
                                  {regeneratingImages[imgKey] ? $_("regenerating_image", {}, "Regenerating image…") : $_("loading_image", {}, "Loading image...")}
                                </span>
                              {/if}
                            </div>
                          {/if}
                        </div>
                        <div class="chat-image-meta">
                          {#if block.data.title}
                            <div class="chat-image-title">{block.data.title}</div>
                          {/if}
                          {#if block.data.description}
                            <div class="chat-image-description">{block.data.description}</div>
                          {/if}
                          {#if block.data.prompt}
                            <details class="chat-image-prompt-details">
                              <summary class="chat-image-prompt-summary">
                                <span>{$_("image_prompt", {}, "Prompt")}</span>
                                <button
                                  type="button"
                                  class="copy-prompt-btn"
                                  title={$_("copy_prompt", {}, "Copy prompt")}
                                  on:click|stopPropagation={() => copyPrompt(formatDisplayPrompt(block.data.prompt), m.id)}
                                >
                                  {#if copiedPromptId === m.id}
                                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="var(--nord14)" stroke-width="2">
                                      <polyline points="20 6 9 17 4 12" />
                                    </svg>
                                    <span style="color: var(--nord14); font-size: 11px;">{$_("prompt_copied", {}, "Copied")}</span>
                                  {:else}
                                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                                      <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
                                      <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
                                    </svg>
                                  {/if}
                                </button>
                              </summary>
                              <div class="chat-image-prompt-text">{formatDisplayPrompt(block.data.prompt)}</div>
                            </details>
                          {/if}
                          <div class="chat-image-actions">
                            {#if !isFailed}
                              <button
                                type="button"
                                class="image-action-btn"
                                title={$_("download_image", {}, "Download image")}
                                on:click={() => downloadImage(block.data)}
                              >
                                <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                  <polyline points="7 10 12 15 17 10" />
                                  <line x1="12" y1="15" x2="12" y2="3" />
                                </svg>
                                <span>{$_("download_image", {}, "Download")}</span>
                              </button>
                            {/if}
                            <button
                              type="button"
                              class="image-action-btn"
                              title={$_("regenerate", {}, "Regenerate")}
                              disabled={!!regeneratingImages[imgKey]}
                              on:click={() => regenerateImage(block.data, m.id)}
                            >
                              <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" class:spin={!!regeneratingImages[imgKey]}>
                                <path d="M21.5 2v6h-6M2.5 22v-6h6M2 11.5a10 10 0 0 1 18.8-4.3M22 12.5a10 10 0 0 1-18.8 4.2" />
                              </svg>
                              <span>{regeneratingImages[imgKey] ? $_("regenerating", {}, "Regenerating…") : $_("regenerate", {}, "Regenerate")}</span>
                            </button>
                          </div>
                          {#if !isFailed && tempShownImages[imgKey] && !savedToDevice[imgKey] && !regeneratingImages[imgKey] && !isStorageOnAiNode()}
                            <div class="chat-image-warning-banner">
                              <div class="chat-image-warning-info">
                                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                                  <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z" />
                                  <line x1="12" y1="9" x2="12" y2="13" />
                                  <line x1="12" y1="17" x2="12.01" y2="17" />
                                </svg>
                                <span>{$_("image_p2p_save_failed_warning", {}, "Image could not be saved to your device via P2P. Select a storage folder to save creations.")}</span>
                              </div>
                              <button
                                type="button"
                                class="chat-image-warning-btn"
                                on:click={() => handleChooseFolderForImage(block.data)}
                              >
                                <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                                  <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
                                </svg>
                                <span>{$_("choose_folder", {}, "Choose folder")}</span>
                              </button>
                            </div>
                          {/if}
                        </div>
                      </div>
                    {:else if block.kind === "imported"}
                      <div class="import-result-card" class:loading={block.data?.loading}>
                        <div class="import-result-head">
                          <span class="import-result-icon">{block.data?.loading ? "⏳" : (block.data?.isImage ? "🖼️" : "📚")}</span>
                          <span class="import-result-title">{block.data?.title || $_("imported_document", {}, "Imported document")}</span>
                          {#if block.data?.loading}
                            <span class="import-result-badge learning">{$_("learning", {}, "learning")}</span>
                          {:else if block.data?.docId}
                            <button
                              type="button"
                              role="switch"
                              aria-checked={isAttachedActive(block.data.docId)}
                              class="attach-switch {isAttachedActive(block.data.docId) ? 'on' : ''}"
                              on:click={() => toggleAttached(block.data.docId)}
                              title={$_("attach_doc_hint", {}, "Attach this document to all further answers")}
                            >
                              <span class="attach-switch-track">
                                <span class="attach-switch-knob"></span>
                              </span>
                              <span class="attach-switch-label">
                                {isAttachedActive(block.data.docId)
                                  ? $_("attach_doc", {}, "Attached to chat")
                                  : $_("attach_doc_off", {}, "Attach to chat")}
                              </span>
                            </button>
                          {:else}
                            <span class="import-result-badge">imported</span>
                          {/if}
                        </div>
                        {#if block.data?.loading}
                          <div class="import-result-loading">
                            <span class="import-spinner"></span>
                            <span>{$_("learning_placeholder", {}, "Indexing the document and building the card…")}</span>
                          </div>
                        {:else}
                          {@const impDoc = block.data?.docId ? String(block.data.docId).replace(/^\/+/, "") : ""}
                          {@const impWantsImage = isDeviceImageCard(block.data) || !!(block.data?.imagePreview)}
                          {@const devSrc = isDeviceImageCard(block.data) ? deviceImageSrc(block.data) : (block.data?.imagePreview || "")}
                          {@const impFailed = Boolean(
                            impWantsImage && (
                              !devSrc ||
                              (impDoc && (importedImgFailed[impDoc] || importedImgFailed["/" + impDoc])) ||
                              (block.data?.docId && importedImgFailed[block.data.docId]) ||
                              (block.data?.id && importedImgFailed[block.data.id]) ||
                              (block.data?.title && importedImgFailed[block.data.title]) ||
                              (block.data?.imagePreview && importedImgFailed[block.data.imagePreview])
                            )
                          )}
                          {#if impFailed}
                            <div class="import-result-image-wrap image-on-another-device">
                              <div class="another-device-placeholder">
                                <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.8" class="another-device-svg">
                                  <rect x="2" y="3" width="20" height="14" rx="2" ry="2" />
                                  <line x1="8" y1="21" x2="16" y2="21" />
                                  <line x1="12" y1="17" x2="12" y2="21" />
                                </svg>
                                <span class="another-device-hint">{$_("on_another_device", {}, "(on another device)")}</span>
                              </div>
                            </div>
                          {:else if devSrc}
                            {#if isDeviceImageCard(block.data)}
                              <div
                                class="import-result-image-wrap clickable-image"
                                role="button"
                                tabindex="0"
                                on:click={() => openChatGallery(block.data.docId)}
                                on:keydown={(e) => e.key === "Enter" && openChatGallery(block.data.docId)}
                                title={$_("open_chat_gallery", {}, "Open in chat gallery")}
                              >
                                <img
                                  class="import-result-image"
                                  src={devSrc}
                                  alt={block.data?.title || "Image"}
                                  loading="lazy"
                                  on:error={(e) => markImportedImageFailed(block.data, e)}
                                />
                                <span class="import-result-image-zoom">⛶</span>
                              </div>
                            {:else}
                              <div class="import-result-image-wrap">
                                <img
                                  class="import-result-image"
                                  src={devSrc}
                                  alt={block.data?.title || "Image"}
                                  loading="lazy"
                                  on:error={(e) => markImportedImageFailed(block.data, e)}
                                />
                              </div>
                            {/if}
                          {/if}
                          {#if block.data?.annotation}
                            <div class="import-result-annotation">{@html renderMd(block.data.annotation)}</div>
                          {/if}
                          {#if block.data?.tags?.length}
                            <div class="import-result-tags">
                              {#each block.data.tags as t}
                                <span class="imported-tag-chip">#{t}</span>
                              {/each}
                            </div>
                          {/if}
                          {#if block.data?.request}
                            <details class="import-request-block">
                              <summary class="import-request-summary">
                                <span class="import-request-label">
                                  <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/></svg>
                                  {$_(block.data.command === "recognize" ? "orig_request_recognize" : "orig_request", {}, block.data.command === "recognize" ? "Original request (recognize)" : "Original request (learn)")}
                                </span>
                                <button
                                  type="button"
                                  class="card-rework-btn"
                                  on:click|preventDefault|stopPropagation={() => handleCardRework(block.data)}
                                  title={$_("rework", {}, "Rework message")}
                                >
                                  <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                                    <polyline points="1 4 1 10 7 10" />
                                    <path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10" />
                                  </svg>
                                  <span>{$_("rework", {}, "Rework")}</span>
                                </button>
                              </summary>
                              <pre class="import-request-text">{block.data.request}</pre>
                            </details>
                          {/if}
                        {/if}
                      </div>
                    {/if}
                  {/each}

                  {#if isStreaming && m === displayMessages[displayMessages.length - 1] && isNewTurnMsg(m) && !blocks.some((b) => b.kind === "md" || b.kind === "reasoning" || b.kind === "plan" || b.kind === "status")}
                    {@const badge = getStatusBadge(activePhaseStatus || "processing")}
                    {#if badge}
                      {#if badge.cls === "thinking"}
                        <button
                          type="button"
                          class="active-stream-status thinking clickable"
                          title={$_("thinking_process", {}, "Thinking process")}
                          on:click={() => {
                            expandedReasoning[m.id] = !expandedReasoning[m.id];
                          }}
                        >
                          <span class="status-icon-bubble">💡</span>
                          <span class="stream-status-text">{badge.text}</span>
                          <span class="typing-dots">
                            <span></span><span></span><span></span>
                          </span>
                          <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2" class="accordion-chevron" class:rotated={expandedReasoning[m.id]}>
                            <polyline points="6 9 12 15 18 9" />
                          </svg>
                        </button>
                        {#if expandedReasoning[m.id]}
                          <div class="gemini-reasoning active-expanded">
                            <div class="reasoning-inner markdown-body">
                              <span class="reasoning-waiting-hint">💡 {$_("thinking", {}, "Thinking…")}</span>
                            </div>
                          </div>
                        {/if}
                      {:else}
                        <div class="active-stream-status {badge.cls}">
                          <span class="status-icon-bubble">{badge.icon}</span>
                          <span class="stream-status-text">{badge.text}</span>
                          <span class="typing-dots">
                            <span></span><span></span><span></span>
                          </span>
                        </div>
                      {/if}
                    {/if}
                  {/if}

                  <!-- Message Action Toolbar (only show once message has content and is done streaming) -->
                  {#if (!isStreaming || m !== chat.messages[chat.messages.length - 1]) && blocks.some((b) => b.kind === "md")}
                    <div class="message-footer-tools">
                      {#if getMessageSources(m).length > 0}
                        <button
                          type="button"
                          class="tool-btn sources-toggle-btn"
                          class:active={expandedSources[m.id]}
                          title={$_("sources", {}, "Sources")}
                          on:click={() => toggleSources(m.id)}
                        >
                          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                            <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71" />
                            <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71" />
                          </svg>
                          <span>{$_("sources", {}, "Sources")} ({getMessageSources(m).length})</span>
                          <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2" class="sources-chevron" class:rotated={expandedSources[m.id]}>
                            <polyline points="6 9 12 15 18 9" />
                          </svg>
                        </button>
                      {/if}

                      <!-- TTS Speak / Stop Button -->
                      <button
                        type="button"
                        class="tool-btn tts-btn"
                        class:active={$ttsState.activeId === m.id && $ttsState.state === 'playing'}
                        class:loading={$ttsState.activeId === m.id && $ttsState.state === 'loading'}
                        title={$ttsState.activeId === m.id && $ttsState.state === 'playing' ? $_("stop_reading", {}, "Stop reading") : $_("read_aloud", {}, "Read aloud")}
                        on:click={() => {
                          const fullText = (m.parts || []).filter((p) => p.type === "text").map((p) => p.text || "").join("\n");
                          ttsPlayer.playMessage(m.id, fullText, httpDrive);
                        }}
                      >
                        {#if $ttsState.activeId === m.id && $ttsState.state === 'loading'}
                          <span class="tts-spinner"></span>
                        {:else if $ttsState.activeId === m.id && $ttsState.state === 'playing'}
                          <div class="tts-equalizer">
                            <span class="bar bar-1"></span>
                            <span class="bar bar-2"></span>
                            <span class="bar bar-3"></span>
                          </div>
                        {:else}
                          <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                            <polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5" />
                            <path d="M15.54 8.46a5 5 0 0 1 0 7.07" />
                            <path d="M19.07 4.93a10 10 0 0 1 0 14.14" />
                          </svg>
                        {/if}
                      </button>

                      <button
                        type="button"
                        class="tool-btn"
                        title={$_("copy", {}, "Copy message")}
                        on:click={() => {
                          const fullText = (m.parts || []).filter((p) => p.type === "text").map((p) => p.text || "").join("\n");
                          copyText(fullText, m.id);
                        }}
                      >
                        {#if copiedId === m.id}
                          <span class="copied-label">✓ {$_("copied", {}, "Copied!")}</span>
                        {:else}
                          <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                            <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
                            <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
                          </svg>
                        {/if}
                      </button>

                      <button
                        type="button"
                        class="tool-btn delete-btn"
                        title={$_("remove", {}, "Delete")}
                        on:click={() => handleDeleteMessage(m)}
                      >
                        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                          <polyline points="3 6 5 6 21 6" />
                          <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                        </svg>
                      </button>
                    </div>
                  {/if}

                  <!-- Expanded Sources Drawer (RAG results) -->
                  {#if expandedSources[m.id] && getMessageSources(m).length > 0}
                    <div class="rag-sources-expanded">
                      <div class="sources-grid">
                        {#each getMessageSources(m) as src}
                          {@const srcPreview = sourcePreviewUrl(src)}
                          {@const srcPreviewKey = sourcePreviewKey(src)}
                          {@const previewFailed = !!(srcPreviewKey && sourcePreviewFailed[srcPreviewKey])}
                          <button
                            type="button"
                            class="source-card"
                            class:static={!isSourceAccessible(src)}
                            title={isSourceAccessible(src) ? cleanSourcePath(src.path || src.fullPath || src.title) : (src.title || (src.path ? cleanSourcePath(src.path).split("/").pop() : src.url))}
                            disabled={!isSourceAccessible(src)}
                            on:click={() => handleSourceClick(src, m)}
                          >
                            <div class="source-card-icon" class:has-preview={!!srcPreview && !previewFailed}>
                              {#if srcPreview && !previewFailed}
                                <img
                                  class="source-card-preview"
                                  src={srcPreview}
                                  alt=""
                                  loading="lazy"
                                  on:error={() => markSourcePreviewFailed(src)}
                                />
                              {:else}
                                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                                  <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
                                  <polyline points="14 2 14 8 20 8" />
                                  <line x1="16" y1="13" x2="8" y2="13" />
                                  <line x1="16" y1="17" x2="8" y2="17" />
                                </svg>
                              {/if}
                            </div>
                            <div class="source-card-info">
                              <span class="source-card-title">{src.title || (src.path ? cleanSourcePath(src.path).split("/").pop() : src.url)}</span>
                              {#if (isPrivateMode || isOwnerMode) && (src.owner || src.mimetype)}
                                <span class="source-card-sub">{src.owner || src.mimetype}</span>
                              {/if}
                            </div>
                          </button>
                        {/each}
                      </div>
                    </div>
                  {/if}
                </div>
              </div>
            {:else if hiddenCommandIds.has(m.id)}
              <!-- Скрытый промпт /learn|/recognize: оригинальный запрос живёт в карточке -->
            {:else}
              <!-- User Message: Sleek elevated pill on the right -->
              <div class="user-bubble-wrapper">
                <div class="user-bubble">
                  {#each m.parts || [] as part}
                    {#if part.type === "text"}
                      <div class="user-text">{part.text}</div>
                    {/if}
                  {/each}
                </div>
                <!-- User message tools (Rework / Copy) -->
                <div class="user-tools">
                  <button
                    type="button"
                    class="tool-btn user-rework-btn"
                    title={$_("rework", {}, "Rework message")}
                    on:click={() => handleRework(m)}
                  >
                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                      <polyline points="1 4 1 10 7 10" />
                      <path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10" />
                    </svg>
                    <span>{$_("rework", {}, "Rework")}</span>
                  </button>
                  <button
                    type="button"
                    class="tool-btn user-copy-btn"
                    title={$_("copy", {}, "Copy")}
                    on:click={() => {
                      const txt = (m.parts || []).filter((p) => p.type === "text").map((p) => p.text).join("\n");
                      copyText(txt, m.id);
                    }}
                  >
                    {#if copiedId === m.id}
                      <span class="copied-label">✓</span>
                    {:else}
                      <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                        <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
                        <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
                      </svg>
                    {/if}
                  </button>
                </div>
              </div>
            {/if}
          </div>
        {/each}

        {#if chat.error}
          <div class="chat-error-banner">
            <span class="error-icon">⚠️</span>
            <span>{chat.error.message || String(chat.error)}</span>
          </div>
        {/if}
      </div>
    {/if}
  </div>

  <!-- Bottom Floating Gemini Composer -->
  <div class="composer-outer" bind:clientHeight={composerHeight}>
    <div class="composer-container">
      <!-- Prompts History Popup -->
      {#if showPromptsPopup}
        <div class="prompts-popup" role="dialog" tabindex="-1">
          <div class="prompts-popup-header">
            <div class="prompts-header-title">
              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                <circle cx="12" cy="12" r="10" />
                <polyline points="12 6 12 12 16 14" />
              </svg>
              <span>{$_("image_prompt_history", {}, "Prompt History")}</span>
            </div>
            <button
              type="button"
              class="prompts-popup-close"
              on:click={() => (showPromptsPopup = false)}
            >
              <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.5">
                <line x1="18" y1="6" x2="6" y2="18" />
                <line x1="6" y1="6" x2="18" y2="18" />
              </svg>
            </button>
          </div>

          {#if recentPrompts.length > 5}
            <div class="prompts-search-wrap">
              <input
                type="text"
                class="prompts-search-input"
                placeholder={$_("search", {}, "Search prompts...")}
                bind:value={promptsSearchQuery}
              />
            </div>
          {/if}

          <div class="prompts-popup-list">
            {#each filteredPrompts as p}
              <div class="prompt-item" on:click={() => selectPrompt(p)}>
                <div class="prompt-item-content">
                  <div class="prompt-item-text">{p.text}</div>
                  <div class="prompt-item-meta">
                    {#if p.style}
                      <span class="prompt-style-badge">{p.style}</span>
                    {/if}
                    <span class="prompt-time">{formatPromptTime(p.lastUsed || p.created)}</span>
                  </div>
                </div>
                <button
                  type="button"
                  class="prompt-delete-btn"
                  title={$_("delete", {}, "Delete")}
                  on:click={(e) => handleDeletePrompt(e, p)}
                >
                  <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                    <polyline points="3 6 5 6 21 6" />
                    <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                  </svg>
                </button>
              </div>
            {:else}
              <div class="prompts-empty-state">
                <svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor" stroke-width="1.5">
                  <circle cx="12" cy="12" r="10" />
                  <polyline points="12 6 12 12 16 14" />
                </svg>
                <span>{$_("no_recent_prompts", {}, "No recent prompts")}</span>
              </div>
            {/each}
          </div>
        </div>
      {/if}

      <!-- Image Generator Toolpanel -->
      {#if showImageToolpanel}
        <div class="image-generator-panel">
          <!-- Row 1: Painting styles & Toggles -->
          <div class="gen-panel-row gen-panel-main-row">
            <div class="gen-styles-group">
              <span class="gen-section-label">{$_("painting_style", {}, "Painting style")}:</span>
              <div class="gen-styles-pills">
                {#each paintingStyles as style}
                  <button
                    type="button"
                    class="gen-style-pill"
                    class:active={isStyleActive(style)}
                    on:click={() => selectPaintingStyle(style)}
                  >
                    {$_(style, {}, style)}
                  </button>
                {/each}
              </div>
            </div>

            <div class="gen-panel-controls">
              <!-- With characters toggle -->
              <label class="gen-toggle-btn" class:active={withCharacters}>
                <input
                  type="checkbox"
                  checked={withCharacters}
                  on:change={toggleWithCharacters}
                  class="gen-checkbox-hidden"
                />
                <span class="gen-toggle-check">
                  {#if withCharacters}
                    <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="3">
                      <polyline points="20 6 9 17 4 12" />
                    </svg>
                  {/if}
                </span>
                <span class="gen-toggle-text">{$_("with_characters", {}, "With characters")}</span>
              </label>

              <!-- Continue generating toggle -->
              <label class="gen-toggle-btn" class:active={continueGenerating}>
                <input
                  type="checkbox"
                  checked={continueGenerating}
                  on:change={toggleContinueGenerating}
                  class="gen-checkbox-hidden"
                />
                <span class="gen-toggle-check">
                  {#if continueGenerating}
                    <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="3">
                      <polyline points="20 6 9 17 4 12" />
                    </svg>
                  {/if}
                </span>
                <span class="gen-toggle-text">{$_("continue_generating", {}, "Continue")}</span>
              </label>

              <!-- Prompts History Button -->
              <button
                type="button"
                class="gen-icon-btn prompts-history-btn"
                class:active={showPromptsPopup}
                title={$_("image_prompt_history", {}, "Prompt history")}
                on:click|stopPropagation={() => (showPromptsPopup = !showPromptsPopup)}
              >
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                  <circle cx="12" cy="12" r="10" />
                  <polyline points="12 6 12 12 16 14" />
                </svg>
              </button>

              <!-- Close Generator Button -->
              <button
                type="button"
                class="gen-icon-btn gen-close-btn"
                title={$_("close", {}, "Close")}
                on:click={closeImageGenerator}
              >
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.5">
                  <line x1="18" y1="6" x2="6" y2="18" />
                  <line x1="6" y1="6" x2="18" y2="18" />
                </svg>
              </button>
            </div>
          </div>

          <!-- Row 2: Characters & Character Styles (Subpanel) -->
          {#if withCharacters}
            <div class="gen-panel-row gen-panel-loras-row">
              <div class="gen-loras-group">
                <span class="gen-section-label">{$_("characters", {}, "Characters")}:</span>
                <div class="gen-lora-chips">
                  {#each availableLoras.filter((l) => l.type === "character" && (contentMode === "fun" || l.mode !== "fun")) as lora}
                    <button
                      type="button"
                      class="gen-lora-chip"
                      class:active={isLoraActive(lora.name)}
                      on:click={() => toggleLoraChip(lora.name)}
                    >
                      {lora.name}
                    </button>
                  {/each}
                </div>
              </div>

              {#if availableLoras.some((l) => l.type === "style" && (contentMode === "fun" || l.mode !== "fun"))}
                <span class="gen-divider"></span>
                <div class="gen-loras-group">
                  <span class="gen-section-label">{$_("character_styles", {}, "Styles")}:</span>
                  <div class="gen-lora-chips">
                    {#each availableLoras.filter((l) => l.type === "style" && (contentMode === "fun" || l.mode !== "fun")) as lora}
                      <button
                        type="button"
                        class="gen-lora-chip"
                        class:active={isLoraActive(lora.name)}
                        on:click={() => toggleLoraChip(lora.name)}
                      >
                        {lora.name}
                      </button>
                    {/each}
                  </div>
                </div>
              {/if}
            </div>
          {/if}
        </div>
      {/if}

      <form class="composer-pill" on:submit|preventDefault={handleSend}>
        {#if showToolsMenu}
          <div class="tools-popup-menu" role="menu" tabindex="-1">
            <div class="tools-popup-header">
              <span class="tools-popup-title">{$_("commands", {}, "Commands")}</span>
            </div>
            <div class="tools-popup-list">
              <button
                type="button"
                class="tools-menu-item"
                on:click={() => applySlashCommand("/think")}
              >
                <div class="tool-icon-circle think">
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M9 18h6M10 22h4M12 2a7 7 0 0 0-7 7c0 2.38 1.19 4.47 3 5.74V17a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1v-2.26c1.81-1.27 3-3.36 3-5.74a7 7 0 0 0-7-7z" />
                  </svg>
                </div>
                <div class="tool-meta">
                  <div class="tool-title-row">
                    <span class="tool-name">{$_("think", {}, "Think and brainstorm")}</span>
                    <span class="tool-chip">/think</span>
                  </div>
                  <span class="tool-hint">{$_("think_hint", {}, "Multi-step reasoning mode")}</span>
                </div>
              </button>

              <button
                type="button"
                class="tools-menu-item"
                on:click={() => applySlashCommand("/explore")}
              >
                <div class="tool-icon-circle search">
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <circle cx="11" cy="11" r="8" />
                    <line x1="21" y1="21" x2="16.65" y2="16.65" />
                  </svg>
                </div>
                <div class="tool-meta">
                  <div class="tool-title-row">
                    <span class="tool-name">{$_("explore", {}, "Explore")}</span>
                    <span class="tool-chip">/explore</span>
                  </div>
                  <span class="tool-hint">{$_("explore_hint", {}, "Search your storage and knowledge base")}</span>
                </div>
              </button>

              <button
                type="button"
                class="tools-menu-item"
                on:click={handleLearnClick}
              >
                <div class="tool-icon-circle learn">
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20" />
                    <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z" />
                  </svg>
                </div>
                <div class="tool-meta">
                  <div class="tool-title-row">
                    <span class="tool-name">{$_("learn", {}, "Learn and explain")}</span>
                    <span class="tool-chip">/learn</span>
                  </div>
                  <span class="tool-hint">{$_("learn_hint", {}, "Index and vectorize documents into RAG memory")}</span>
                </div>
              </button>

              {#if assistantCapabilities?.vision}
                <button
                  type="button"
                  class="tools-menu-item"
                  on:click={handleRecognizeClick}
                >
                  <div class="tool-icon-circle recognize">
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z" />
                      <circle cx="12" cy="13" r="4" />
                    </svg>
                  </div>
                  <div class="tool-meta">
                    <div class="tool-title-row">
                      <span class="tool-name">{$_("recognize", {}, "Recognize the image")}</span>
                      <span class="tool-chip">/recognize</span>
                    </div>
                    <span class="tool-hint">{$_("recognize_hint", {}, "Visual analysis and OCR")}</span>
                  </div>
                </button>
              {/if}

              {#if assistantCapabilities?.image_generation}
                <button
                  type="button"
                  class="tools-menu-item"
                  on:click={() => applySlashCommand("/generate")}
                >
                  <div class="tool-icon-circle generate">
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                      <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
                      <circle cx="8.5" cy="8.5" r="1.5" />
                      <polyline points="21 15 16 10 5 21" />
                    </svg>
                  </div>
                  <div class="tool-meta">
                    <div class="tool-title-row">
                      <span class="tool-name">{$_("generate_image", {}, "Create image")}</span>
                      <span class="tool-chip">/generate</span>
                    </div>
                    <span class="tool-hint">{$_("generate_image_hint", {}, "AI image generation with ComfyUI")}</span>
                  </div>
                </button>
              {/if}
            </div>
          </div>
        {/if}

        <!-- Plus Action Button -->
        <div class="composer-action-wrap">
          <button
            type="button"
            class="composer-action-btn"
            class:active={showToolsMenu}
            title={$_("tools", {}, "Tools & Slash Commands")}
            on:click|stopPropagation={() => (showToolsMenu = !showToolsMenu)}
          >
            <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5" class="plus-icon" class:rotated={showToolsMenu}>
              <line x1="12" y1="5" x2="12" y2="19" />
              <line x1="5" y1="12" x2="19" y2="12" />
            </svg>
          </button>
        </div>

        <!-- Auto-expanding Textarea -->
        <textarea
          bind:this={textareaEl}
          bind:value={input}
          class="composer-textarea"
          rows="1"
          placeholder={showImageToolpanel ? $_("image_prompt_placeholder", {}, "Describe the image to generate...") : $_("ask_anything", {}, "Ask anything…")}
          on:input={onComposerInput}
          on:keydown={handleKeydown}
          disabled={!chat || isStreaming}
        ></textarea>

        <!-- Voice Stream Mode Toggle Button -->
        <button
          type="button"
          class="composer-voice-toggle-btn"
          class:active={$ttsState.voiceStreamEnabled}
          title={$ttsState.voiceStreamEnabled ? $_("voice_stream_on", {}, "Voice response: Active") : $_("voice_stream_off", {}, "Voice response: Off")}
          on:click={() => ttsPlayer.setVoiceStreamEnabled(!$ttsState.voiceStreamEnabled)}
        >
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" />
            <path d="M19 10v2a7 7 0 0 1-14 0v-2" />
            <line x1="12" y1="19" x2="12" y2="23" />
            <line x1="8" y1="23" x2="16" y2="23" />
          </svg>
        </button>

        <!-- Right Action Button: Send or Stop -->
        {#if isStreaming}
          <button
            type="button"
            class="composer-stop-btn"
            title={$_("stop", {}, "Stop generating")}
            on:click={stopGeneration}
          >
            <svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor">
              <rect x="6" y="6" width="12" height="12" rx="2" />
            </svg>
          </button>
        {:else}
          <button
            type="submit"
            class="composer-send-btn"
            class:active={input.trim().length > 0}
            disabled={!chat || !input.trim()}
            title={$_("send", {}, "Send message")}
          >
            <svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor">
              <path d="M2.01 21L23 12 2.01 3 2 10l15 2-15 2z" />
            </svg>
          </button>
        {/if}
      </form>

      <div class="composer-disclaimer">
        <span>{$_("chat_disclaimer", {}, "On My Chat · AI that keeps your data yours")}</span>
      </div>
    </div>
  </div>

  {#if sourceChooserMode}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="source-chooser-backdrop" on:click|self={() => (sourceChooserMode = "")}>
      <div class="source-chooser">
        <div class="source-chooser-header">
          <h3>{sourceChooserMode === "storage"
            ? $_("storage_source_choose", {}, "Where to save files?")
            : sourceChooserMode === "recognize"
            ? $_("recognize_source_choose", {}, "Where is the image?")
            : $_("learn_source_choose", {}, "Where is the file?")}</h3>
          <p>{sourceChooserMode === "storage"
            ? $_("storage_source_hint", {}, "Select a device to store generated images, memory, and chat artifacts. The node will send files directly via P2P.")
            : sourceChooserMode === "recognize"
            ? $_("recognize_source_hint", {}, "Pick an image on one of your own devices. The image stays where it is — only its bytes are sent to the AI node to build the knowledge card.")
            : $_("learn_source_hint", {}, "Pick a document on your own device. The file stays where it is — only its content is sent to the AI node to build the knowledge card.")}</p>
        </div>
        <div class="source-list">
          {#each sourceChooserSources as src (src.linkId || src.deviceId)}
            <button type="button" class="source-item" on:click={() => pickSourceDevice(src)}>
              <span class="source-icon">
                {#if src.local === true}
                  <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="4" width="20" height="14" rx="2"/><path d="M2 9h20"/></svg>
                {:else}
                  <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="8" rx="2"/><rect x="2" y="13" width="20" height="8" rx="2"/><path d="M6 7h.01M6 17h.01"/></svg>
                {/if}
              </span>
              <span class="source-name">{src.name || $_("this_device", {}, "This device")}</span>
              <span class="source-badge">{src.local === true ? $_("this_device", {}, "This device") : $_("my_device", {}, "My device")}</span>
            </button>
          {/each}
        </div>
        <div class="source-chooser-actions">
          <button type="button" class="source-cancel" on:click={() => (sourceChooserMode = "")}>
            {$_("cancel", {}, "Cancel")}
          </button>
        </div>
      </div>
    </div>
  {/if}

  {#if showLearnPicker}
    <DocumentPicker
      device={learnSourceDevice}
      title={$_("select_document", {}, "Select Document to Learn")}
      on:select={handleLearnFileSelected}
      on:cancel={() => (showLearnPicker = false)}
    />
  {/if}

  {#if showRecognizePicker}
    <ImagePicker
      device={recognizeSourceDevice}
      title={$_("select_image", {}, "Select Image to Recognize")}
      on:select={handleRecognizeFileSelected}
      on:cancel={() => (showRecognizePicker = false)}
    />
  {/if}

  {#if showChatFolderPicker && chatStoragePickerDevice}
    <FolderPicker
      device={chatStoragePickerDevice}
      mode="folder"
      title={$_("select_destination_folder", {}, "Select Destination Folder")}
      on:select={onChatFolderSelected}
      on:cancel={() => (showChatFolderPicker = false)}
    />
  {/if}

  <!-- Гость: системный выбор файла для /learn (файл не сохраняется, только имя) -->
  <input
    type="file"
    bind:this={learnFileInput}
    accept={FILE_INPUT_ACCEPT}
    style="display:none"
    on:change={handleGuestLearnFile}
  />

  <!-- Гость: системный выбор картинки для /recognize -->
  <input
    type="file"
    bind:this={recognizeFileInput}
    accept={IMAGE_ACCEPT.join(",")}
    style="display:none"
    on:change={handleGuestRecognizeFile}
  />

  <!-- Floating Quick Scroll Dock (Bottom Right, Telegram/WhatsApp style) -->
  <div class="scroll-fab-dock" style="bottom: {(composerHeight || 90) + 14}px;">
    {#if showScrollTop}
      <button
        type="button"
        class="scroll-fab-btn"
        title={$_("scroll_top", {}, "Scroll to top")}
        on:click={() => scrollToTop(true)}
        transition:fly={{ y: 8, duration: 180 }}
      >
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="18 15 12 9 6 15" />
        </svg>
      </button>
    {/if}

    {#if showScrollBottom}
      <button
        type="button"
        class="scroll-fab-btn"
        title={$_("scroll_bottom", {}, "Scroll to bottom")}
        on:click={() => scrollToBottom(true)}
        transition:fly={{ y: 8, duration: 180 }}
      >
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="6 9 12 15 18 9" />
        </svg>
      </button>
    {/if}
  </div>
</div>

{#if showAvatarModal}
  <AssistantAvatarModal
    bind:open={showAvatarModal}
    {chatStore}
    {httpDrive}
    {omdKey}
    getDevicePush={buildDevicePush}
    getDeviceShareUrl={generatedDeviceShareUrl}
    defaultStorage={defaultStorage || ""}
    currentAvatarUrl={assistantAvatarUrl}
    {assistantName}
    on:saved={(e) => {
      if (e.detail?.avatarUrl) assistantAvatarUrl = e.detail.avatarUrl;
      if (e.detail?.assistant_name) assistantName = e.detail.assistant_name;
    }}
    on:close={() => (showAvatarModal = false)}
  />
{/if}

<style>
  .hero-avatar.clickable,
  .entry-avatar.clickable {
    cursor: pointer;
    border-radius: 50%;
    transition: transform 0.2s ease, box-shadow 0.2s ease;
  }

  .hero-avatar.clickable:hover {
    transform: scale(1.06);
    border-radius: 50%;
    box-shadow: 0 6px 30px rgba(41, 150, 204, 0.65);
  }

  .entry-avatar.clickable:hover {
    transform: scale(1.08);
    border-radius: 50%;
    box-shadow: 0 0 16px rgba(103, 194, 232, 0.6);
  }
  .chat-container {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background-color: var(--bg-primary);
    color: var(--text-primary);
    position: relative;
    overflow: hidden;
  }

  .dark-theme {
    --bg-primary: #101c22;
    --bg-secondary: #202a2f;
    --bg-card: #202a2f;
    --bg-card-hover: #2c383f;
    --border-color: #3d4f57;
    --border-subtle: #2a373e;
    --text-primary: #ffffff;
    --text-secondary: #b8c6cc;
    --text-muted: #889fa8;
    --accent-blue: #2996cc;
    --accent-blue-hover: #37a9ef;
    --user-bubble-bg: #202a2f;
    --user-bubble-border: #3d4f57;
    --pill-bg: #202a2f;
  }

  .light-theme {
    --bg-primary: #f6f8f9;
    --bg-secondary: #ffffff;
    --bg-card: #ffffff;
    --bg-card-hover: #ecf0f2;
    --border-color: #d6dde1;
    --border-subtle: #ecf0f2;
    --text-primary: #101c22;
    --text-secondary: #546b75;
    --text-muted: #889fa8;
    --accent-blue: #184562;
    --accent-blue-hover: #2996cc;
    --user-bubble-bg: #e2ecf2;
    --user-bubble-border: #ccd8e0;
    --pill-bg: #ffffff;
  }

  /* Viewport */
  .messages-viewport {
    flex: 1;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 20px 20px 140px;
    display: flex;
    flex-direction: column;
    align-items: center;
    scroll-behavior: smooth;
  }

  /* Gemini Welcome Hero */
  .gemini-hero {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    text-align: center;
    max-width: 760px;
    margin: auto 0;
    padding: 40px 16px;
    animation: fadeIn 0.4s ease-out;
  }

  .hero-assistant-badge {
    display: inline-flex;
    align-items: center;
    gap: 16px;
    margin-bottom: 24px;
    user-select: none;
    text-align: left;
  }

  .hero-avatar {
    width: 68px;
    height: 68px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .avatar-hero-img {
    width: 68px;
    height: 68px;
    border-radius: 50%;
    object-fit: cover;
    box-shadow: 0 4px 24px rgba(41, 150, 204, 0.45);
  }

  .avatar-hero-spark {
    width: 68px;
    height: 68px;
    border-radius: 50%;
    background: linear-gradient(135deg, #2996cc, #8dcbec);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
    box-shadow: 0 4px 24px rgba(41, 150, 204, 0.45);
  }

  .hero-assistant-meta {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    text-align: left;
  }

  .hero-assistant-name {
    font-size: 1.35rem;
    font-weight: 700;
    color: var(--text-primary);
    margin: 0;
    line-height: 1.2;
    letter-spacing: -0.01em;
  }

  .hero-assistant-title {
    font-size: 0.92rem;
    color: var(--text-secondary);
    margin-top: 3px;
    font-weight: 500;
    line-height: 1.2;
  }

  .hero-greeting {
    font-size: 2.2rem;
    font-weight: 700;
    letter-spacing: -0.02em;
    margin: 0 0 10px;
  }

  .gradient-text {
    background: linear-gradient(90deg, #50b1e0, #2996cc, #8dcbec);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
  }

  .hero-sub {
    font-size: 1.05rem;
    color: var(--text-secondary);
    margin: 0 0 36px;
    max-width: 520px;
  }

  .suggestion-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
    width: 100%;
  }

  .suggestion-card {
    display: flex;
    align-items: flex-start;
    gap: 14px;
    padding: 16px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    text-align: left;
    cursor: pointer;
    transition: all 0.2s ease;
    color: var(--text-primary);
  }

  .suggestion-card:hover {
    background: var(--bg-card-hover);
    border-color: var(--accent-blue);
    transform: translateY(-2px);
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.15);
  }

  .suggestion-icon {
    font-size: 1.4rem;
    flex-shrink: 0;
  }

  .suggestion-text {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .suggestion-text strong {
    font-size: 0.92rem;
    font-weight: 600;
  }

  .suggestion-text span {
    font-size: 0.82rem;
    color: var(--text-secondary);
    line-height: 1.35;
  }

  /* Conversation Thread */
  .conversation-thread {
    width: 100%;
    max-width: min(820px, calc(100% - 192px));
    display: flex;
    flex-direction: column;
    gap: 28px;
  }

  @media (max-width: 960px) {
    .conversation-thread {
      max-width: 820px;
    }
  }

  .thread-row {
    display: flex;
    width: 100%;
  }

  .thread-row.user {
    justify-content: flex-end;
  }

  .thread-row.assistant {
    justify-content: flex-start;
  }

  .thread-row.card-focus-flash {
    animation: card-focus-flash 1.8s ease;
  }

  @keyframes card-focus-flash {
    0%, 60% {
      background: rgba(41, 150, 204, 0.16);
      border-radius: 14px;
      outline: 2px solid rgba(41, 150, 204, 0.5);
      outline-offset: 2px;
    }
    100% {
      background: transparent;
      outline: 2px solid transparent;
    }
  }

  /* User Message Bubble */
  .user-bubble-wrapper {
    max-width: 78%;
    display: flex;
    flex-direction: column;
    align-items: flex-end;
  }

  .user-bubble {
    background-color: var(--user-bubble-bg);
    border: 1px solid var(--user-bubble-border);
    border-radius: 20px 20px 4px 20px;
    padding: 12px 18px;
    font-size: 0.96rem;
    line-height: 1.5;
    color: var(--text-primary);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.12);
  }

  .user-text {
    white-space: pre-wrap;
    word-break: break-word;
  }

  /* User Message Tools (Rework & Copy) */
  .user-tools {
    display: flex;
    justify-content: flex-end;
    align-items: center;
    gap: 6px;
    margin-top: 4px;
    opacity: 0;
    transition: opacity 0.15s ease;
  }

  .user-bubble-wrapper:hover .user-tools,
  .user-bubble-wrapper:focus-within .user-tools,
  .user-bubble-wrapper:active .user-tools {
    opacity: 1;
  }

  @media (hover: none) {
    .user-tools {
      opacity: 0.85;
    }
  }

  .user-rework-btn,
  .user-copy-btn {
    font-size: 0.76rem;
    font-weight: 500;
    color: var(--text-secondary);
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 3px 8px;
    border-radius: 6px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid transparent;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .user-rework-btn:hover {
    color: var(--accent-blue);
    background: rgba(41, 150, 204, 0.12);
    border-color: rgba(41, 150, 204, 0.25);
  }

  .user-copy-btn:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.1);
  }

  /* Assistant Message Layout (Gemini Style) */
  .assistant-entry {
    display: flex;
    align-items: flex-start;
    gap: 16px;
    width: 100%;
  }

  .entry-avatar {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    margin-top: 2px;
    background: transparent;
  }

  .avatar-chat-img {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    object-fit: cover;
    border: 2px solid #67c2e8;
    box-shadow: 0 0 10px rgba(103, 194, 232, 0.4);
    box-sizing: border-box;
  }

  .avatar-chat-spark {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    border: 2px solid #67c2e8;
    box-shadow: 0 0 10px rgba(103, 194, 232, 0.4);
    background: linear-gradient(135deg, #2996cc, #8dcbec);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
    box-sizing: border-box;
  }

  .entry-content {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }

  .entry-header {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 6px;
  }

  .assistant-name {
    font-weight: 600;
    font-size: 0.92rem;
    color: var(--text-primary);
  }

  .assistant-tag {
    font-size: 0.72rem;
    padding: 1px 6px;
    border-radius: 4px;
    background: rgba(41, 150, 204, 0.15);
    color: var(--accent-blue);
    font-weight: 500;
  }

  /* Markdown Styling */
  .markdown-body {
    font-size: 0.96rem;
    line-height: 1.62;
    color: var(--text-primary);
    word-break: break-word;
  }

  .markdown-body :global(p) {
    margin: 0 0 12px;
  }

  .markdown-body :global(p:last-child) {
    margin-bottom: 0;
  }

  .markdown-body :global(ul),
  .markdown-body :global(ol) {
    margin: 8px 0 14px;
    padding-left: 24px;
  }

  .markdown-body :global(li) {
    margin-bottom: 4px;
  }

  .markdown-body :global(h1),
  .markdown-body :global(h2),
  .markdown-body :global(h3),
  .markdown-body :global(h4) {
    margin: 18px 0 8px;
    font-weight: 600;
    line-height: 1.35;
  }

  .markdown-body :global(h1) { font-size: 1.35rem; }
  .markdown-body :global(h2) { font-size: 1.2rem; }
  .markdown-body :global(h3) { font-size: 1.05rem; }

  .markdown-body :global(pre) {
    background: rgba(0, 0, 0, 0.35);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 14px 16px;
    overflow-x: auto;
    font-family: 'Roboto Mono', monospace;
    font-size: 0.88rem;
    margin: 14px 0;
  }

  .markdown-body :global(code) {
    font-family: 'Roboto Mono', monospace;
    font-size: 0.88rem;
  }

  .markdown-body :global(:not(pre) > code) {
    background: rgba(84, 107, 117, 0.22);
    border-radius: 4px;
    padding: 2px 6px;
  }

  .markdown-body :global(blockquote) {
    border-left: 3px solid var(--accent-blue);
    margin: 12px 0;
    padding-left: 14px;
    color: var(--text-secondary);
  }

  .markdown-body :global(table) {
    border-collapse: collapse;
    margin: 14px 0;
    width: 100%;
    font-size: 0.9rem;
    display: block;
    max-width: 100%;
    overflow-x: auto;
  }

  .markdown-body :global(th),
  .markdown-body :global(td) {
    border: 1px solid var(--border-color);
    padding: 8px 12px;
  }

  .markdown-body :global(th) {
    background: rgba(41, 150, 204, 0.08);
    font-weight: 600;
  }

  /* Gemini Reasoning/Thinking Accordion */
  .gemini-reasoning {
    margin: 6px 0 12px;
    background: rgba(41, 150, 204, 0.05);
    border: 1px solid rgba(41, 150, 204, 0.2);
    border-radius: 12px;
    overflow: hidden;
  }

  .gemini-reasoning summary {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 8px 14px;
    font-size: 0.84rem;
    color: var(--accent-blue);
    cursor: pointer;
    user-select: none;
    font-weight: 500;
    list-style: none;
  }

  .gemini-reasoning summary::-webkit-details-marker {
    display: none;
  }

  .reasoning-summary-left {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .accordion-chevron {
    transition: transform 0.2s ease;
    color: var(--accent-blue);
    opacity: 0.8;
  }

  .gemini-reasoning[open] .accordion-chevron {
    transform: rotate(180deg);
  }

  .reasoning-inner {
    padding: 10px 14px 12px;
    font-size: 0.85rem;
    color: var(--text-secondary);
    line-height: 1.5;
    border-top: 1px solid rgba(41, 150, 204, 0.12);
    max-height: 340px;
    overflow-y: auto;
  }

  .reasoning-inner p {
    margin: 0 0 6px;
  }

  .reasoning-inner p:last-child {
    margin-bottom: 0;
  }

  .reasoning-inner ul,
  .reasoning-inner ol {
    margin: 4px 0 6px 18px;
    padding: 0;
  }

  .reasoning-inner li {
    margin-bottom: 3px;
  }

  .reasoning-inner li:last-child {
    margin-bottom: 0;
  }

  .reasoning-waiting-hint {
    opacity: 0.75;
    font-style: italic;
  }

  .gemini-reasoning.active-expanded {
    margin-top: 6px;
  }

  button.active-stream-status.clickable {
    background: inherit;
    border: inherit;
    cursor: pointer;
    font-family: inherit;
    color: inherit;
  }

  button.active-stream-status.clickable:hover {
    filter: brightness(1.2);
    transform: translateY(-1px);
  }

  .accordion-chevron.rotated {
    transform: rotate(180deg);
  }

  /* Execution Plan / Multi-step Accordion */
  .plan-card {
    margin: 8px 0 12px;
    border-radius: 12px;
    overflow: hidden;
    transition: all 0.25s ease;
  }

  .plan-card.running {
    background: rgba(249, 115, 22, 0.05);
    border: 1px solid rgba(249, 115, 22, 0.25);
  }

  .plan-card.completed {
    background: rgba(16, 185, 129, 0.05);
    border: 1px solid rgba(16, 185, 129, 0.25);
  }

  .plan-summary {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 8px 14px;
    font-size: 0.84rem;
    font-weight: 500;
    cursor: pointer;
    user-select: none;
    list-style: none;
  }

  .plan-card.running .plan-summary {
    color: #f97316;
  }

  .plan-card.completed .plan-summary {
    color: #10b981;
  }

  .plan-summary::-webkit-details-marker {
    display: none;
  }

  .plan-summary-left {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .plan-icon.executing {
    display: inline-block;
    animation: rotate 2s linear infinite;
    transform-origin: center center;
    font-size: 1.05rem;
    line-height: 1;
  }

  .plan-header-checkbox.completed {
    flex-shrink: 0;
    width: 18px;
    height: 18px;
    border-radius: 5px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: #10b981;
    color: #ffffff;
    border: 1px solid #059669;
    box-shadow: 0 1px 2px rgba(16, 185, 129, 0.25);
    box-sizing: border-box;
  }

  .plan-counter {
    font-size: 0.78rem;
    opacity: 0.85;
    font-weight: 600;
    margin-left: 2px;
  }

  .plan-card[open] .accordion-chevron {
    transform: rotate(180deg);
  }

  .plan-inner {
    padding: 10px 14px 12px;
    font-size: 0.85rem;
    line-height: 1.5;
    border-top: 1px solid rgba(0, 0, 0, 0.08);
    max-height: 380px;
    overflow-y: auto;
  }

  .plan-card.running .plan-inner {
    border-top-color: rgba(249, 115, 22, 0.15);
  }

  .plan-card.completed .plan-inner {
    border-top-color: rgba(16, 185, 129, 0.15);
  }

  .plan-steps-list {
    display: flex;
    flex-direction: column;
    gap: 9px;
  }

  .plan-step-item {
    display: flex;
    align-items: flex-start;
    gap: 10px;
    color: var(--text-secondary);
    transition: color 0.2s ease;
  }

  .plan-step-item.completed {
    color: var(--text-primary);
  }

  .plan-step-item.running {
    color: var(--text-primary);
    font-weight: 500;
  }

  button.step-checkbox {
    flex-shrink: 0;
    width: 18px;
    height: 18px;
    margin-top: 2px;
    padding: 0;
    border-radius: 5px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    transition: all 0.2s ease;
    box-sizing: border-box;
    cursor: pointer;
    font-family: inherit;
  }

  button.step-checkbox.completed {
    background: #10b981;
    color: #ffffff;
    border: 1px solid #059669;
    box-shadow: 0 1px 2px rgba(16, 185, 129, 0.25);
  }

  button.step-checkbox.running {
    background: rgba(249, 115, 22, 0.1);
    border: 1.5px solid #f97316;
  }

  button.step-checkbox.pending {
    background: rgba(0, 0, 0, 0.03);
    border: 1.5px solid rgba(125, 125, 125, 0.35);
  }

  .dark-theme button.step-checkbox.pending {
    background: rgba(255, 255, 255, 0.03);
    border-color: rgba(255, 255, 255, 0.25);
  }

  button.step-checkbox:hover {
    filter: brightness(1.1);
    transform: scale(1.05);
  }

  .step-mini-spinner {
    width: 9px;
    height: 9px;
    border: 2px solid rgba(249, 115, 22, 0.3);
    border-top-color: #f97316;
    border-radius: 50%;
    animation: rotate 0.8s linear infinite;
  }

  .step-text {
    flex: 1;
    min-width: 0;
    font-size: 0.85rem;
    line-height: 1.5;
    word-break: break-word;
  }

  .step-text :global(p) {
    margin: 0 0 4px;
  }

  .step-text :global(p:last-child) {
    margin-bottom: 0;
  }

  .step-text :global(code) {
    font-size: 0.82rem;
    padding: 1px 5px;
    border-radius: 4px;
    background: rgba(0, 0, 0, 0.06);
    color: inherit;
  }

  .dark-theme .step-text :global(code) {
    background: rgba(255, 255, 255, 0.08);
  }

  /* RAG Facts Accordion (Known facts & New facts) */
  .rag-facts-accordion {
    margin: 8px 0 12px;
    background: rgba(234, 179, 8, 0.05);
    border: 1px solid rgba(234, 179, 8, 0.25);
    border-radius: 12px;
    overflow: hidden;
  }

  .rag-facts-accordion.new-fact {
    background: rgba(168, 85, 247, 0.05);
    border-color: rgba(168, 85, 247, 0.25);
  }

  .rag-facts-accordion summary {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 8px 14px;
    font-size: 0.84rem;
    color: #eab308;
    cursor: pointer;
    user-select: none;
    font-weight: 500;
    list-style: none;
  }

  .rag-facts-accordion.new-fact summary {
    color: #c084fc;
  }

  .rag-facts-accordion summary::-webkit-details-marker {
    display: none;
  }

  .facts-summary-left {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .rag-facts-accordion[open] .accordion-chevron {
    transform: rotate(180deg);
  }

  .facts-inner {
    padding: 10px 14px 12px;
    font-size: 0.88rem;
    line-height: 1.55;
    border-top: 1px solid rgba(234, 179, 8, 0.15);
    max-height: 300px;
    overflow-y: auto;
    overflow-x: auto;
    min-width: 0;
    width: 100%;
    box-sizing: border-box;
    overflow-wrap: anywhere;
  }

  .rag-facts-accordion.new-fact .facts-inner {
    border-top-color: rgba(168, 85, 247, 0.15);
  }

  /* Status indicator */
  .gemini-status-indicator {
    display: flex;
    align-items: center;
    gap: 8px;
    margin: 6px 0;
    font-size: 0.84rem;
    color: var(--text-muted);
  }

  .pulse-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--accent-blue);
    animation: pulse 1.4s infinite;
  }

  @keyframes pulse {
    0% { transform: scale(0.9); opacity: 0.7; }
    50% { transform: scale(1.3); opacity: 1; }
    100% { transform: scale(0.9); opacity: 0.7; }
  }

  /* Sources toggle in message toolbar */
  .sources-toggle-btn.active {
    color: var(--accent-blue);
    background: rgba(41, 150, 204, 0.12);
  }

  .sources-chevron {
    transition: transform 0.2s ease;
  }

  .sources-chevron.rotated {
    transform: rotate(180deg);
  }

  /* Expanded Sources Drawer (RAG results) */
  .rag-sources-expanded {
    margin-top: 10px;
    padding: 12px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.08);
  }

  .sources-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: 8px;
  }

  .source-card {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 12px;
    border-radius: 8px;
    background: var(--bg-primary);
    border: 1px solid var(--border-subtle);
    color: var(--text-primary);
    text-decoration: none;
    cursor: pointer;
    text-align: left;
    width: 100%;
    font-family: inherit;
    transition: all 0.15s ease;
  }

  .source-card:hover {
    border-color: var(--accent-blue);
    background: var(--bg-card-hover);
    transform: translateY(-1px);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
  }

  .source-card.static {
    cursor: default;
  }

  .source-card.static:hover {
    border-color: var(--border-subtle);
    background: var(--bg-primary);
    transform: none;
    box-shadow: none;
  }

  .source-card:disabled {
    opacity: 1;
  }

  .source-card-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    border-radius: 6px;
    background: rgba(41, 150, 204, 0.12);
    color: var(--accent-blue);
    flex-shrink: 0;
  }

  .source-card-icon.has-preview {
    background: var(--bg-primary);
    overflow: hidden;
  }

  .source-card-preview {
    width: 28px;
    height: 28px;
    object-fit: cover;
    border-radius: 6px;
    display: block;
  }

  .source-card-info {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }

  .source-card-title {
    font-size: 0.84rem;
    font-weight: 500;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .source-card-sub {
    font-size: 0.72rem;
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* Inline clickable chips for source files mentioned inside the answer.
     Thumbnail-чипсы остаются инлайн (не ломают поток текста): маленькая
     миниатюра, имя файла, вся строка помещается внутри пузыря сообщения. */
  .markdown-body :global(.inline-file-chip) {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    max-width: 100%;
    margin: 0 2px;
    padding: 2px 8px 2px 3px;
    border-radius: 7px;
    background: rgba(41, 150, 204, 0.10);
    border: 1px solid rgba(41, 150, 204, 0.28);
    color: var(--accent-blue);
    text-decoration: none;
    vertical-align: middle;
    cursor: pointer;
    line-height: 1.25;
    font-weight: 500;
    box-sizing: border-box;
  }

  .markdown-body :global(.inline-file-chip:hover) {
    background: rgba(41, 150, 204, 0.18);
    border-color: var(--accent-blue);
  }

  .markdown-body :global(.inline-file-thumb) {
    width: 30px;
    height: 30px;
    object-fit: cover;
    border-radius: 5px;
    flex-shrink: 0;
    display: block;
  }

  .markdown-body :global(.inline-file-icon) {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    min-width: 26px;
    height: 26px;
    padding: 0 4px;
    flex-shrink: 0;
    border-radius: 6px;
    background: rgba(41, 150, 204, 0.14);
    border: 1px solid rgba(41, 150, 204, 0.28);
    color: var(--accent-blue);
    font-size: 0.62rem;
    font-weight: 700;
    letter-spacing: 0.03em;
    line-height: 1;
    box-sizing: border-box;
  }

  .markdown-body :global(.inline-file-name) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }

  /* RAG Sources */
  .rag-sources-box {
    margin-top: 14px;
    padding-top: 10px;
    border-top: 1px solid var(--border-subtle);
  }

  .sources-label {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 0.78rem;
    font-weight: 600;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.04em;
    margin-bottom: 8px;
  }

  .sources-pill-row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
  }

  .source-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 12px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 20px;
    font-size: 0.82rem;
    color: var(--text-primary);
    text-decoration: none;
    transition: all 0.2s ease;
    max-width: 280px;
  }

  .source-pill:hover {
    border-color: var(--accent-blue);
    background: var(--bg-card-hover);
    color: var(--accent-blue);
  }

  .source-title {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* Interactive Cards */
  .interactive-action-card {
    margin: 12px 0;
    padding: 14px 16px;
    border-radius: 14px;
    background: rgba(41, 150, 204, 0.08);
    border: 1px solid var(--accent-blue);
  }

  .perm-card {
    background: rgba(41, 204, 84, 0.06);
    border-color: #29cc54;
  }

  .action-title {
    margin: 0 0 10px;
    font-weight: 600;
    font-size: 0.92rem;
  }

  .perm-code {
    display: block;
    font-size: 0.8rem;
    color: var(--text-secondary);
    margin-bottom: 10px;
    word-break: break-all;
  }

  .action-buttons {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    align-items: center;
  }

  .action-choice-btn,
  .action-allow-btn,
  .action-deny-btn,
  .action-reject-btn {
    border: none;
    border-radius: 8px;
    padding: 6px 14px;
    font-size: 0.85rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.2s;
  }

  .action-choice-btn {
    background: var(--accent-blue);
    color: #fff;
  }

  .action-allow-btn {
    background: #29cc54;
    color: #fff;
  }

  .action-deny-btn,
  .action-reject-btn {
    background: transparent;
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
  }

  .action-submitted {
    font-size: 0.82rem;
    color: #29cc54;
    font-weight: 500;
    margin-left: 6px;
  }

  /* Message Footer Tools */
  .message-footer-tools {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-top: 8px;
    opacity: 0.6;
    transition: opacity 0.2s;
  }

  .assistant-entry:hover .message-footer-tools {
    opacity: 1;
  }

  .tool-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 4px 6px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    gap: 4px;
    font-size: 0.78rem;
  }

  .tool-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
  }

  .copied-label {
    color: #29cc54;
    font-weight: 600;
  }

  .tool-btn.tts-btn.active {
    color: #38bdf8;
    background: rgba(56, 189, 248, 0.15);
  }
  .tool-btn.tts-btn.loading {
    color: #f59e0b;
  }
  .tts-spinner {
    width: 13px;
    height: 13px;
    border: 2px solid rgba(255, 255, 255, 0.2);
    border-top-color: currentColor;
    border-radius: 50%;
    animation: tts-spin 0.8s linear infinite;
    display: inline-block;
  }
  @keyframes tts-spin {
    to { transform: rotate(360deg); }
  }
  .tts-equalizer {
    display: flex;
    align-items: flex-end;
    gap: 2px;
    height: 12px;
    padding: 0 1px;
  }
  .tts-equalizer .bar {
    width: 2.5px;
    background: #38bdf8;
    border-radius: 1px;
    animation: tts-wave 0.8s ease-in-out infinite alternate;
  }
  .tts-equalizer .bar-1 { height: 40%; animation-delay: 0.1s; }
  .tts-equalizer .bar-2 { height: 85%; animation-delay: 0.3s; }
  .tts-equalizer .bar-3 { height: 60%; animation-delay: 0.2s; }
  @keyframes tts-wave {
    0% { height: 25%; }
    100% { height: 100%; }
  }

  .composer-voice-toggle-btn {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    border: none;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    flex-shrink: 0;
    transition: all 0.2s ease;
    background: transparent;
    color: var(--text-secondary);
    padding: 0;
    margin: 0;
  }
  .composer-voice-toggle-btn:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.08);
  }
  .composer-voice-toggle-btn.active {
    color: #38bdf8;
    background: rgba(56, 189, 248, 0.2);
    box-shadow: 0 0 10px rgba(56, 189, 248, 0.25);
  }

  /* Floating Bottom Composer */
  .composer-outer {
    position: absolute;
    bottom: 0;
    left: 0;
    right: 0;
    display: flex;
    justify-content: center;
    padding: 0 20px calc(16px + env(safe-area-inset-bottom, 0px));
    pointer-events: none;
    background: linear-gradient(to top, var(--bg-primary) 70%, transparent 100%);
  }

  /* Floating Quick Scroll Controls (Telegram / WhatsApp style) */
  .scroll-fab-dock {
    position: absolute;
    right: 22px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
    z-index: 35;
    pointer-events: none;
    transition: bottom 0.2s ease;
  }

  .scroll-fab-btn {
    width: 38px;
    height: 38px;
    border-radius: 50%;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    pointer-events: auto;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.25);
    transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
  }

  .scroll-fab-btn:hover {
    background: var(--bg-card-hover);
    color: var(--accent-blue);
    transform: translateY(-2px);
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.35);
  }

  .scroll-fab-btn:active {
    transform: translateY(0);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
  }

  @media (max-width: 768px) {
    .scroll-fab-dock {
      right: 14px;
      gap: 6px;
    }
    .scroll-fab-btn {
      width: 36px;
      height: 36px;
    }
  }

  .composer-container {
    width: 100%;
    max-width: 820px;
    display: flex;
    flex-direction: column;
    align-items: center;
    pointer-events: auto;
  }

  .composer-pill {
    width: 100%;
    display: flex;
    align-items: flex-end;
    gap: 8px;
    background: var(--pill-bg);
    border: 1px solid var(--border-color);
    border-radius: 24px;
    padding: 6px 10px;
    box-shadow: 0 4px 24px rgba(0, 0, 0, 0.28);
    backdrop-filter: blur(16px);
    transition: border-color 0.2s, box-shadow 0.2s;
    box-sizing: border-box;
    position: relative;
  }

  .composer-pill:focus-within {
    border-color: var(--accent-blue);
    box-shadow: 0 4px 28px rgba(41, 150, 204, 0.2);
  }

  /* Plus Button and Tools Popup */
  .composer-action-wrap {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .composer-action-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    width: 36px;
    height: 36px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.2s, color 0.2s;
    flex-shrink: 0;
  }

  .composer-action-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
  }

  .composer-action-btn.active {
    color: var(--accent-blue);
    background: rgba(41, 150, 204, 0.15);
  }

  .plus-icon {
    transition: transform 0.2s ease;
  }

  .plus-icon.rotated {
    transform: rotate(45deg);
  }

  .tools-popup-menu {
    position: absolute;
    bottom: calc(100% + 12px);
    left: 0;
    width: 350px;
    max-width: calc(100vw - 32px);
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 18px;
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.35), 0 0 0 1px rgba(255, 255, 255, 0.04);
    backdrop-filter: blur(20px);
    z-index: 50;
    animation: popIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  @keyframes popIn {
    from {
      opacity: 0;
      transform: translateY(8px) scale(0.96);
    }
    to {
      opacity: 1;
      transform: translateY(0) scale(1);
    }
  }

  .tools-popup-header {
    padding: 12px 16px 8px;
    border-bottom: 1px solid var(--border-subtle);
  }

  .tools-popup-title {
    font-size: 0.75rem;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-muted);
  }

  .tools-popup-list {
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .tools-menu-item {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 9px 10px;
    border: none;
    background: transparent;
    border-radius: 12px;
    cursor: pointer;
    text-align: left;
    color: var(--text-primary);
    transition: background 0.15s ease, transform 0.1s ease;
    font-family: inherit;
    width: 100%;
    box-sizing: border-box;
  }

  .tools-menu-item:hover {
    background: var(--bg-card-hover);
  }

  .tools-menu-item:active {
    transform: scale(0.98);
  }

  .tool-icon-circle {
    width: 32px;
    height: 32px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    transition: transform 0.15s ease;
  }

  .tools-menu-item:hover .tool-icon-circle {
    transform: scale(1.08);
  }

  .tool-icon-circle.think {
    background: rgba(245, 158, 11, 0.15);
    color: #f59e0b;
  }

  .tool-icon-circle.search {
    background: rgba(41, 150, 204, 0.15);
    color: #38bdf8;
  }

  .tool-icon-circle.learn {
    background: rgba(16, 185, 129, 0.15);
    color: #10b981;
  }

  .tool-icon-circle.recognize {
    background: rgba(168, 85, 247, 0.15);
    color: #c084fc;
  }

  .tool-icon-circle.generate {
    background: rgba(236, 72, 153, 0.15);
    color: #f472b6;
  }

  .tool-meta {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .tool-title-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 6px;
  }

  .tool-name {
    font-size: 0.88rem;
    font-weight: 600;
    line-height: 1.2;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .tool-chip {
    font-family: monospace;
    font-size: 0.72rem;
    font-weight: 600;
    padding: 1px 6px;
    border-radius: 6px;
    background: rgba(41, 150, 204, 0.12);
    color: var(--accent-blue);
    flex-shrink: 0;
  }

  .tool-hint {
    font-size: 0.74rem;
    color: var(--text-muted);
    line-height: 1.2;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .composer-textarea {
    flex: 1;
    background: transparent;
    border: none;
    outline: none;
    color: var(--text-primary);
    font-size: 0.95rem;
    line-height: 24px;
    resize: none;
    padding: 5px 6px 7px;
    font-family: inherit;
    box-sizing: border-box;
    height: 36px;
    min-height: 36px;
    max-height: 180px;
    overflow-y: hidden;
  }

  .composer-textarea::placeholder {
    color: var(--text-muted);
  }

  .composer-textarea::-webkit-scrollbar {
    width: 4px;
  }

  .composer-textarea::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.2);
    border-radius: 4px;
  }

  .composer-textarea::-webkit-scrollbar-track {
    background: transparent;
  }

  .composer-send-btn,
  .composer-stop-btn {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    border: none;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    flex-shrink: 0;
    transition: all 0.2s ease;
  }

  .composer-send-btn {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-muted);
  }

  .composer-send-btn.active {
    background: var(--accent-blue);
    color: #ffffff;
    box-shadow: 0 2px 10px rgba(41, 150, 204, 0.4);
  }

  .composer-send-btn:disabled {
    cursor: not-allowed;
    opacity: 0.45;
  }

  .composer-stop-btn {
    background: #ef4444;
    color: #ffffff;
    box-shadow: 0 2px 10px rgba(239, 68, 68, 0.4);
  }

  .composer-disclaimer {
    font-size: 0.74rem;
    color: var(--text-muted);
    margin-top: 8px;
    text-align: center;
  }

  .import-result-card {
    border: 1px solid rgba(41, 150, 204, 0.3);
    background: rgba(41, 150, 204, 0.06);
    border-radius: 14px;
    padding: 12px 16px;
    margin: 8px 0;
    color: var(--text-primary);
  }

  .import-result-head {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .import-result-icon {
    font-size: 1.1rem;
  }

  .import-result-title {
    font-weight: 600;
    flex: 1;
    min-width: 0;
  }

  .import-result-badge {
    background: rgba(41, 150, 204, 0.2);
    color: #7dd3fc;
    border-radius: 10px;
    padding: 2px 8px;
    font-size: 0.7rem;
    font-weight: 600;
  }

  .import-result-annotation {
    margin-top: 8px;
    font-size: 0.84rem;
    color: var(--text-secondary);
    line-height: 1.5;
  }

  .import-result-image-wrap {
    margin-top: 8px;
    border-radius: 10px;
    overflow: hidden;
    border: 1px solid var(--border-color, rgba(0,0,0,0.08));
    max-width: 100%;
  }

  .import-result-image-wrap.image-on-another-device {
    background: var(--bg-hover, rgba(255, 255, 255, 0.03));
    border: 1px dashed var(--border-color, rgba(255, 255, 255, 0.15));
    padding: 16px 14px;
    box-sizing: border-box;
  }

  .another-device-placeholder {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    color: var(--text-secondary, #94a3b8);
    font-size: 0.85rem;
    font-weight: 500;
    user-select: none;
  }

  .another-device-svg {
    opacity: 0.75;
    flex-shrink: 0;
  }

  .chat-placeholder-device-hint {
    display: block;
    margin-top: 4px;
    font-size: 0.8rem;
    color: var(--text-secondary, #94a3b8);
    opacity: 0.85;
    font-weight: normal;
  }

  .import-result-image-wrap.clickable-image {
    position: relative;
    cursor: pointer;
    transition: border-color 0.15s ease;
  }

  .import-result-image-wrap.clickable-image:hover {
    border-color: var(--accent, #2996cc);
  }

  .import-result-image-zoom {
    position: absolute;
    right: 8px;
    bottom: 8px;
    width: 26px;
    height: 26px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 8px;
    background: rgba(0, 0, 0, 0.55);
    color: #fff;
    font-size: 15px;
    pointer-events: none;
  }

  .import-request-block {
    margin-top: 10px;
    border-top: 1px dashed var(--border-color, rgba(0,0,0,0.12));
    padding-top: 8px;
  }

  .import-request-summary {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    list-style: none;
    user-select: none;
    color: var(--text-secondary);
    font-size: 0.76rem;
  }

  .import-request-summary::-webkit-details-marker {
    display: none;
  }

  .import-request-label {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .card-rework-btn {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    background: rgba(41, 150, 204, 0.15);
    border: 1px solid rgba(41, 150, 204, 0.3);
    color: #67c2e8;
    border-radius: 12px;
    padding: 2px 8px;
    font-size: 0.72rem;
    font-weight: 600;
    cursor: pointer;
    flex-shrink: 0;
  }

  .card-rework-btn:hover {
    background: rgba(41, 150, 204, 0.25);
  }

  .import-request-text {
    margin: 8px 0 0;
    padding: 8px 10px;
    border-radius: 8px;
    background: var(--bg-soft, rgba(0, 0, 0, 0.04));
    border: 1px solid var(--border-color, rgba(0,0,0,0.08));
    font-size: 0.72rem;
    color: var(--text-secondary);
    line-height: 1.5;
    white-space: pre-wrap;
    word-break: break-word;
    max-height: 160px;
    overflow-y: auto;
  }

  .import-result-image {
    display: block;
    width: 100%;
    max-height: 260px;
    object-fit: cover;
    border-radius: 10px;
  }

  .import-result-annotation p {
    margin: 4px 0;
  }

  .import-result-tags {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-top: 10px;
  }

  .imported-tag-chip {
    background: rgba(41, 150, 204, 0.15);
    border: 1px solid rgba(41, 150, 204, 0.3);
    color: #67c2e8;
    border-radius: 12px;
    padding: 2px 8px;
    font-size: 0.72rem;
    font-weight: 500;
  }

  .import-result-card.loading {
    border-color: rgba(234, 179, 8, 0.3);
    background: rgba(234, 179, 8, 0.06);
  }

  .import-result-badge.learning {
    background: rgba(234, 179, 8, 0.2);
    color: #facc15;
  }

  .import-result-badge.recognizing {
    background: rgba(41, 150, 204, 0.2);
    color: #4fc3f7;
  }

  .import-result-loading {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-top: 8px;
    font-size: 0.82rem;
    color: var(--text-secondary);
  }

  .import-spinner {
    display: inline-block;
    width: 14px;
    height: 14px;
    border: 2px solid rgba(234, 179, 8, 0.4);
    border-top-color: #facc15;
    border-radius: 50%;
    animation: importSpin 0.8s linear infinite;
    flex-shrink: 0;
  }

  @keyframes importSpin {
    to { transform: rotate(360deg); }
  }

  .attach-switch {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    background: none;
    border: none;
    padding: 0 4px 0 0;
    cursor: pointer;
    color: var(--text-secondary);
    font-family: inherit;
    transition: color 0.18s;
    margin-left: auto;
    flex-shrink: 0;
  }

  .attach-switch.on {
    color: #4ade80;
  }

  .attach-switch-track {
    position: relative;
    width: 34px;
    height: 19px;
    border-radius: 12px;
    background: rgba(120, 120, 120, 0.25);
    border: 1px solid rgba(120, 120, 120, 0.35);
    transition: background 0.18s, border-color 0.18s;
    flex-shrink: 0;
  }

  .attach-switch.on .attach-switch-track {
    background: rgba(34, 197, 94, 0.35);
    border-color: rgba(34, 197, 94, 0.6);
  }

  .attach-switch-knob {
    position: absolute;
    top: 2px;
    left: 1px;
    width: 15px;
    height: 15px;
    border-radius: 50%;
    background: #c0c0c0;
    transition: transform 0.18s, background 0.18s;
  }

  .attach-switch.on .attach-switch-knob {
    transform: translateX(15px);
    background: #22c55e;
  }

  .attach-switch-label {
    font-size: 0.78rem;
    font-weight: 500;
  }

  @media (max-width: 768px) {
    .attach-switch-label {
      display: none;
    }
    .attach-switch {
      gap: 0;
      padding: 0;
      margin-left: 6px;
    }
  }

  .active-stream-status {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    border-radius: 20px;
    font-size: 0.84rem;
    font-weight: 500;
    margin: 8px 0;
    width: fit-content;
    background: rgba(41, 150, 204, 0.08);
    border: 1px solid rgba(41, 150, 204, 0.2);
    color: var(--accent-blue);
    animation: fadeIn 0.2s ease-out;
  }

  .active-stream-status.typing,
  .active-stream-status.processing {
    background: rgba(41, 150, 204, 0.12);
    border-color: rgba(41, 150, 204, 0.3);
    color: var(--accent-blue);
  }

  .active-stream-status.thinking {
    background: rgba(234, 179, 8, 0.08);
    border-color: rgba(234, 179, 8, 0.25);
    color: #eab308;
  }

  .active-stream-status.summarizing {
    background: rgba(16, 185, 129, 0.09);
    border-color: rgba(16, 185, 129, 0.28);
    color: #10b981;
  }

  .active-stream-status.searching {
    background: rgba(6, 182, 212, 0.08);
    border-color: rgba(6, 182, 212, 0.25);
    color: #06b6d4;
  }

  .active-stream-status.performing {
    background: rgba(168, 85, 247, 0.08);
    border-color: rgba(168, 85, 247, 0.25);
    color: #a855f7;
  }

  .active-stream-status.executing {
    background: rgba(249, 115, 22, 0.08);
    border-color: rgba(249, 115, 22, 0.25);
    color: #f97316;
  }

  .active-stream-status.generating {
    background: rgba(59, 130, 246, 0.1);
    border-color: rgba(59, 130, 246, 0.3);
    color: #60a5fa;
  }

  .active-stream-status.generating .status-icon-bubble {
    display: inline-block;
    animation: rotate 1.2s linear infinite;
    transform-origin: center center;
    font-size: 1.15rem;
    filter: drop-shadow(0 0 6px rgba(59, 130, 246, 0.6));
  }

  .status-icon-bubble {
    font-size: 1rem;
    line-height: 1;
  }

  .active-stream-status.thinking .status-icon-bubble {
    animation: pulse 1.5s infinite;
  }

  .active-stream-status.searching .status-icon-bubble,
  .active-stream-status.executing .status-icon-bubble {
    display: inline-block;
    animation: rotate 2s linear infinite;
  }

  .stream-status-text {
    font-size: 0.84rem;
  }

  .typing-dots {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    margin-left: 2px;
  }

  .typing-dots span {
    width: 4px;
    height: 4px;
    border-radius: 50%;
    background: currentColor;
    opacity: 0.4;
    animation: typingBlink 1.2s infinite ease-in-out;
  }

  .typing-dots span:nth-child(2) {
    animation-delay: 0.2s;
  }

  .typing-dots span:nth-child(3) {
    animation-delay: 0.4s;
  }

  @keyframes typingBlink {
    0%, 80%, 100% { transform: scale(0.8); opacity: 0.3; }
    40% { transform: scale(1.3); opacity: 1; }
  }

  .sparkle-spinning {
    animation: rotate 2s linear infinite;
  }

  @keyframes rotate {
    from { transform: rotate(0deg); }
    to { transform: rotate(360deg); }
  }

  .chat-error-banner {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 14px;
    border-radius: 10px;
    background: rgba(239, 68, 68, 0.12);
    border: 1px solid #ef4444;
    color: #f87171;
    font-size: 0.88rem;
  }

  @keyframes fadeIn {
    from { opacity: 0; transform: translateY(8px); }
    to { opacity: 1; transform: translateY(0); }
  }

  /* Generated Image Card in Chat */
  .chat-image-card {
    margin-top: 10px;
    margin-bottom: 6px;
    max-width: 440px;
    border-radius: 14px;
    overflow: hidden;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    box-shadow: 0 4px 18px rgba(0, 0, 0, 0.18);
    transition: transform 0.18s ease, box-shadow 0.18s ease;
  }

  .chat-image-card:hover {
    box-shadow: 0 8px 26px rgba(0, 0, 0, 0.28);
  }

  .chat-image-wrap {
    position: relative;
    cursor: pointer;
    overflow: hidden;
    display: flex;
    justify-content: center;
    align-items: center;
    background: rgba(0, 0, 0, 0.25);
    min-height: 260px;
    width: 100%;
    aspect-ratio: 1 / 1;
  }

  .chat-image-wrap.has-error {
    min-height: 80px;
    aspect-ratio: auto;
    cursor: default;
  }

  .chat-image-placeholder {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    background: var(--bg-hover, rgba(255, 255, 255, 0.03));
    gap: 16px;
    z-index: 2;
    padding: 20px;
    box-sizing: border-box;
  }

  .chat-image-placeholder.has-error {
    position: relative;
    padding: 20px 16px;
    min-height: 80px;
  }

  .chat-placeholder-svg {
    width: 90px;
    height: 90px;
    object-fit: contain;
    opacity: 0.95;
  }

  .chat-placeholder-svg.spinner-pulse {
    filter: drop-shadow(0 6px 20px rgba(59, 130, 246, 0.45));
  }

  .chat-placeholder-text {
    font-size: 13px;
    font-weight: 500;
    color: var(--text-secondary, #94a3b8);
    text-align: center;
    padding: 0 16px;
  }

  .chat-placeholder-retry {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 14px;
    font-size: 12px;
    border-radius: 6px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    cursor: pointer;
    transition: background 0.15s ease, border-color 0.15s ease;
  }

  .chat-placeholder-retry:hover {
    background: var(--border-color);
    border-color: var(--accent-blue);
    color: var(--accent-blue);
  }

  .chat-generated-img {
    width: 100%;
    height: 100%;
    object-fit: contain;
    display: block;
    transition: opacity 0.3s ease, transform 0.25s ease;
  }

  .chat-generated-img.hidden {
    display: none !important;
  }

  .chat-image-wrap:hover .chat-generated-img:not(.hidden) {
    transform: scale(1.02);
  }

  .chat-image-description {
    font-size: 12px;
    line-height: 1.45;
    color: var(--text-secondary);
    word-break: break-word;
  }

  :global(.spin) {
    animation: spin-icon 1s linear infinite;
  }

  @keyframes spin-icon {
    from { transform: rotate(0deg); }
    to { transform: rotate(360deg); }
  }

  .chat-image-meta {
    padding: 10px 14px 12px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .chat-image-title {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-primary);
  }

  .chat-image-prompt-details {
    background: var(--bg-hover);
    border-radius: 8px;
    border: 1px solid var(--border-color);
    padding: 4px 8px;
    font-size: 12px;
  }

  .chat-image-prompt-summary {
    display: flex;
    align-items: center;
    justify-content: space-between;
    cursor: pointer;
    color: var(--text-secondary);
    font-weight: 500;
    user-select: none;
    outline: none;
  }

  .chat-image-prompt-text {
    margin-top: 6px;
    font-size: 11px;
    line-height: 1.45;
    color: var(--text-primary);
    word-break: break-word;
  }

  .copy-prompt-btn {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    background: none;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 2px 4px;
    border-radius: 4px;
    transition: color 0.15s ease;
  }

  .copy-prompt-btn:hover {
    color: var(--accent-blue);
  }

  .chat-image-actions {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 8px;
    margin-top: 2px;
  }

  .temp-image-hint {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 3px 8px;
    border-radius: 6px;
    font-size: 11px;
    line-height: 1.3;
    color: var(--text-secondary, var(--text-muted, inherit));
    background: var(--bg-hover);
    border: 1px dashed var(--border-color);
    opacity: 0.85;
  }

  .image-action-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 10px;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 500;
    background: var(--bg-hover);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    cursor: pointer;
    transition: background 0.15s ease, border-color 0.15s ease;
  }

  .image-action-btn:hover {
    background: var(--border-color);
    border-color: var(--accent-blue);
    color: var(--accent-blue);
  }

  .chat-image-warning-banner {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 8px;
    padding: 8px 12px;
    border-radius: 8px;
    background: rgba(245, 158, 11, 0.12);
    border: 1px solid rgba(245, 158, 11, 0.3);
    margin-top: 8px;
  }

  .chat-image-warning-info {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 11.5px;
    color: #fbbf24;
    line-height: 1.35;
    flex: 1;
    min-width: 180px;
  }

  .chat-image-warning-info svg {
    flex-shrink: 0;
    color: #f59e0b;
  }

  .chat-image-warning-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 10px;
    border-radius: 6px;
    background: rgba(245, 158, 11, 0.22);
    border: 1px solid rgba(245, 158, 11, 0.45);
    color: #fbbf24;
    font-size: 11.5px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.15s ease;
  }

  .chat-image-warning-btn:hover {
    background: rgba(245, 158, 11, 0.35);
    border-color: #f59e0b;
    color: #ffffff;
  }

  /* Lightbox Modal */
  .lightbox-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.88);
    backdrop-filter: blur(8px);
    z-index: 99999;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
    animation: fadeIn 0.2s ease;
  }

  .lightbox-container {
    position: relative;
    max-width: 90vw;
    max-height: 90vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    overflow: hidden;
    box-shadow: 0 16px 48px rgba(0, 0, 0, 0.5);
  }

  .lightbox-close-btn {
    position: absolute;
    top: 12px;
    right: 12px;
    z-index: 10;
    width: 36px;
    height: 36px;
    border-radius: 50%;
    background: rgba(15, 23, 42, 0.7);
    border: 1px solid rgba(255, 255, 255, 0.2);
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.2s ease;
  }

  .lightbox-close-btn:hover {
    background: rgba(239, 68, 68, 0.85);
  }

  .lightbox-img {
    max-width: 100%;
    max-height: calc(90vh - 70px);
    object-fit: contain;
    display: block;
    background: #000;
  }

  .lightbox-bar {
    width: 100%;
    padding: 10px 18px;
    background: var(--bg-card);
    border-top: 1px solid var(--border-color);
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 14px;
    box-sizing: border-box;
  }

  .lightbox-title {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .lightbox-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }

  .lightbox-action-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 500;
    background: var(--bg-hover);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .lightbox-action-btn:hover {
    background: var(--border-color);
    color: var(--accent-blue);
  }

  /* Image Generator Toolpanel */
  .image-generator-panel {
    width: 100%;
    margin-bottom: 8px;
    background: var(--pill-bg);
    border: 1px solid var(--border-color);
    border-radius: 20px;
    padding: 10px 14px;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.2);
    backdrop-filter: blur(16px);
    display: flex;
    flex-direction: column;
    gap: 10px;
    animation: slideUpFade 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    box-sizing: border-box;
  }

  @keyframes slideUpFade {
    from {
      opacity: 0;
      transform: translateY(8px);
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  .gen-panel-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    flex-wrap: wrap;
  }

  .gen-styles-group {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .gen-section-label {
    font-size: 0.76rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--text-muted);
  }

  .gen-styles-pills {
    display: flex;
    align-items: center;
    gap: 4px;
    background: rgba(255, 255, 255, 0.04);
    padding: 3px;
    border-radius: 12px;
    border: 1px solid var(--border-subtle);
  }

  .gen-style-pill {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    font-size: 0.78rem;
    font-weight: 500;
    padding: 4px 10px;
    border-radius: 9px;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .gen-style-pill:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.08);
  }

  .gen-style-pill.active,
  .gen-style-pill.active:hover,
  .gen-style-pill.active:focus {
    background: var(--accent-blue);
    color: #ffffff !important;
    box-shadow: 0 2px 8px rgba(41, 150, 204, 0.35);
  }

  .gen-panel-controls {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-left: auto;
  }

  .gen-toggle-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    padding: 4px 8px;
    border-radius: 10px;
    border: 1px solid transparent;
    transition: all 0.15s ease;
    user-select: none;
  }

  .gen-toggle-btn:hover {
    background: rgba(255, 255, 255, 0.05);
  }

  .gen-toggle-btn.active {
    background: rgba(41, 150, 204, 0.12);
    border-color: rgba(41, 150, 204, 0.3);
  }

  .gen-checkbox-hidden {
    position: absolute;
    opacity: 0;
    pointer-events: none;
    margin: 0;
  }

  .gen-toggle-check {
    width: 14px;
    height: 14px;
    border-radius: 4px;
    border: 1.5px solid var(--border-color);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s ease;
  }

  .gen-toggle-btn.active .gen-toggle-check {
    background: var(--accent-blue);
    border-color: var(--accent-blue);
    color: #ffffff;
  }

  .gen-toggle-text {
    font-size: 0.78rem;
    color: var(--text-secondary);
    font-weight: 500;
  }

  .gen-toggle-btn.active .gen-toggle-text {
    color: var(--text-primary);
  }

  .gen-icon-btn {
    background: transparent;
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    width: 28px;
    height: 28px;
    border-radius: 8px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .gen-icon-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
    border-color: var(--text-muted);
  }

  .gen-icon-btn.active {
    background: rgba(41, 150, 204, 0.15);
    color: var(--accent-blue);
    border-color: var(--accent-blue);
  }

  .gen-close-btn:hover {
    color: #ef4444;
    border-color: #ef4444;
  }

  .gen-panel-loras-row {
    padding-top: 8px;
    border-top: 1px solid var(--border-subtle);
    display: flex;
    align-items: center;
    gap: 12px;
    flex-wrap: wrap;
  }

  .gen-loras-group {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .gen-lora-chips {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }

  .gen-lora-chip {
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    font-size: 0.74rem;
    font-weight: 500;
    padding: 3px 9px;
    border-radius: 12px;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .gen-lora-chip:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.1);
    border-color: var(--text-muted);
  }

  .gen-lora-chip.active {
    background: rgba(41, 150, 204, 0.18);
    border-color: var(--accent-blue);
    color: var(--accent-blue);
    font-weight: 600;
  }

  .gen-divider {
    width: 1px;
    height: 18px;
    background: var(--border-subtle);
  }

  /* Prompts History Popup */
  .prompts-popup {
    position: absolute;
    bottom: calc(100% + 10px);
    left: 0;
    right: 0;
    max-width: 820px;
    margin: 0 auto;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 18px;
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.4);
    backdrop-filter: blur(24px);
    z-index: 60;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    animation: popIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  .prompts-popup-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 16px;
    border-bottom: 1px solid var(--border-subtle);
  }

  .prompts-header-title {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--text-primary);
  }

  .prompts-popup-close {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 4px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 6px;
    transition: color 0.15s ease;
  }

  .prompts-popup-close:hover {
    color: var(--text-primary);
  }

  .prompts-search-wrap {
    padding: 8px 14px;
    border-bottom: 1px solid var(--border-subtle);
  }

  .prompts-search-input {
    width: 100%;
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid var(--border-subtle);
    border-radius: 8px;
    padding: 6px 10px;
    font-size: 0.8rem;
    color: var(--text-primary);
    outline: none;
    box-sizing: border-box;
  }

  .prompts-search-input:focus {
    border-color: var(--accent-blue);
  }

  .prompts-popup-list {
    max-height: 280px;
    overflow-y: auto;
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .prompt-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 8px 12px;
    border-radius: 10px;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .prompt-item:hover {
    background: var(--bg-card-hover);
  }

  .prompt-item-content {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .prompt-item-text {
    font-size: 0.84rem;
    color: var(--text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .prompt-item-meta {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.72rem;
    color: var(--text-muted);
  }

  .prompt-style-badge {
    background: rgba(41, 150, 204, 0.12);
    color: var(--accent-blue);
    padding: 1px 6px;
    border-radius: 4px;
    font-weight: 500;
    text-transform: capitalize;
  }

  .prompt-delete-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 6px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0;
    transition: all 0.15s ease;
  }

  .prompt-item:hover .prompt-delete-btn {
    opacity: 1;
  }

  .prompt-delete-btn:hover {
    color: #ef4444;
    background: rgba(239, 68, 68, 0.12);
  }

  .prompts-empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 32px 16px;
    color: var(--text-muted);
    font-size: 0.82rem;
  }

  @media (max-width: 768px) {
    .suggestion-grid {
      grid-template-columns: 1fr;
    }
    .hero-greeting {
      font-size: 1.7rem;
    }
    .messages-viewport {
      padding: 16px 12px 130px;
    }
    .user-bubble-wrapper {
      max-width: 88%;
    }
    .image-generator-panel {
      padding: 8px 10px;
      border-radius: 16px;
    }
    .gen-panel-row {
      flex-direction: column;
      align-items: flex-start;
      gap: 8px;
    }
    .gen-panel-controls {
      width: 100%;
      justify-content: space-between;
      margin-left: 0;
    }
    .prompts-popup {
      left: 8px;
      right: 8px;
    }
  }

  /* /learn source chooser — same look as the folder-index chooser in ChatApplet */
  .source-chooser-backdrop {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(15, 17, 26, 0.8);
    display: flex;
    justify-content: center;
    align-items: center;
    padding: 20px;
    z-index: 1100;
    backdrop-filter: blur(4px);
  }

  .source-chooser {
    background: var(--bg-secondary, #18242B);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 14px;
    width: min(460px, 100%);
    max-height: 80vh;
    overflow: auto;
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }

  .source-chooser-header h3 {
    margin: 0 0 6px;
    font-size: 16px;
  }

  .source-chooser-header p {
    margin: 0;
    font-size: 12px;
    line-height: 1.5;
    opacity: 0.7;
  }

  .source-list {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .source-item {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    padding: 10px 12px;
    border-radius: 10px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    background: var(--bg-primary, #111a20);
    color: inherit;
    cursor: pointer;
    text-align: left;
  }

  .source-item:hover { border-color: var(--accent-color, #4a9eff); }

  .source-icon { display: inline-flex; opacity: 0.8; }

  .source-name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 14px;
  }

  .source-badge {
    font-size: 11px;
    opacity: 0.65;
    white-space: nowrap;
  }

  .source-chooser-actions {
    display: flex;
    gap: 8px;
    justify-content: flex-end;
    flex-wrap: wrap;
  }

  .source-cancel {
    padding: 8px 14px;
    border-radius: 8px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    background: transparent;
    color: inherit;
    cursor: pointer;
    font-size: 13px;
  }
</style>
