import { Capacitor, registerPlugin } from "@capacitor/core";
import { getViewerSettings } from "./viewer-settings.js";

// Register NativeStorage Capacitor plugin
export const NativeStorage = registerPlugin("NativeStorage");

export function isNativeStorageAvailable() {
  return typeof window !== "undefined" && Capacitor.isNativePlatform() && Capacitor.isPluginAvailable("NativeStorage");
}

async function base64ToBlob(base64, mimeType = "image/jpeg") {
  try {
    const res = await fetch(`data:${mimeType};base64,${base64}`);
    return await res.blob();
  } catch (_) {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return new Blob([bytes], { type: mimeType });
  }
}

/**
 * NativeStorageDrive — implements the OnMyDisk Drive interface
 * for real mobile storage on Android / iOS via Capacitor NativeStoragePlugin.
 */
export class NativeStorageDrive {
  constructor() {
    this.isConnected = true;
    this.dc = { readyState: "open" };
    this.isNative = true;
  }

  async reconnect() {
    this.isConnected = true;
    return true;
  }

  /**
   * Device root ("/") returns top-level roots (Camera, Pictures, Documents, Downloads).
   * Deeper paths list files from the physical storage directory.
   */
  async getFiles(folderId = "/", opts = {}) {
    if (!isNativeStorageAvailable()) return [];

    const path = folderId === "" ? "/" : folderId;
    if (path === "/") {
      const res = await NativeStorage.listRoots();
      const roots = (res && res.roots) || [];
      const items = roots.map((r) => ({
        id: r.id,
        value: r.name,
        name: r.name,
        type: "folder",
        isDir: true,
        size: 0,
        date: r.date ? new Date(r.date) : new Date(),
        lazy: true,
        mimeType: "",
        realPath: r.id,
        isRoot: true,
      }));
      items.realPath = "/";
      return items;
    }

    const res = await NativeStorage.list({ path });
    const files = (res && res.files) || [];
    const items = files.map((f) => {
      const isDir = !!f.isDir;
      const mime = f.type || "";
      const nameLower = (f.name || "").toLowerCase();
      const isMedia = !isDir && (
        mime.startsWith("image/") || mime.startsWith("video/") ||
        nameLower.endsWith(".jpg") || nameLower.endsWith(".jpeg") ||
        nameLower.endsWith(".png") || nameLower.endsWith(".gif") ||
        nameLower.endsWith(".webp") || nameLower.endsWith(".mp4") ||
        nameLower.endsWith(".mkv") || nameLower.endsWith(".mov") ||
        nameLower.endsWith(".avi") || nameLower.endsWith(".3gp") ||
        nameLower.endsWith(".webm")
      );
      const cleanId = f.id.startsWith("/") ? f.id : "/" + f.id;
      const p2pUrl = `/p2p/local${cleanId}`;

      return {
        id: f.id,
        value: f.name,
        name: f.name,
        type: isDir ? "folder" : (f.type || "file"),
        isDir,
        size: f.size || 0,
        date: f.date ? new Date(f.date) : new Date(),
        lazy: isDir,
        mimeType: f.type || "",
        realPath: f.id,
        ...(() => {
          const vs = getViewerSettings();
          if (!isMedia || !vs.generateThumbs) return {};
          if (vs.downscaleImages) {
            return {
              preview: `${p2pUrl}?resize=true&width=360&height=360`,
              image: `${p2pUrl}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}`,
              thumb: `${p2pUrl}?resize=true&width=360&height=360`,
            };
          }
          return { preview: p2pUrl, image: p2pUrl, thumb: p2pUrl };
        })()
      };
    });

    items.sort((a, b) =>
      a.isDir === b.isDir ? a.name.localeCompare(b.name) : a.isDir ? -1 : 1
    );
    items.realPath = path;
    return items;
  }

  async resolvePath(path = "") {
    return { realPath: path || "/" };
  }

  async mkDir(virtualPath) {
    return this.homeMkDir(virtualPath);
  }

  async homeMkDir(virtualPath) {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");
    await NativeStorage.mkdir({ path: virtualPath });
    const name = virtualPath.split("/").filter(Boolean).pop() || "";
    return { path: virtualPath, name };
  }

  async homeWrite(virtualPath, dataBase64) {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");
    const res = await NativeStorage.writeChunk({
      path: virtualPath,
      offset: 0,
      data: dataBase64,
      append: false,
    });
    const name = virtualPath.split("/").filter(Boolean).pop() || "";
    return { path: virtualPath, name, size: res.bytesWritten || 0 };
  }

  _parseThumbDims(query) {
    const vs = getViewerSettings();
    let width = 360;
    let height = 360;
    if (query) {
      const matchW = query.match(/width=(\d+)/);
      const matchH = query.match(/height=(\d+)/);
      if (matchW) width = parseInt(matchW[1], 10);
      if (matchH) height = parseInt(matchH[1], 10);
      else if (matchW) height = width;
      const matchCover = query.match(/cover=(\d+)/);
      if (matchCover) {
        width = parseInt(matchCover[1], 10);
        height = width;
      }
    } else {
      width = vs.scaleTo || 1200;
      height = width;
    }
    return { width, height };
  }

  async open(virtualPath, query = "") {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");
    const isThumb = query && (query.includes("resize=true") || query.includes("thumbnail=true") || query.includes("cover=") || query.includes("poster"));
    if (isThumb) {
      try {
        const { width, height } = this._parseThumbDims(query);
        const thumbRes = await NativeStorage.getThumbnail({
          path: virtualPath,
          width,
          height,
        });
        if (thumbRes && thumbRes.data) {
          const mimeType = thumbRes.mimeType || "image/jpeg";
          const blob = await base64ToBlob(thumbRes.data, mimeType);
          this._cachedThumbBlob = {
            key: `${virtualPath}?${query}`,
            blob
          };
          return {
            name: virtualPath.split("/").pop() || "",
            path: virtualPath,
            isDir: false,
            size: blob.size,
            mimeType,
          };
        }
      } catch (_) {}
    }
    const stat = await NativeStorage.stat({ path: virtualPath });
    if (!stat || !stat.exists) throw new Error("File not found: " + virtualPath);
    return {
      name: stat.name,
      path: virtualPath,
      isDir: stat.isDir,
      size: stat.size || 0,
      mimeType: stat.type || "application/octet-stream",
    };
  }

  async read(virtualPath, offset = 0, size = 65536, query = "") {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");
    const res = await NativeStorage.readChunk({
      path: virtualPath,
      offset,
      length: size,
    });
    return {
      data: res.data || "",
      size: res.bytesRead || 0,
      totalSize: res.totalSize || 0,
      eof: !!res.eof,
    };
  }

  async readRange(virtualPath, offset = 0, size = 0, query = "") {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");
    const isThumb = query && (query.includes("resize=true") || query.includes("thumbnail=true") || query.includes("cover=") || query.includes("poster"));
    if (isThumb) {
      const cacheKey = `${virtualPath}?${query}`;
      if (this._cachedThumbBlob && this._cachedThumbBlob.key === cacheKey) {
        const b = this._cachedThumbBlob.blob;
        const buf = await b.arrayBuffer();
        const bytes = new Uint8Array(buf);
        return { bytes, totalSize: bytes.length, eof: true };
      }
      try {
        const { width, height } = this._parseThumbDims(query);
        const thumbRes = await NativeStorage.getThumbnail({
          path: virtualPath,
          width,
          height,
        });
        if (thumbRes && thumbRes.data) {
          const b = await base64ToBlob(thumbRes.data, thumbRes.mimeType || "image/jpeg");
          const buf = await b.arrayBuffer();
          const bytes = new Uint8Array(buf);
          return { bytes, totalSize: bytes.length, eof: true };
        }
      } catch (_) {}
    }
    const toRead = size > 0 ? size : 4 * 1024 * 1024;
    const res = await NativeStorage.readChunk({
      path: virtualPath,
      offset,
      length: toRead,
    });
    const base64 = res.data || "";
    let bytes = new Uint8Array(0);
    if (base64) {
      const b = await base64ToBlob(base64, "application/octet-stream");
      const buf = await b.arrayBuffer();
      bytes = new Uint8Array(buf);
    }
    return {
      bytes,
      totalSize: res.totalSize || bytes.length,
      eof: !!res.eof,
    };
  }

  async getFileContent(virtualPath, totalSize = 0, onProgress = null, startOffset = 0, query = "") {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");

    // Fast thumbnail for image/video preview in cards/grid
    const isThumb = query && (query.includes("resize=true") || query.includes("thumbnail=true") || query.includes("cover=") || query.includes("poster"));
    if (isThumb) {
      const cacheKey = `${virtualPath}?${query}`;
      if (this._cachedThumbBlob && this._cachedThumbBlob.key === cacheKey) {
        const b = this._cachedThumbBlob.blob;
        this._cachedThumbBlob = null; // consume
        return b;
      }
      try {
        const { width, height } = this._parseThumbDims(query);
        const thumbRes = await NativeStorage.getThumbnail({
          path: virtualPath,
          width,
          height,
        });
        if (thumbRes && thumbRes.data) {
          return await base64ToBlob(thumbRes.data, thumbRes.mimeType || "image/jpeg");
        }
      } catch (_) {}
    }

    const { bytes } = await this.readRange(virtualPath, startOffset, totalSize, query);
    return new Blob([bytes]);
  }

  async getThumbnail(virtualPath, width = 256, height = 256) {
    if (!isNativeStorageAvailable()) return null;
    try {
      const thumbRes = await NativeStorage.getThumbnail({
        path: virtualPath,
        width,
        height,
      });
      if (thumbRes && thumbRes.data) {
        return `data:${thumbRes.mimeType || "image/jpeg"};base64,${thumbRes.data}`;
      }
    } catch (_) {}
    return null;
  }

  async delete(virtualPath) {
    if (!isNativeStorageAvailable()) throw new Error("Native storage unavailable");
    return NativeStorage.delete({ path: virtualPath });
  }

  async listMedia(type = "images", limit = 500, offset = 0) {
    if (!isNativeStorageAvailable()) return { items: [], count: 0 };
    return NativeStorage.listMedia({ limit, offset });
  }

  async clearCache() {
    if (isNativeStorageAvailable() && typeof NativeStorage.clearCache === "function") {
      try {
        await NativeStorage.clearCache();
      } catch (_) {}
    }
  }
}

export function onStorageChanged(callback) {
  if (!isNativeStorageAvailable()) return Promise.resolve({ remove: () => {} });
  return NativeStorage.addListener("storageChanged", callback);
}
