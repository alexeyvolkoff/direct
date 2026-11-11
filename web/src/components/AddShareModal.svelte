<script>
  import { createEventDispatcher, onMount } from "svelte";
  import { _ } from "../lib/i18n/index.js";
  import { parseShareLink, resolveShare, addSharedToMe } from "../lib/sharing.js";

  export let identity = null;
  export let isDarkTheme = false;
  export let existingAddresses = [];

  const dispatch = createEventDispatcher();

  let linkInput = "";
  let parsedLink = null;
  let password = "";
  let checking = false;
  let adding = false;
  let needsPassword = false;
  let record = null;
  let resolvedAddress = "";
  let errorMsg = "";
  let infoMsg = "";
  let checkTimer = null;
  let linkInputEl = null;

  $: alreadyAdded = !!(record && (existingAddresses || []).includes(record.address));
  $: canAdd = !!record && !adding && !alreadyAdded && !record.isExpired;

  onMount(() => {
    if (linkInputEl) linkInputEl.focus();
    parsedLink = parseShareLink(linkInput);
    return () => { if (checkTimer) clearTimeout(checkTimer); };
  });

  function resetResolved() {
    record = null;
    resolvedAddress = "";
    needsPassword = false;
    errorMsg = "";
    infoMsg = "";
  }

  function onInput() {
    resetResolved();
    parsedLink = parseShareLink(linkInput);
    if (checkTimer) clearTimeout(checkTimer);
    if (!parsedLink) return;
    checkTimer = setTimeout(() => checkLink(parsedLink), 450);
  }

  async function checkLink(p) {
    const target = p || parseShareLink(linkInput);
    if (!target) {
      errorMsg = $_("invalid_share_link", {}, "That doesn't look like a valid share link.");
      return;
    }
    checking = true;
    errorMsg = "";
    infoMsg = "";
    try {
      const res = await resolveShare(target.address, password, identity && identity.gun);
      if (res && res.requiresPassword) {
        needsPassword = true;
        record = null;
        resolvedAddress = target.address;
        infoMsg = $_("share_password_required", {}, "This link is password protected.");
      } else if (res && res.isExpired) {
        record = null;
        errorMsg = $_("share_link_expired", {}, "This share link has expired.");
      } else if (!res || !res.linkId || !res.path) {
        record = null;
        errorMsg = $_("share_link_unavailable", {}, "Share is unavailable. The device may be offline or the link was revoked.");
      } else {
        record = { ...res, address: target.address, shareUrl: target.shareUrl };
        resolvedAddress = target.address;
        needsPassword = false;
        infoMsg = "";
      }
    } catch (e) {
      record = null;
      errorMsg = (e && e.message) || $_("share_link_unavailable", {}, "Share is unavailable. The device may be offline or the link was revoked.");
    } finally {
      checking = false;
    }
  }

  async function onAdd() {
    if (!canAdd) return;
    adding = true;
    errorMsg = "";
    try {
      await addSharedToMe(identity, {
        address: record.address,
        shareUrl: record.shareUrl,
        ownerPub: record.ownerPub,
        ownerName: record.ownerName,
        shareId: record.shareId,
        linkId: record.linkId,
        path: record.path,
        name: record.name,
        isDir: record.isDir,
        mimeType: record.mimeType,
        size: record.size,
        writable: record.writable,
        protected: !!record.protected
      });
      dispatch("added", {
        address: record.address,
        name: record.name,
        isDir: !!record.isDir
      });
    } catch (e) {
      errorMsg = (e && e.message) || $_("generic_error", {}, "Something went wrong");
    } finally {
      adding = false;
    }
  }

  function formatSize(bytes) {
    const n = Number(bytes) || 0;
    if (n <= 0) return "";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let i = 0;
    let v = n;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return `${v < 10 && i > 0 ? v.toFixed(1) : Math.round(v)} ${units[i]}`;
  }
</script>

<!-- svelte-ignore a11y-click-events-have-key-events -->
<!-- svelte-ignore a11y-no-static-element-interactions -->
<div class="modal-backdrop" on:click|self={() => dispatch("close")}>
  <div class="modal-card {isDarkTheme ? 'dark-theme' : 'light-theme'}">
    <!-- Header -->
    <div class="modal-header">
      <div class="header-info">
        <div class="header-icon">
          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71" />
            <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71" />
          </svg>
        </div>
        <div>
          <h3 class="modal-title">{$_("paste_share_link", {}, "Paste link")}</h3>
          <p class="modal-sub">{$_("shared_to_me_sub", {}, "Add someone else's file or folder to Shared with me")}</p>
        </div>
      </div>
      <button class="close-btn" on:click={() => dispatch("close")} aria-label={$_("close", {}, "Close")}>
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
      </button>
    </div>

    <!-- Body -->
    <div class="modal-body">
      <div class="link-section">
        <label class="section-label" for="share-link-input">{$_("resource_link", {}, "Resource link")}</label>
        <div class="link-input-group">
          <input
            id="share-link-input"
            class="link-input"
            type="text"
            bind:this={linkInputEl}
            bind:value={linkInput}
            on:input={onInput}
            placeholder="https://…/shared/<owner>.<id>"
            spellcheck="false"
            autocomplete="off"
          />
          <button class="btn-check" on:click={() => checkLink()} disabled={!linkInput.trim() || checking}>
            {checking ? $_("checking", {}, "Checking…") : $_("check_link", {}, "Check")}
          </button>
        </div>
        <p class="link-format-hint">{$_("share_link_format_hint", {}, "Format: https://…/shared/&lt;owner>.&lt;id> or &lt;owner>.&lt;id>")}</p>
      </div>

      {#if needsPassword}
        <div class="password-section">
          <label class="section-label" for="share-password-input">{$_("password", {}, "Password")}</label>
          <div class="link-input-group">
            <input
              id="share-password-input"
              class="text-input"
              type="password"
              bind:value={password}
              on:keydown={(e) => { if (e.key === "Enter") checkLink(); }}
              placeholder={$_("enter_share_password", {}, "Enter the link password")}
            />
            <button class="btn-check" on:click={() => checkLink()} disabled={!password || checking}>
              {$_("unlock", {}, "Unlock")}
            </button>
          </div>
        </div>
      {/if}

      {#if record}
        <div class="preview-card">
          <div class="preview-icon">
            {#if record.isDir}
              <svg viewBox="0 0 24 24" width="26" height="26" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
                <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
              </svg>
            {:else}
              <svg viewBox="0 0 24 24" width="26" height="26" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
                <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/>
              </svg>
            {/if}
          </div>
          <div class="preview-text">
            <span class="preview-name" title={record.name}>{record.name || resolvedAddress}</span>
            <span class="preview-meta">
              {record.isDir ? $_("folder", {}, "Folder") : ($_("file", {}, "File"))}
              {#if !record.isDir && record.size}
                • {formatSize(record.size)}
              {/if}
              • {$_("shared_by", {}, "Shared by")} {record.ownerName || "OMD User"}
            </span>
            <div class="preview-badges">
              <span class="badge {record.writable ? 'badge-write' : 'badge-read'}">
                {record.writable ? $_("can_edit", {}, "Can edit") : $_("read_only", {}, "Read-only")}
              </span>
              {#if record.protected}
                <span class="badge badge-lock">{$_("password_protected", {}, "Password protected")}</span>
              {/if}
            </div>
          </div>
        </div>
      {/if}

      {#if errorMsg}
        <div class="alert error">{errorMsg}</div>
      {/if}
      {#if infoMsg}
        <div class="alert info">{infoMsg}</div>
      {/if}
      {#if alreadyAdded}
        <div class="alert success">{$_("already_in_shared_to_me", {}, "This link is already in Shared to me.")}</div>
      {/if}
    </div>

    <!-- Footer -->
    <div class="modal-footer">
      <div class="footer-spacer"></div>
      <button class="btn-cancel" on:click={() => dispatch("close")}>{$_("close", {}, "Close")}</button>
      <button class="btn-primary" on:click={onAdd} disabled={!canAdd}>
        {#if adding}
          {$_("adding", {}, "Adding…")}
        {:else}
          {$_("add_to_shared_to_me", {}, "Add to Shared to me")}
        {/if}
      </button>
    </div>
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.6);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 9999;
    padding: 16px;
  }

  .modal-card {
    width: 100%;
    max-width: 520px;
    border-radius: 16px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    box-shadow: 0 20px 48px rgba(0, 0, 0, 0.25);
    border: 1px solid rgba(139, 128, 163, 0.2);
    animation: modalAppear 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  @keyframes modalAppear {
    from { opacity: 0; transform: scale(0.96) translateY(8px); }
    to { opacity: 1; transform: scale(1) translateY(0); }
  }

  .modal-card.light-theme {
    background: #ffffff;
    color: #151821;
    border-color: #D8DCE8;
  }

  .modal-card.dark-theme {
    background: #1F2430;
    color: #ffffff;
    border-color: #32394B;
  }

  .modal-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 18px 20px;
    border-bottom: 1px solid rgba(139, 128, 163, 0.15);
  }

  .light-theme .modal-header { border-bottom-color: #D8DCE8; }
  .dark-theme .modal-header { border-bottom-color: #32394B; }

  .header-info {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .header-icon {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: rgba(123, 134, 196, 0.15);
    color: #5863A6;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .modal-title {
    font-size: 16px;
    font-weight: 600;
    margin: 0;
  }

  .modal-sub {
    font-size: 12px;
    color: #7E869B;
    margin: 2px 0 0 0;
  }

  .close-btn {
    background: transparent;
    border: none;
    cursor: pointer;
    color: #7E869B;
    padding: 6px;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .close-btn:hover {
    background: rgba(139, 128, 163, 0.15);
    color: inherit;
  }

  .modal-body {
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }

  .link-section,
  .password-section {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .section-label {
    font-size: 13px;
    font-weight: 600;
  }

  .link-format-hint {
    margin: 2px 0 0;
    font-size: 11.5px;
    color: #7E869B;
  }

  .link-input-group {
    display: flex;
    gap: 8px;
  }

  .link-input {
    flex: 1;
    min-width: 0;
    padding: 9px 12px;
    border-radius: 8px;
    font-family: monospace;
    font-size: 13px;
    border: 1px solid rgba(139, 128, 163, 0.3);
    background: rgba(139, 128, 163, 0.08);
    color: inherit;
  }

  .text-input {
    flex: 1;
    min-width: 0;
    padding: 9px 12px;
    border-radius: 8px;
    border: 1px solid rgba(139, 128, 163, 0.3);
    background: rgba(139, 128, 163, 0.08);
    color: inherit;
    font-size: 13px;
    box-sizing: border-box;
  }

  .btn-check {
    padding: 9px 16px;
    background: #5863A6;
    color: #ffffff;
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: background 0.15s;
  }

  .btn-check:hover { background: #3D467A; }
  .btn-check:disabled { opacity: 0.5; cursor: default; }

  .preview-card {
    display: flex;
    gap: 12px;
    align-items: flex-start;
    padding: 14px;
    border-radius: 12px;
    background: rgba(123, 134, 196, 0.08);
    border: 1px solid rgba(123, 134, 196, 0.2);
  }

  .preview-icon {
    color: #5863A6;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    padding-top: 2px;
  }

  .preview-text {
    display: flex;
    flex-direction: column;
    gap: 3px;
    min-width: 0;
  }

  .preview-name {
    font-size: 14px;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .preview-meta {
    font-size: 12px;
    color: #7E869B;
  }

  .preview-path {
    font-size: 11px;
    font-family: monospace;
    color: #7E869B;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .preview-badges {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
    margin-top: 4px;
  }

  .badge {
    font-size: 10px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.4px;
    padding: 2px 7px;
    border-radius: 999px;
    border: 1px solid transparent;
  }

  .badge-read {
    background: rgba(139, 128, 163, 0.15);
    color: #7E869B;
    border-color: rgba(139, 128, 163, 0.3);
  }

  .badge-write {
    background: rgba(41, 204, 84, 0.12);
    color: #29cc54;
    border-color: rgba(41, 204, 84, 0.3);
  }

  .badge-lock {
    background: rgba(245, 158, 11, 0.12);
    color: #f59e0b;
    border-color: rgba(245, 158, 11, 0.3);
  }

  .alert {
    padding: 10px 14px;
    border-radius: 8px;
    font-size: 13px;
  }

  .alert.error {
    background: rgba(248, 59, 66, 0.12);
    color: #f83b42;
    border: 1px solid rgba(248, 59, 66, 0.25);
  }

  .alert.info {
    background: rgba(123, 134, 196, 0.12);
    color: #5863A6;
    border: 1px solid rgba(123, 134, 196, 0.25);
  }

  .alert.success {
    background: rgba(41, 204, 84, 0.12);
    color: #29cc54;
    border: 1px solid rgba(41, 204, 84, 0.25);
  }

  .modal-footer {
    display: flex;
    align-items: center;
    padding: 14px 20px;
    border-top: 1px solid rgba(139, 128, 163, 0.15);
    gap: 10px;
  }

  .light-theme .modal-footer { border-top-color: #D8DCE8; }
  .dark-theme .modal-footer { border-top-color: #32394B; }

  .footer-spacer { flex: 1; }

  .btn-primary {
    padding: 9px 18px;
    background: #5863A6;
    color: #ffffff;
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s;
  }

  .btn-primary:hover { background: #7B86C4; }
  .btn-primary:disabled { opacity: 0.5; cursor: default; }

  .btn-cancel {
    padding: 9px 14px;
    background: transparent;
    border: 1px solid rgba(139, 128, 163, 0.25);
    color: inherit;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
  }
</style>
