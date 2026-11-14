import { writable, derived, get } from "svelte/store";

const STORAGE_KEY = "omd_unread_notifications";
const DISMISSED_IDS_KEY = "omd_dismissed_notification_ids";
const MAX_ITEMS = 50;

function loadStoredItems() {
  if (typeof localStorage === "undefined") return [];
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed : [];
  } catch (_) {
    return [];
  }
}

function saveStoredItems(items) {
  if (typeof localStorage === "undefined") return;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(items.slice(0, MAX_ITEMS)));
  } catch (_) {}
}

function loadDismissedIds() {
  if (typeof localStorage === "undefined") return new Set();
  try {
    const raw = localStorage.getItem(DISMISSED_IDS_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return new Set(Array.isArray(parsed) ? parsed : []);
  } catch (_) {
    return new Set();
  }
}

function rememberDismissedIds(ids) {
  if (typeof localStorage === "undefined" || !ids.length) return;
  try {
    const cur = Array.from(loadDismissedIds());
    const merged = Array.from(new Set([...cur, ...ids])).slice(-100);
    localStorage.setItem(DISMISSED_IDS_KEY, JSON.stringify(merged));
  } catch (_) {}
}

export function normalizeInboxNotification(payload) {
  const data = payload?.data && typeof payload.data === "object" ? payload.data : {};
  const timestamp = Number(payload?.timestamp || data.timestamp || Date.now());
  const id = String(
    payload?.id ||
      data.inbox_id ||
      (payload?.tag ? `tag:${payload.tag}` : `local_${timestamp}_${Math.random().toString(36).slice(2, 7)}`)
  );
  return {
    id,
    inboxId: String(data.inbox_id || payload?.id || ""),
    title: String(payload?.title || data.title || "Disk Direct"),
    body: String(payload?.body || data.body || ""),
    image: String(payload?.image || data.image || ""),
    url: String(payload?.url || "/"),
    tag: String(payload?.tag || ""),
    actions: Array.isArray(payload?.actions) ? payload.actions : [],
    data,
    timestamp
  };
}

export const unreadNotifications = writable(loadStoredItems());
export const unreadCount = derived(unreadNotifications, ($items) => $items.length);
export const activeNotificationToast = writable(null);
export const notificationsModalOpen = writable(false);

let _toastTimer = null;

export function dismissNotificationToast() {
  if (_toastTimer) {
    clearTimeout(_toastTimer);
    _toastTimer = null;
  }
  activeNotificationToast.set(null);
}

export function showNotificationToast(item, autoHideMs = 9000) {
  if (!item) return;
  if (_toastTimer) {
    clearTimeout(_toastTimer);
    _toastTimer = null;
  }
  activeNotificationToast.set(item);
  if (autoHideMs > 0 && (!Array.isArray(item.actions) || item.actions.length === 0)) {
    _toastTimer = setTimeout(() => {
      activeNotificationToast.set(null);
      _toastTimer = null;
    }, autoHideMs);
  }
}

export function pushIncomingNotification(payload, { showToast = true } = {}) {
  const item = normalizeInboxNotification(payload);
  unreadNotifications.update((prev) => {
    const filtered = prev.filter((x) => x.id !== item.id && (!item.tag || x.tag !== item.tag));
    const next = [item, ...filtered].slice(0, MAX_ITEMS);
    saveStoredItems(next);
    return next;
  });
  if (showToast) {
    showNotificationToast(item);
  }
  return item;
}

export async function markNotificationRead(id, fetchFn = null, omdKey = "") {
  const cleanId = String(id || "").trim();
  if (!cleanId) return;
  const current = get(unreadNotifications);
  const target = current.find((x) => x.id === cleanId || x.inboxId === cleanId);
  const idsToForget = [cleanId, target?.inboxId].filter(Boolean);
  rememberDismissedIds(idsToForget);

  unreadNotifications.update((prev) => {
    const next = prev.filter((x) => x.id !== cleanId && x.inboxId !== cleanId);
    saveStoredItems(next);
    return next;
  });

  const active = get(activeNotificationToast);
  if (active && (active.id === cleanId || active.inboxId === cleanId)) {
    dismissNotificationToast();
  }

  if (typeof fetchFn === "function") {
    try {
      await fetchFn("/push/inbox/read", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          id: target?.inboxId || cleanId,
          omd_key: omdKey || ""
        })
      });
    } catch (_) {}
  }
}

export async function markAllNotificationsRead(fetchFn = null, omdKey = "") {
  const current = get(unreadNotifications);
  const ids = current.flatMap((x) => [x.id, x.inboxId]).filter(Boolean);
  rememberDismissedIds(ids);
  unreadNotifications.set([]);
  saveStoredItems([]);
  dismissNotificationToast();

  if (typeof fetchFn === "function") {
    try {
      await fetchFn("/push/inbox/read", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ all: true, omd_key: omdKey || "" })
      });
    } catch (_) {}
  }
}

export async function fetchUnreadNotificationsFromNode(fetchFn, omdKey = "") {
  if (typeof fetchFn !== "function") return [];
  try {
    const qs = omdKey ? `?omd_key=${encodeURIComponent(omdKey)}` : "";
    const res = await fetchFn(`/push/inbox${qs}`);
    if (!res || !res.ok) return [];
    const data = await res.json().catch(() => ({}));
    const rawItems = Array.isArray(data?.items) ? data.items : [];
    const dismissed = loadDismissedIds();
    const normalized = rawItems
      .map(normalizeInboxNotification)
      .filter((it) => !dismissed.has(it.id) && (!it.inboxId || !dismissed.has(it.inboxId)));

    if (normalized.length > 0) {
      unreadNotifications.update((prev) => {
        const byId = new Map();
        for (const it of [...normalized, ...prev]) {
          if (!byId.has(it.id)) byId.set(it.id, it);
        }
        const merged = Array.from(byId.values())
          .sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0))
          .slice(0, MAX_ITEMS);
        saveStoredItems(merged);
        return merged;
      });
      showNotificationToast(normalized[0]);
    } else {
      const existing = get(unreadNotifications);
      if (existing.length > 0 && !get(activeNotificationToast)) {
        showNotificationToast(existing[0]);
      }
    }
    return normalized;
  } catch (_) {
    return [];
  }
}

export function openNotificationsModal() {
  notificationsModalOpen.set(true);
}

export function closeNotificationsModal() {
  notificationsModalOpen.set(false);
}

