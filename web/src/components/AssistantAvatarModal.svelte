<script>
  import { createEventDispatcher, onMount } from "svelte";
  import { _ } from "../lib/i18n/index.js";

  export let open = false;
  export let chatStore = null;
  export let httpDrive = null;
  export let currentAvatarUrl = "";
  export let assistantName = "June";
  export let style = "realistic";
  export let character = "June";
  export let appearancePrompt = "";
  export let defaultStorage = "";
  export let omdKey = "";
  // Одноразовый consent-грант под запрос (buildDevicePush из Chat) и резолвер
  // /p2p-URL для файла, который пушнули на устройство.
  export let getDevicePush = null;
  export let getDeviceShareUrl = null;

  const dispatch = createEventDispatcher();

  let avatarStyle = style || "realistic";
  let avatarCharacter = (character && character !== "June") ? character : "Domi";
  let promptText = appearancePrompt || "";
  let generating = false;
  let applying = false;
  let errorMessage = "";

  let candidates = [];
  let currentIndex = 0;

  let fileInputEl;

  let availableCharacters = ["Domi"];
  let loadingLoras = false;

  $: if (open) {
    initModal();
  }

  $: if (open && httpDrive) {
    loadLoras();
  }

  function initModal() {
    avatarStyle = style || "realistic";
    avatarCharacter = (character && character !== "June") ? character : "Domi";
    promptText = appearancePrompt || "social media profile picture, headshot, best quality, highres, smiling, office background";
    errorMessage = "";
    candidates = [];
    currentIndex = 0;

    if (currentAvatarUrl) {
      candidates.push({
        url: currentAvatarUrl,
        isCurrent: true,
      });
    }

    loadLoras();
    loadSavedAvatars();
  }

  async function loadLoras() {
    if (!httpDrive) return;
    loadingLoras = true;
    try {
      const mode = (typeof localStorage !== "undefined" && localStorage.getItem("onmychat_mode")) || "work";
      let res = await httpDrive.fetch(`/assistant/loras?mode=${mode}`);
      if (!res.ok) {
        res = await httpDrive.fetch("/assistant/loras");
      }
      if (res.ok) {
        const data = await res.json();
        const loraList = Array.isArray(data) ? data : (data.loras || data.data || []);
        const characters = loraList
          .filter((l) => (!l.type || l.type === "character"))
          .map((l) => (typeof l === "string" ? l : l.name))
          .filter(Boolean);
        if (characters.length > 0) {
          availableCharacters = characters;
          if (avatarCharacter && !availableCharacters.includes(avatarCharacter)) {
            availableCharacters = [avatarCharacter, ...availableCharacters];
          } else if (!avatarCharacter) {
            avatarCharacter = availableCharacters[0];
          }
        }
      }
    } catch (err) {
      console.warn("[avatar-modal] Failed to load loras via p2p:", err);
    } finally {
      loadingLoras = false;
    }
  }

  function updateAvatarPromptTags() {
    let prompt = promptText || "";
    let cleanPrompt = prompt.replace(/<[^>]+>/g, "").replace(/\s+/g, " ").trim();
    if (cleanPrompt.endsWith(",")) cleanPrompt = cleanPrompt.slice(0, -1);
    if (avatarStyle) {
      cleanPrompt += " <" + avatarStyle + ">";
    }
    if (avatarCharacter) {
      cleanPrompt += " <" + avatarCharacter + ">";
    }
    promptText = cleanPrompt;
  }

  async function loadSavedAvatars() {
    if (!httpDrive) return;
    try {
      const storagePath = defaultStorage || "/Private/Data";
      const listPath = `${storagePath}/generated/avatars`.replace(/\/+/g, "/");
      const res = await httpDrive.fetch(`/api/v1/files?path=${encodeURIComponent(listPath)}`);
      if (res.ok) {
        const data = await res.json();
        const items = (data.list || data.files || []).filter((f) => /\.(jpe?g|png|webp)$/i.test(f.name || ""));
        for (const it of items) {
          const fileUrl = `${listPath}/${it.name}`.replace(/\/+/g, "/");
          candidates.push({
            url: `/api/v1/download?path=${encodeURIComponent(fileUrl)}`,
            name: it.name,
            isCurrent: false,
          });
        }
        candidates = candidates;
      }
    } catch (_) {
      // Gracefully ignore file listing errors
    }
  }

  function prevCandidate() {
    if (currentIndex > 0) currentIndex--;
  }

  function nextCandidate() {
    if (currentIndex < candidates.length - 1) currentIndex++;
  }

  function handleFileUpload(e) {
    const file = e.target?.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      const base64 = reader.result;
      candidates = [
        ...candidates,
        {
          url: base64,
          base64: base64,
          isCurrent: false,
          isUploaded: true,
        },
      ];
      currentIndex = candidates.length - 1;
    };
    reader.readAsDataURL(file);
  }

  async function handleGenerate() {
    generating = true;
    errorMessage = "";
    try {
      let resultUrl = "";
      if (httpDrive) {
        const body = {
          style: avatarStyle,
          character_lora: avatarCharacter,
          prompt: promptText,
          storage: defaultStorage || "/Private/Data",
          omd_key: omdKey,
        };
        try {
          const push = typeof getDevicePush === "function" ? getDevicePush() : null;
          if (push) body.device_push = push;
        } catch (_) { /* consent выдать не удалось — пуш просто не уйдёт */ }
        const res = await httpDrive.fetch("/assistant/avatar/generate", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        });
        if (res.ok) {
          const data = await res.json();
          if (data && (data.image || data.url || data.device_path)) {
            // Приоритет — копия на устройстве (персистентна), потом шлюз, потом имя файла.
            resultUrl = (data.device_path && typeof getDeviceShareUrl === "function" && getDeviceShareUrl(data.device_path))
              || data.url
              || (data.image ? `/generated/${data.image}` : "");
          }
        }
      }

      if (resultUrl) {
        let candidateBase64 = null;
        if (!resultUrl.startsWith("data:")) {
          try {
            let res = null;
            if (httpDrive) {
              try { res = await httpDrive.fetch(resultUrl); } catch (_) {}
            }
            if (!res || !res.ok) {
              try { res = await fetch(resultUrl); } catch (_) {}
            }
            if (res && res.ok) {
              const blob = await res.blob();
              candidateBase64 = await new Promise((resolve) => {
                const r = new FileReader();
                r.onloadend = () => resolve(r.result);
                r.readAsDataURL(blob);
              });
            }
          } catch (_) {}
        }

        candidates = [
          ...candidates,
          {
            url: candidateBase64 || resultUrl,
            base64: candidateBase64,
            isCurrent: false,
          },
        ];
        currentIndex = candidates.length - 1;
      } else {
        // Notice if backend avatar generation not ready
        errorMessage = $_("avatar_gen_notice", {}, "Inference node avatar generator is processing your prompt...");
      }
    } catch (err) {
      errorMessage = err.message || $_("avatar_gen_error", {}, "Avatar generation error");
    } finally {
      generating = false;
    }
  }

  async function handleApply() {
    applying = true;
    errorMessage = "";
    try {
      const selected = candidates[currentIndex];
      let avatarBase64 = null;

      if (selected?.base64) {
        avatarBase64 = selected.base64;
      } else if (selected?.url && selected.url.startsWith("data:")) {
        avatarBase64 = selected.url;
      } else if (selected?.url && !selected.isCurrent) {
        try {
          let res = null;
          if (httpDrive) {
            try { res = await httpDrive.fetch(selected.url); } catch (_) {}
          }
          if (!res || !res.ok) {
            try { res = await fetch(selected.url); } catch (_) {}
          }
          if (res && res.ok) {
            const blob = await res.blob();
            avatarBase64 = await new Promise((resolve) => {
              const r = new FileReader();
              r.onloadend = () => resolve(r.result);
              r.readAsDataURL(blob);
            });
          }
        } catch (_) {}
      }

      const updatePayload = {
        style: avatarStyle,
        assistant_model: avatarCharacter,
        assistant_appearance: promptText,
      };

      if (avatarBase64) {
        updatePayload.assistant_avatar = avatarBase64;
        updatePayload.avatar_source = "custom";
      }

      if (chatStore && chatStore.updateAssistantSettings) {
        await chatStore.updateAssistantSettings(updatePayload);
      }

      dispatch("saved", {
        ...updatePayload,
        avatarUrl: avatarBase64 || selected?.url || currentAvatarUrl,
      });

      close();
    } catch (err) {
      errorMessage = err.message || "Failed to apply appearance settings";
    } finally {
      applying = false;
    }
  }

  function close() {
    open = false;
    dispatch("close");
  }
</script>

<svelte:window on:keydown={(e) => open && e.key === "Escape" && close()} />

{#if open}
  <!-- Backdrop -->
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="avatar-modal-backdrop" on:click|self={close}>
    <div class="avatar-modal-card">
      <!-- Header -->
      <div class="avatar-modal-header">
        <div class="header-titles">
          <h3>{$_("edit_avatar", {}, "Assistant Appearance & Avatar")}</h3>
          <p class="header-sub">{assistantName} · {$_("customize_look", {}, "Customize appearance and style")}</p>
        </div>
        <button type="button" class="close-btn" on:click={close} title={$_("close", {}, "Close")}>
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="18" y1="6" x2="6" y2="18" />
            <line x1="6" y1="6" x2="18" y2="18" />
          </svg>
        </button>
      </div>

      <!-- Body -->
      <div class="avatar-modal-body">
        <!-- Avatar Preview with Glow & Navigation -->
        <div class="avatar-stage">
          <div class="avatar-circle-wrap">
            {#if candidates.length > 0 && candidates[currentIndex]?.url}
              <img src={candidates[currentIndex].url} alt={assistantName} class="avatar-display-img" />
            {:else}
              <div class="avatar-placeholder">
                <svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5">
                  <circle cx="12" cy="8" r="5" />
                  <path d="M20 21a8 8 0 1 0-16 0" />
                </svg>
              </div>
            {/if}

            {#if generating || applying}
              <div class="avatar-loader-overlay">
                <div class="spinner"></div>
              </div>
            {/if}
          </div>

          <!-- Carousel Controls if multiple candidates -->
          {#if candidates.length > 1}
            <div class="carousel-nav-row">
              <button
                type="button"
                class="carousel-btn"
                on:click={prevCandidate}
                disabled={currentIndex <= 0}
                title="Previous"
              >
                ‹
              </button>
              <span class="carousel-badge">{currentIndex + 1} / {candidates.length}</span>
              <button
                type="button"
                class="carousel-btn"
                on:click={nextCandidate}
                disabled={currentIndex >= candidates.length - 1}
                title="Next"
              >
                ›
              </button>
            </div>
          {/if}

          <!-- Upload local photo button -->
          <div class="upload-row">
            <input
              type="file"
              accept="image/*"
              bind:this={fileInputEl}
              on:change={handleFileUpload}
              style="display: none;"
            />
            <button
              type="button"
              class="btn-text-subtle"
              on:click={() => fileInputEl && fileInputEl.click()}
            >
              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                <polyline points="17 8 12 3 7 8" />
                <line x1="12" y1="3" x2="12" y2="15" />
              </svg>
              {$_("upload_custom_image", {}, "Upload custom photo")}
            </button>
          </div>
        </div>

        <!-- Controls Form -->
        <div class="avatar-fields">
          <div class="field-row-2">
            <!-- Style -->
            <div class="field-group">
              <label for="avatarStyle">{$_("style", {}, "Style")}:</label>
              <select id="avatarStyle" class="select-styled" bind:value={avatarStyle} on:change={updateAvatarPromptTags}>
                <option value="realistic">{$_("realistic", {}, "Realistic")}</option>
                <option value="fantasy">{$_("fantasy", {}, "Fantasy")}</option>
                <option value="perfect">{$_("perfect", {}, "Perfect")}</option>
                <option value="tooned">{$_("tooned", {}, "Tooned")}</option>
              </select>
            </div>

            <!-- Character LoRA / Preset -->
            <div class="field-group">
              <div class="field-label-row">
                <label for="avatarCharacter">{$_("character", {}, "Character Model")}:</label>
                {#if loadingLoras}
                  <span class="loading-hint">…</span>
                {/if}
              </div>
              <select id="avatarCharacter" class="select-styled" bind:value={avatarCharacter} on:change={updateAvatarPromptTags}>
                {#each availableCharacters as char}
                  <option value={char}>{char}</option>
                {/each}
              </select>
            </div>
          </div>

          <!-- Prompt -->
          <div class="field-group">
            <label for="appearancePrompt">{$_("appearance", {}, "Appearance Prompt")}:</label>
            <textarea
              id="appearancePrompt"
              class="textarea-styled"
              rows="3"
              bind:value={promptText}
              placeholder="social media profile picture, headshot, best quality, highres, smiling, office background"
            ></textarea>
            <span class="field-hint">{$_("appearance_hint", {}, "Describes visual features, hairstyle, lighting and background")}</span>
          </div>

          {#if errorMessage}
            <div class="error-banner">{errorMessage}</div>
          {/if}
        </div>
      </div>

      <!-- Footer -->
      <div class="avatar-modal-footer">
        <button type="button" class="btn-secondary" on:click={close} disabled={generating || applying}>
          {$_("cancel", {}, "Cancel")}
        </button>
        <button
          type="button"
          class="btn-warning"
          on:click={handleGenerate}
          disabled={generating || applying}
        >
          {#if generating}
            <span class="spinner-inline"></span>
            {$_("generating", {}, "Generating...")}
          {:else}
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67" />
            </svg>
            {$_("regenerate", {}, "Regenerate")}
          {/if}
        </button>
        <button
          type="button"
          class="btn-primary"
          on:click={handleApply}
          disabled={generating || applying}
        >
          {#if applying}
            {$_("saving", {}, "Saving...")}
          {:else}
            {$_("apply", {}, "Apply")}
          {/if}
        </button>
      </div>
    </div>
  </div>
{/if}

<style>
  .avatar-modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.72);
    backdrop-filter: blur(10px);
    z-index: 1000;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
    animation: fadeIn 0.2s ease;
  }

  :global(.light-theme) .avatar-modal-backdrop,
  :global(:root.light-theme) .avatar-modal-backdrop {
    background: rgba(15, 23, 42, 0.45);
  }

  @keyframes fadeIn {
    from { opacity: 0; }
    to { opacity: 1; }
  }

  .avatar-modal-card {
    --card-bg: #181d24;
    --card-border: rgba(255, 255, 255, 0.12);
    --card-shadow: 0 24px 60px rgba(0, 0, 0, 0.5), 0 0 0 1px rgba(41, 150, 204, 0.15);
    --card-color: #e2e8f0;
    --header-title: #f8fafc;
    --header-sub: #94a3b8;
    --header-border: rgba(255, 255, 255, 0.08);
    --close-btn: #94a3b8;
    --close-btn-hover-bg: rgba(255, 255, 255, 0.08);
    --close-btn-hover: #f8fafc;
    --circle-bg: #0f1318;
    --circle-loader-bg: rgba(15, 23, 42, 0.75);
    --carousel-btn-bg: rgba(255, 255, 255, 0.08);
    --carousel-btn-border: rgba(255, 255, 255, 0.1);
    --carousel-btn-color: #f8fafc;
    --carousel-badge-color: #94a3b8;
    --carousel-badge-bg: rgba(255, 255, 255, 0.06);
    --label-color: #94a3b8;
    --input-bg: rgba(255, 255, 255, 0.05);
    --input-border: rgba(255, 255, 255, 0.12);
    --input-color: #f8fafc;
    --input-option-bg: #181d24;
    --hint-color: #64748b;
    --footer-bg: rgba(0, 0, 0, 0.2);
    --footer-border: rgba(255, 255, 255, 0.08);
    --btn-sec-border: rgba(255, 255, 255, 0.15);
    --btn-sec-color: #cbd5e1;
    --btn-sec-hover-bg: rgba(255, 255, 255, 0.08);
    --btn-sec-hover-color: #ffffff;
    --btn-warn-bg: rgba(245, 158, 11, 0.15);
    --btn-warn-border: rgba(245, 158, 11, 0.3);
    --btn-warn-color: #fbbf24;
    --btn-warn-hover-bg: rgba(245, 158, 11, 0.25);

    width: 100%;
    max-width: 540px;
    background: var(--card-bg);
    border: 1px solid var(--card-border);
    border-radius: 20px;
    box-shadow: var(--card-shadow);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    color: var(--card-color);
    animation: slideUp 0.25s cubic-bezier(0.16, 1, 0.3, 1);
  }

  :global(.light-theme) .avatar-modal-card,
  :global(:root.light-theme) .avatar-modal-card {
    --card-bg: #ffffff;
    --card-border: rgba(0, 0, 0, 0.1);
    --card-shadow: 0 20px 50px rgba(0, 0, 0, 0.15), 0 0 0 1px rgba(41, 150, 204, 0.18);
    --card-color: #1e293b;
    --header-title: #0f172a;
    --header-sub: #64748b;
    --header-border: rgba(0, 0, 0, 0.08);
    --close-btn: #64748b;
    --close-btn-hover-bg: rgba(0, 0, 0, 0.06);
    --close-btn-hover: #0f172a;
    --circle-bg: #f8fafc;
    --circle-loader-bg: rgba(255, 255, 255, 0.85);
    --carousel-btn-bg: rgba(0, 0, 0, 0.05);
    --carousel-btn-border: rgba(0, 0, 0, 0.1);
    --carousel-btn-color: #1e293b;
    --carousel-badge-color: #475569;
    --carousel-badge-bg: rgba(0, 0, 0, 0.05);
    --label-color: #475569;
    --input-bg: #f8fafc;
    --input-border: rgba(0, 0, 0, 0.15);
    --input-color: #0f172a;
    --input-option-bg: #ffffff;
    --hint-color: #64748b;
    --footer-bg: #f8fafc;
    --footer-border: rgba(0, 0, 0, 0.08);
    --btn-sec-border: rgba(0, 0, 0, 0.15);
    --btn-sec-color: #334155;
    --btn-sec-hover-bg: rgba(0, 0, 0, 0.06);
    --btn-sec-hover-color: #0f172a;
    --btn-warn-bg: rgba(245, 158, 11, 0.12);
    --btn-warn-border: rgba(217, 119, 6, 0.35);
    --btn-warn-color: #d97706;
    --btn-warn-hover-bg: rgba(245, 158, 11, 0.2);
  }

  @media (prefers-color-scheme: light) {
    :global(:not(.dark-theme)) .avatar-modal-backdrop {
      background: rgba(15, 23, 42, 0.45);
    }
    :global(:not(.dark-theme)) .avatar-modal-card {
      --card-bg: #ffffff;
      --card-border: rgba(0, 0, 0, 0.1);
      --card-shadow: 0 20px 50px rgba(0, 0, 0, 0.15), 0 0 0 1px rgba(41, 150, 204, 0.18);
      --card-color: #1e293b;
      --header-title: #0f172a;
      --header-sub: #64748b;
      --header-border: rgba(0, 0, 0, 0.08);
      --close-btn: #64748b;
      --close-btn-hover-bg: rgba(0, 0, 0, 0.06);
      --close-btn-hover: #0f172a;
      --circle-bg: #f8fafc;
      --circle-loader-bg: rgba(255, 255, 255, 0.85);
      --carousel-btn-bg: rgba(0, 0, 0, 0.05);
      --carousel-btn-border: rgba(0, 0, 0, 0.1);
      --carousel-btn-color: #1e293b;
      --carousel-badge-color: #475569;
      --carousel-badge-bg: rgba(0, 0, 0, 0.05);
      --label-color: #475569;
      --input-bg: #f8fafc;
      --input-border: rgba(0, 0, 0, 0.15);
      --input-color: #0f172a;
      --input-option-bg: #ffffff;
      --hint-color: #64748b;
      --footer-bg: #f8fafc;
      --footer-border: rgba(0, 0, 0, 0.08);
      --btn-sec-border: rgba(0, 0, 0, 0.15);
      --btn-sec-color: #334155;
      --btn-sec-hover-bg: rgba(0, 0, 0, 0.06);
      --btn-sec-hover-color: #0f172a;
      --btn-warn-bg: rgba(245, 158, 11, 0.12);
      --btn-warn-border: rgba(217, 119, 6, 0.35);
      --btn-warn-color: #d97706;
      --btn-warn-hover-bg: rgba(245, 158, 11, 0.2);
    }
  }

  @keyframes slideUp {
    from { transform: translateY(16px) scale(0.97); }
    to { transform: translateY(0) scale(1); }
  }

  .avatar-modal-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 18px 24px;
    border-bottom: 1px solid var(--header-border);
  }

  .header-titles h3 {
    margin: 0;
    font-size: 1.15rem;
    font-weight: 700;
    color: var(--header-title);
  }

  .header-sub {
    margin: 4px 0 0;
    font-size: 0.8rem;
    color: var(--header-sub);
  }

  .close-btn {
    background: transparent;
    border: none;
    color: var(--close-btn);
    width: 34px;
    height: 34px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.15s, color 0.15s;
  }

  .close-btn:hover {
    background: var(--close-btn-hover-bg);
    color: var(--close-btn-hover);
  }

  .avatar-modal-body {
    padding: 22px 24px;
    display: flex;
    flex-direction: column;
    gap: 20px;
  }

  .avatar-stage {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
  }

  .avatar-circle-wrap {
    position: relative;
    width: 140px;
    height: 140px;
    border-radius: 50%;
    border: 3px solid #2996cc;
    box-shadow: 0 0 24px rgba(41, 150, 204, 0.45);
    overflow: hidden;
    background: var(--circle-bg);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .avatar-display-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .avatar-placeholder {
    color: #475569;
  }

  .avatar-loader-overlay {
    position: absolute;
    inset: 0;
    background: var(--circle-loader-bg);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .spinner {
    width: 32px;
    height: 32px;
    border: 3px solid rgba(41, 150, 204, 0.3);
    border-top-color: #2996cc;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  .spinner-inline {
    display: inline-block;
    width: 14px;
    height: 14px;
    border: 2px solid rgba(255, 255, 255, 0.3);
    border-top-color: #ffffff;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
    margin-right: 6px;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .carousel-nav-row {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .carousel-btn {
    background: var(--carousel-btn-bg);
    border: 1px solid var(--carousel-btn-border);
    color: var(--carousel-btn-color);
    width: 28px;
    height: 28px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    font-size: 1.2rem;
    line-height: 1;
    transition: background 0.15s;
  }

  .carousel-btn:hover:not(:disabled) {
    background: rgba(41, 150, 204, 0.2);
  }

  .carousel-btn:disabled {
    opacity: 0.3;
    cursor: not-allowed;
  }

  .carousel-badge {
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--carousel-badge-color);
    background: var(--carousel-badge-bg);
    padding: 2px 8px;
    border-radius: 12px;
  }

  .btn-text-subtle {
    background: transparent;
    border: none;
    color: #2996cc;
    font-size: 0.82rem;
    font-weight: 500;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    padding: 4px 8px;
    border-radius: 6px;
    transition: background 0.15s;
  }

  .btn-text-subtle:hover {
    background: rgba(41, 150, 204, 0.12);
  }

  .avatar-fields {
    display: flex;
    flex-direction: column;
    gap: 14px;
  }

  .field-row-2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
  }

  .field-group {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .field-group label {
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--label-color);
  }

  .select-styled {
    background-color: var(--input-bg);
    border: 1px solid var(--input-border);
    border-radius: 10px;
    padding: 9px 36px 9px 12px;
    color: var(--input-color);
    font-size: 0.9rem;
    outline: none;
    transition: border-color 0.2s, box-shadow 0.2s;
    appearance: none;
    -webkit-appearance: none;
    -moz-appearance: none;
    background-repeat: no-repeat;
    background-position: right 12px center;
    background-size: 14px 14px;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%2394a3b8' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    cursor: pointer;
    font-family: inherit;
    width: 100%;
    box-sizing: border-box;
  }

  :global(.light-theme) .select-styled,
  :global(:root.light-theme) .select-styled {
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%23475569' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
  }

  @media (prefers-color-scheme: light) {
    :global(:not(.dark-theme)) .select-styled {
      background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%23475569' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    }
  }

  .select-styled:focus {
    border-color: #2996cc;
    box-shadow: 0 0 0 2px rgba(41, 150, 204, 0.2);
  }

  .select-styled option {
    background: var(--input-option-bg);
    color: var(--input-color);
  }

  .textarea-styled {
    background: var(--input-bg);
    border: 1px solid var(--input-border);
    border-radius: 10px;
    padding: 10px 12px;
    color: var(--input-color);
    font-size: 0.88rem;
    line-height: 1.4;
    outline: none;
    resize: vertical;
    font-family: inherit;
    transition: border-color 0.2s, box-shadow 0.2s;
    scrollbar-width: thin;
    scrollbar-color: rgba(148, 163, 184, 0.3) transparent;
  }

  .textarea-styled:focus {
    border-color: #2996cc;
    box-shadow: 0 0 0 2px rgba(41, 150, 204, 0.2);
  }

  .textarea-styled::-webkit-scrollbar {
    width: 6px;
    height: 6px;
  }

  .textarea-styled::-webkit-scrollbar-track {
    background: transparent;
  }

  .textarea-styled::-webkit-scrollbar-thumb {
    background: rgba(148, 163, 184, 0.25);
    border-radius: 3px;
  }

  .textarea-styled::-webkit-scrollbar-thumb:hover {
    background: rgba(148, 163, 184, 0.45);
  }

  .textarea-styled::-webkit-resizer {
    background-color: transparent;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10' fill='none'%3E%3Cpath d='M9 2L2 9M9 6L6 9' stroke='%2364748b' stroke-width='1.5' stroke-linecap='round'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: bottom 2px right 2px;
    background-size: 8px 8px;
  }

  .field-hint {
    font-size: 0.74rem;
    color: var(--hint-color);
  }

  .error-banner {
    padding: 8px 12px;
    border-radius: 8px;
    background: rgba(239, 68, 68, 0.15);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: #fca5a5;
    font-size: 0.82rem;
  }

  .avatar-modal-footer {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 10px;
    padding: 16px 24px;
    border-top: 1px solid var(--footer-border);
    background: var(--footer-bg);
  }

  .btn-secondary {
    background: transparent;
    border: 1px solid var(--btn-sec-border);
    color: var(--btn-sec-color);
    padding: 8px 16px;
    border-radius: 10px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s, color 0.15s;
  }

  .btn-secondary:hover:not(:disabled) {
    background: var(--btn-sec-hover-bg);
    color: var(--btn-sec-hover-color);
  }

  .btn-warning {
    background: var(--btn-warn-bg);
    border: 1px solid var(--btn-warn-border);
    color: var(--btn-warn-color);
    padding: 8px 16px;
    border-radius: 10px;
    font-weight: 600;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    transition: background 0.15s;
  }

  .btn-warning:hover:not(:disabled) {
    background: var(--btn-warn-hover-bg);
  }

  .btn-primary {
    background: #2996cc;
    border: none;
    color: #ffffff;
    padding: 8px 20px;
    border-radius: 10px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s, box-shadow 0.15s;
    box-shadow: 0 2px 10px rgba(41, 150, 204, 0.4);
  }

  .btn-primary:hover:not(:disabled) {
    background: #2384b5;
    box-shadow: 0 4px 14px rgba(41, 150, 204, 0.5);
  }

  .btn-secondary:disabled,
  .btn-warning:disabled,
  .btn-primary:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
</style>
