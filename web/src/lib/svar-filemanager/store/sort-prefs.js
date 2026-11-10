// Per-folder sort preferences for the SVAR filemanager.
// Stores the user's sort choice (key + order) per folder path in localStorage.
// Folders without a stored choice fall back to view defaults
// (Camera Roll: newest first; other views: name ascending).

const STORAGE_KEY = "omd_fm_sort_prefs";

// Global defaults (used when a folder has no stored preference)
export const SORT_DEFAULTS = { key: "name", order: "asc" };

// Camera Roll default: newest first
export const CAMERAROLL_DEFAULTS = { key: "date", order: "desc" };

let cache = null;

function load() {
	if (cache) return cache;
	try {
		cache = JSON.parse(localStorage.getItem(STORAGE_KEY) || "{}") || {};
	} catch (_) {
		cache = {};
	}
	return cache;
}

function save(prefs) {
	cache = prefs;
	try {
		localStorage.setItem(STORAGE_KEY, JSON.stringify(prefs));
	} catch (_) {}
}

/**
 * Get the sort for a folder.
 * @param {string} path folder path ("/", "/device/folder", ...)
 * @param {{key: string, order: string}} [defaults] view defaults override
 * @returns {{key: string, order: string, custom: boolean}}
 */
export function getFolderSort(path, defaults = SORT_DEFAULTS) {
	const prefs = load();
	const stored = prefs[path];
	if (stored && stored.key && (stored.order === "asc" || stored.order === "desc")) {
		return { ...stored, custom: true };
	}
	return { ...(defaults || SORT_DEFAULTS), custom: false };
}

/**
 * Store the sort for a folder.
 * @param {string} path folder path
 * @param {string} key "name" | "size" | "date"
 * @param {string} order "asc" | "desc"
 */
export function setFolderSort(path, key, order) {
	const prefs = load();
	prefs[path] = { key, order };
	save(prefs);
}

export const SORT_FIELDS = ["name", "size", "date"];
