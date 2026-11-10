import { writable, derived } from "svelte/store";

// Import all 19 locale dictionaries
import en from "./locales/en.json";
import ru from "./locales/ru.json";
import sl from "./locales/sl.json";
import de from "./locales/de.json";
import es from "./locales/es.json";
import fr from "./locales/fr.json";
import it from "./locales/it.json";
import nl from "./locales/nl.json";
import pl from "./locales/pl.json";
import pt from "./locales/pt.json";
import tr from "./locales/tr.json";
import uk from "./locales/uk.json";
import hr from "./locales/hr.json";
import sr from "./locales/sr.json";
import sk from "./locales/sk.json";
import zh_cn from "./locales/zh_cn.json";
import zh_tw from "./locales/zh_tw.json";
import he from "./locales/he.json";
import fa from "./locales/fa.json";

export const LOCALES = {
  en,
  ru,
  sl,
  de,
  es,
  fr,
  it,
  nl,
  pl,
  pt,
  tr,
  uk,
  hr,
  sr,
  sk,
  zh_cn,
  zh_tw,
  he,
  fa
};

export const SUPPORTED_LANGUAGES = [
  { code: "en", shortCode: "EN", name: "English", nativeName: "English", dir: "ltr" },
  { code: "ru", shortCode: "RU", name: "Russian", nativeName: "Русский", dir: "ltr" },
  { code: "sl", shortCode: "SI", name: "Slovenian", nativeName: "Slovenščina", dir: "ltr" },
  { code: "de", shortCode: "DE", name: "German", nativeName: "Deutsch", dir: "ltr" },
  { code: "es", shortCode: "ES", name: "Spanish", nativeName: "Español", dir: "ltr" },
  { code: "fr", shortCode: "FR", name: "French", nativeName: "Français", dir: "ltr" },
  { code: "it", shortCode: "IT", name: "Italian", nativeName: "Italiano", dir: "ltr" },
  { code: "nl", shortCode: "NL", name: "Dutch", nativeName: "Nederlands", dir: "ltr" },
  { code: "pl", shortCode: "PL", name: "Polish", nativeName: "Polski", dir: "ltr" },
  { code: "pt", shortCode: "PT", name: "Portuguese", nativeName: "Português", dir: "ltr" },
  { code: "uk", shortCode: "UA", name: "Ukrainian", nativeName: "Українська", dir: "ltr" },
  { code: "tr", shortCode: "TR", name: "Turkish", nativeName: "Türkçe", dir: "ltr" },
  { code: "hr", shortCode: "HR", name: "Croatian", nativeName: "Hrvatski", dir: "ltr" },
  { code: "sr", shortCode: "SR", name: "Serbian", nativeName: "Srpski", dir: "ltr" },
  { code: "sk", shortCode: "SK", name: "Slovak", nativeName: "Slovenčina", dir: "ltr" },
  { code: "zh_cn", shortCode: "ZH", name: "Chinese (Simplified)", nativeName: "简体中文", dir: "ltr" },
  { code: "zh_tw", shortCode: "TW", name: "Chinese (Traditional)", nativeName: "繁體中文", dir: "ltr" },
  { code: "he", shortCode: "HE", name: "Hebrew", nativeName: "עברית", dir: "rtl" },
  { code: "fa", shortCode: "FA", name: "Persian", nativeName: "فارسی", dir: "rtl" }
];

function detectInitialLocale() {
  if (typeof window === "undefined") return "en";

  // 1. URL Query Parameter (?lang=ru or ?lang=sl)
  try {
    const urlParams = new URLSearchParams(window.location.search);
    const qLang = urlParams.get("lang");
    if (qLang) {
      const normalized = normalizeLangCode(qLang);
      if (LOCALES[normalized]) return normalized;
    }
  } catch (e) {}

  // 2. localStorage ('omd_locale' or 'omd_lang')
  try {
    const saved = localStorage.getItem("omd_locale") || localStorage.getItem("omd_lang");
    if (saved) {
      const normalized = normalizeLangCode(saved);
      if (LOCALES[normalized]) return normalized;
    }
  } catch (e) {}

  // 3. Browser Navigator Language (e.g. 'ru-RU' -> 'ru', 'zh-CN' -> 'zh_cn')
  try {
    const navLang = navigator.language || navigator.userLanguage;
    if (navLang) {
      const normalized = normalizeLangCode(navLang);
      if (LOCALES[normalized]) return normalized;
    }
  } catch (e) {}

  return "en";
}

function normalizeLangCode(code) {
  if (!code) return "en";
  const c = code.toLowerCase().trim().replace("-", "_");
  if (LOCALES[c]) return c;
  const primary = c.split("_")[0];
  if (primary === "si") return "sl";
  if (primary === "ua") return "uk";
  if (c.startsWith("zh")) {
    return c.includes("tw") || c.includes("hk") ? "zh_tw" : "zh_cn";
  }
  if (LOCALES[primary]) return primary;
  return "en";
}

export const locale = writable(detectInitialLocale());

// Keep HTML document lang & dir attributes in sync with active locale
if (typeof window !== "undefined") {
  locale.subscribe(code => {
    try {
      localStorage.setItem("omd_locale", code);
      localStorage.setItem("omd_lang", code);
      document.documentElement.lang = code;
      const meta = SUPPORTED_LANGUAGES.find(l => l.code === code);
      document.documentElement.dir = (meta && meta.dir) || "ltr";
    } catch (e) {}
  });
}

export function setLocale(newLocale) {
  const normalized = normalizeLangCode(newLocale);
  locale.set(normalized);
}

/**
 * Translate a key into the active locale with parameter interpolation.
 * @param {string} key - The translation key.
 * @param {Object} [params] - Optional named parameters for substitution e.g. { name: "Doc" }.
 * @param {string} [fallback] - Fallback text if key not found.
 */
export function translate(key, params = {}, fallback = "") {
  if (!key) return "";
  let currentCode = "en";
  const unsub = locale.subscribe(val => { currentCode = val; });
  unsub();

  const dict = LOCALES[currentCode] || LOCALES.en;
  let text = dict[key];
  if (text === undefined) {
    text = LOCALES.en[key] !== undefined ? LOCALES.en[key] : (fallback || key);
  }

  if (typeof text !== "string") return text;

  // Parameter interpolation: {name} or {{name}} or %name%
  if (params && typeof params === "object") {
    for (const [k, v] of Object.entries(params)) {
      text = text.replace(new RegExp(`\\{\\{?\\s*${k}\\s*\\}?\}`, "g"), String(v));
      text = text.replace(new RegExp(`%${k}%`, "g"), String(v));
    }
  }

  return text;
}

/**
 * Derived reactive translation store for Svelte templates:
 * Usage:
 *   import { _ } from "./lib/i18n";
 *   {$_("connect_device")} or {$_("welcome_user", { name: "Alex" })}
 */
export const _ = derived(locale, $locale => {
  const dict = LOCALES[$locale] || LOCALES.en;
  return (key, params = {}, fallback = "") => {
    if (!key) return "";
    let text = dict[key];
    if (text === undefined) {
      text = LOCALES.en[key] !== undefined ? LOCALES.en[key] : (fallback || key);
    }
    if (typeof text !== "string") return text;
    if (params && typeof params === "object") {
      for (const [k, v] of Object.entries(params)) {
        text = text.replace(new RegExp(`\\{\\{?\\s*${k}\\s*\\}?\}`, "g"), String(v));
        text = text.replace(new RegExp(`%${k}%`, "g"), String(v));
      }
    }
    return text;
  };
});

export const t = _;

/**
 * Construct SVAR Filemanager words dictionary for <Locale words={...}>
 * Mapping internal SVAR keys to active language.
 */
export function getSvarLocale(langCode, rootName = "My files") {
  const dict = LOCALES[langCode] || LOCALES.en;
  return {
    core: {
      "ok": dict["ok"] || dict["Ok"] || "OK",
      "cancel": dict["cancel"] || dict["Cancel"] || "Cancel",
      "Ok": dict["ok"] || dict["Ok"] || "OK",
      "Cancel": dict["cancel"] || dict["Cancel"] || "Cancel"
    },
    filemanager: {
      "My files": rootName || dict["my_files"] || "My files",
      "Add New": dict["add_new"] || dict["Add New"] || dict["add"] || "Add New",
      "shared-to-me": dict["shared_to_me"] || dict["shared-to-me"] || "Shared with me",
      "shared-by-me": dict["shared_by_me"] || dict["shared-by-me"] || "Shared by me",
      "chats": dict["chats"] || dict["local_ai"] || "Chats",
      "/chats": dict["chats"] || dict["local_ai"] || "Chats",
      "Chats": dict["chats"] || dict["local_ai"] || "Chats",
      "terminal": dict["terminal"] || dict["Terminal"] || "Terminal",
      "/terminal": dict["terminal"] || dict["Terminal"] || "Terminal",
      "Terminal": dict["terminal"] || dict["Terminal"] || "Terminal",
      "App": dict["app_type"] || "App",
      "/shared-to-me": dict["shared_to_me"] || dict["shared-to-me"] || "Shared with me",
      "/shared-by-me": dict["shared_by_me"] || dict["shared-by-me"] || "Shared by me",
      "Shared with me": dict["shared_to_me"] || "Shared with me",
      "Shared by me": dict["shared_by_me"] || "Shared by me",
      "SHARED WITH ME": dict["shared_to_me"] ? dict["shared_to_me"].toUpperCase() : "SHARED WITH ME",
      "SHARED BY ME": dict["shared_by_me"] ? dict["shared_by_me"].toUpperCase() : "SHARED BY ME",
      "Back to files": dict["back_to_files"] || "Back to files",
      "Ok": dict["ok"] || dict["Ok"] || "OK",
      "Cancel": dict["cancel"] || dict["Cancel"] || "Cancel",
      "Delete files": dict["delete"] || dict["Delete files"] || "Delete files",
      "Files": dict["files_app"] || dict["Files"] || "Files",
      "Folder": dict["folder"] || dict["Folder"] || "Folder",
      "Back to parent folder": dict["back"] || dict["Back to parent folder"] || "Back",
      "Information": dict["info"] || dict["Information"] || "Information",
      "Found": dict["Found"] || "Found",
      "Are you sure you want to delete these items:": dict["delete_confirm"] || "Are you sure you want to delete these items:",
      "Enter file name": dict["enter_file_name"] || "Enter file name",
      "Enter folder name": dict["enter_folder_name"] || "Enter folder name",
      "New file": dict["new_file"] || "New file",
      "New folder": dict["new_folder"] || "New folder",
      "Result": dict["result"] || "Result",
      "Search": dict["search"] || "Search",
      "Search results in": dict["search_results_in"] || "Search results in",
      "Add new file": dict["add_file"] || "Add new file",
      "Add new folder": dict["create_folder"] || dict["add_folder"] || "Add new folder",
      "Add folder": dict["add_folder"] || "Add folder",
      "Add folder on this device": dict["add_folder_this_device"] || "Add folder on this device",
      "Upload file": dict["upload"] || "Upload file",
      "Size": dict["size"] || "Size",
      "Name": dict["name"] || "Name",
      "Type": dict["type"] || "Type",
      "Date": dict["date"] || "Date",
      "Rename": dict["rename"] || "Rename",
      "Cut": dict["cut"] || "Cut",
      "Delete": dict["delete"] || "Delete",
      "Copy": dict["copy"] || "Copy",
      "Paste": dict["paste"] || "Paste",
      "Download": dict["download"] || "Download",
      "Count": dict["count"] || "Count",
      "folder": dict["folder"] || "folder",
      "folders": dict["folders"] || "folders",
      "file": dict["file"] || "file",
      "files": dict["files_app"] ? dict["files_app"].toLowerCase() : "files",
      "Multiple files": dict["multiple_files"] || "Multiple files",
      "multiple": dict["multiple"] || "multiple",
      "Preview": dict["preview"] || "Preview",
      "of": dict["of"] || "of",
      "used": dict["used"] || "used",
      "Select file or folder to view details": dict["select_to_view_details"] || "Select file or folder to view details",
      "Looks like nothing is here": dict["empty_folder"] || "Looks like nothing is here",
      "Unknown file": dict["unknown_file"] || "Unknown file",
      "A miniature file preview": dict["miniature_preview"] || "A miniature file preview",
      "Close search": dict["close_search"] || dict["close"] || "Close search",
      "Close": dict["close"] || "Close",
      "Sort by": dict["sort_by"] || "Sort by",
      "Ascending": dict["ascending"] || "Ascending",
      "Descending": dict["descending"] || "Descending",
      "Name (A-Z)": dict["sort_name_asc"] || "Name (A-Z)",
      "Name (Z-A)": dict["sort_name_desc"] || "Name (Z-A)",
      "Date (oldest first)": dict["sort_date_asc"] || "Date (oldest first)",
      "Date (newest first)": dict["sort_date_desc"] || "Date (newest first)",
      "Size (smallest first)": dict["sort_size_asc"] || "Size (smallest first)",
      "Size (largest first)": dict["sort_size_desc"] || "Size (largest first)"
    }
  };
}
