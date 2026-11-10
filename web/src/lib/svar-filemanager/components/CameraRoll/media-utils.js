export const IMAGE_EXTS = new Set([
	"jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "tiff", "tif",
	"heic", "heif", "avif", "ico", "raw", "cr2", "nef", "dng", "arw"
]);

export const VIDEO_EXTS = new Set([
	"mp4", "mkv", "mov", "avi", "webm", "wmv", "flv", "m4v", "3gp",
	"ts", "mts", "mpeg", "mpg", "ogv", "vob"
]);

export const IGNORED_NAMES = new Set([
	"thumbs.db", "desktop.ini", "ehthumbs.db", "ehthumbs_vista.db",
	".ds_store", "ds_store", ".nomedia", "nomedia", ".trashed", "trashed"
]);

export function isAssociatedReadme(file) {
	if (!file) return false;
	const name = (file.name || file.value || "").trim();
	if (name === "Readme.md" || name === "README.md") return true;
	if (name.toLowerCase().endsWith(".readme.md")) return true;
	return false;
}

export function isIgnoredFile(file) {
	if (!file) return true;
	if (file.id === "/wx-filemanager-parent-link") return true;
	const rawName = (file.name || file.value || file.id || "").trim();
	const name = rawName.includes("/") ? rawName.split("/").filter(Boolean).pop() : rawName;
	if (!name || name.startsWith(".")) return true;
	if (IGNORED_NAMES.has(name.toLowerCase())) return true;
	if (isAssociatedReadme(file)) return true;
	return false;
}

export function isImageOrVideo(file) {
	if (!file) return false;
	if (file.type === "folder" || file.isDir || file.isDevice || file.itemType === "device" || file.itemType === "app") {
		return false;
	}
	if (file.type === "image" || file.type === "video") return true;
	const mime = (file.mimeType || file.type || "").toLowerCase();
	if (mime.startsWith("image/") || mime.startsWith("video/")) return true;
	const name = (file.name || file.value || file.id || "").trim();
	const ext = (file.ext || (name.includes(".") ? name.split(".").pop() : "")).toLowerCase();
	return IMAGE_EXTS.has(ext) || VIDEO_EXTS.has(ext);
}

export function isVideoFile(file) {
	if (!file) return false;
	if (file.type === "video") return true;
	const mime = (file.mimeType || file.type || "").toLowerCase();
	if (mime.startsWith("video/")) return true;
	const name = (file.name || file.value || file.id || "").trim();
	const ext = (file.ext || (name.includes(".") ? name.split(".").pop() : "")).toLowerCase();
	return VIDEO_EXTS.has(ext);
}

export function isCameraRollFolder(files) {
	if (!files || !files.length) return false;
	const realFiles = files.filter(f => !isIgnoredFile(f));
	if (!realFiles.length) return false;
	const mediaFiles = realFiles.filter(f => isImageOrVideo(f));
	if (!mediaFiles.length) return false;
	// All non-folder files must be image or video (subfolders are allowed and displayed as album chips)
	const nonFolderFiles = realFiles.filter(
		f => f.type !== "folder" && !f.isDir && !f.isDevice && f.itemType !== "device" && f.itemType !== "app"
	);
	if (!nonFolderFiles.length) return false;
	return nonFolderFiles.every(f => isImageOrVideo(f));
}

export function formatDuration(val) {
	if (!val) return "";
	if (typeof val === "string" && val.includes(":")) return val;
	const num = Number(val);
	if (isNaN(num) || num <= 0) return "";
	const mins = Math.floor(num / 60);
	const secs = Math.floor(num % 60);
	return `${mins}:${secs < 10 ? "0" : ""}${secs}`;
}
