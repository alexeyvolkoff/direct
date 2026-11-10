import { writable } from "svelte/store";

export const profileStore = writable({
  authorized: false,
  identityType: "none", // "none" | "ephemeral" | "persistent"
  isEphemeral: false,
  isPersistent: false,
  did: "",
  name: "",
  user: "",
  email: "",
  image: "",
  defaultStorage: "",
  online: true,
  deviceCount: 0,
});

export function setProfile(data) {
  profileStore.set(data);
  if (typeof window !== "undefined") {
    window.__omdProfile = data;
    window.dispatchEvent(new CustomEvent("omd-profile-update", { detail: data }));
  }
}

// Имя, которое передаём ИИ в настройках ("Calls me" → дисплей нейм → user → "User")
export function resolveChatUserName(profile, callsMe) {
  if (callsMe && String(callsMe).trim()) return String(callsMe).trim();
  const name = profile?.name || (typeof window !== "undefined" && window.__omdProfile?.name) || "";
  const user = profile?.user || (typeof window !== "undefined" && window.__omdProfile?.user) || "";
  const display = name && name.trim() !== "Guest User" ? name.trim() : "";
  const account = user && user.trim() !== "anonymous" ? user.trim() : "";
  return display || account || "User";
}
