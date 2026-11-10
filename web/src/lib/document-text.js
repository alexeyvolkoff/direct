/**
 * Загрузка документа для RAG-импорта: сырые байты документа аплоадятся на
 * AI-ноду по P2P (HttpDrive), которая конвертирует локально (pdftotext/pandoc),
 * векторизует и возвращает payload. Временный файл на ноде удаляется.
 * Никакого шлюза.
 */
const CONVERT_EXTS = ["pdf", "docx", "odt", "epub", "fb2", "csv", "html", "htm", "md", "markdown", "rst", "rtf"];

// accept-атрибут для системного file input
const FILE_INPUT_ACCEPT =
  ".pdf,.docx,.doc,.odt,.rtf,.txt,.md,.markdown,.html,.htm,.csv,.epub,.fb2,application/pdf";

async function postRawImport(httpDrive, u8, filename, source, opts = {}) {
  // Кириллица в HTTP-заголовках недопустима — имя всегда URL-encoded
  const headers = { Accept: "application/json", "X-OMD-Filename": encodeURIComponent(filename) };
  if (source) headers["X-OMD-Source"] = encodeURIComponent(source);
  // Полный логический путь /<устройство>/<путь> → document_id карточки.
  // Отдельно от X-OMD-Source: source — путь относительно устройства-источника.
  if (opts.docPath) headers["X-OMD-Doc-Path"] = encodeURIComponent(opts.docPath);
  if (opts.scope) headers["X-OMD-RAG-Scope"] = opts.scope;
  if (opts.owner) headers["X-OMD-Owner"] = opts.owner;
  if (opts.tags && opts.tags.length) headers["X-OMD-Tags"] = JSON.stringify(opts.tags.slice(0, 10));

  const res = await httpDrive.fetch("/rag/import/raw", {
    method: "POST",
    headers,
    body: u8,
  });

  let data = null;
  try {
    data = await res.json();
  } catch (_) {
    data = null;
  }

  if (!res.ok || !data) {
    const msg = (data && (data.error?.message || data.error || data.detail)) || `import failed (${res.status})`;
    throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
  }
  return data;
}

function base64ToUint8(b64) {
  if (!b64) return null;
  const bin = atob(b64.replace(/\s+/g, ""));
  const u8 = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
  return u8;
}

function concatBytes(parts) {
  if (!parts.length) return null;
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

async function readFileBytes(drive, source) {
  if (!drive) throw new Error("No device drive to read the document");

  // 1) LocalDrive/NativeDrive: readFile(path) → whole file as base64
  if (typeof drive.readFile === "function" && drive.readFile.length <= 1) {
    try {
      const res = await drive.readFile(source);
      const u8 = res && res.data ? base64ToUint8(res.data) : (typeof res === "string" ? base64ToUint8(res) : null);
      if (u8 && u8.length) return u8;
    } catch (_) {}
  }

  // 2) LocalDrive/NativeDrive: getFileContent(path) → Blob/File (whole file, no size arg required)
  if (typeof drive.getFileContent === "function" && drive.getFileContent.length <= 1) {
    try {
      const blob = await drive.getFileContent(source);
      if (blob && blob.arrayBuffer) {
        const u8 = new Uint8Array(await blob.arrayBuffer());
        if (u8.length) return u8;
      }
    } catch (_) {}
  }

  // 3) WebRTCDataChannelDrive: readFile(path, offset, size) → base64 chunk
  if (typeof drive.readFile === "function" && drive.readFile.length >= 3) {
    try {
      const parts = [];
      let offset = 0;
      const CH = 65536;
      while (true) {
        const b64 = await drive.readFile(source, offset, CH);
        const u8 = base64ToUint8(b64);
        if (!u8 || !u8.length) break;
        parts.push(u8);
        offset += u8.length;
        if (u8.length < CH) break;
      }
      if (parts.length) return concatBytes(parts);
    } catch (_) {}
  }

  throw new Error("Could not read file content from device drive");
}

/**
 * Импорт файла, выбранного СИСТЕМНЫМ file picker'ом (File/Blob уже в руках).
 * @param {File|Blob} file
 * @returns payload /rag/import/raw
 */
export async function importUploadedFile(httpDrive, file, opts = {}) {
  if (!httpDrive) throw new Error("AI node is not reachable (no httpDrive)");
  if (!file) throw new Error("No file selected");

  const filename = file.name || "document.bin";
  const u8 = new Uint8Array(await file.arrayBuffer());
  if (!u8.length) throw new Error("Document is empty");

  return postRawImport(httpDrive, u8, filename, filename, opts);
}

/**
 * Импорт документа, лежащего на устройстве (по пути через device drive).
 */
export async function uploadDocumentForImport(httpDrive, deviceDrive, source, opts = {}) {
  if (!httpDrive) throw new Error("AI node is not reachable (no httpDrive)");
  if (!source || !source.trim()) throw new Error("Source is empty");

  const filename = source.split("/").pop()?.split("?")[0] || "document.bin";
  const u8 = await readFileBytes(deviceDrive, source.trim());
  if (!u8 || u8.length === 0) throw new Error("Document is empty");

  return postRawImport(httpDrive, u8, filename, source.trim(), opts);
}

// ─── /recognize (vision-распознавание изображения) ─────────────────────────

async function postRawRecognize(httpDrive, u8, filename, source, opts = {}) {
  const headers = { Accept: "application/json", "X-OMD-Filename": encodeURIComponent(filename) };
  if (source) headers["X-OMD-Source"] = encodeURIComponent(source);
  // Полный логический путь /<устройство>/<путь> → document_id карточки.
  // Отдельно от X-OMD-Source: source — путь относительно устройства-источника.
  if (opts.docPath) headers["X-OMD-Doc-Path"] = encodeURIComponent(opts.docPath);
  if (opts.scope) headers["X-OMD-RAG-Scope"] = opts.scope;
  if (opts.owner) headers["X-OMD-Owner"] = opts.owner;
  if (opts.prompt) headers["X-OMD-Prompt"] = opts.prompt;
  if (opts.tags && opts.tags.length) headers["X-OMD-Tags"] = JSON.stringify(opts.tags.slice(0, 10));

  const res = await httpDrive.fetch("/rag/recognize/raw", {
    method: "POST",
    headers,
    body: u8,
  });

  let data = null;
  try {
    data = await res.json();
  } catch (_) {
    data = null;
  }

  if (!res.ok || !data) {
    const msg = (data && (data.error?.message || data.error || data.detail)) || `recognition failed (${res.status})`;
    throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
  }
  return data;
}

/**
 * Распознавание картинки, выбранной СИСТЕМНЫМ file picker'ом (File/Blob в руках).
 */
export async function recognizeUploadedImage(httpDrive, file, opts = {}) {
  if (!httpDrive) throw new Error("AI node is not reachable (no httpDrive)");
  if (!file) throw new Error("No image selected");

  const filename = file.name || "image.png";
  const u8 = new Uint8Array(await file.arrayBuffer());
  if (!u8.length) throw new Error("Image is empty");

  return postRawRecognize(httpDrive, u8, filename, filename, opts);
}

/**
 * Распознавание картинки, лежащей на устройстве (по пути через device drive).
 */
export async function uploadImageForRecognize(httpDrive, deviceDrive, source, opts = {}) {
  if (!httpDrive) throw new Error("AI node is not reachable (no httpDrive)");
  if (!source || !source.trim()) throw new Error("Source is empty");

  const filename = source.split("/").pop()?.split("?")[0] || "image.png";
  const u8 = await readFileBytes(deviceDrive, source.trim());
  if (!u8 || u8.length === 0) throw new Error("Image is empty");

  return postRawRecognize(httpDrive, u8, filename, source.trim(), opts);
}

export { CONVERT_EXTS, FILE_INPUT_ACCEPT };
