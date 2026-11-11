<script>
  import { createEventDispatcher, onMount } from "svelte";
  import { _ } from "../lib/i18n/index.js";
  import { createShare, revokeShare, findExistingShare } from "../lib/sharing.js";
  import { sharedLinkUrl } from "../lib/public-origin.js";

  export let item = null; // { id, name, type, size, isDir, mimeType }
  export let activeDevice = null; // { linkId, name }
  export let identity = null;
  export let isDarkTheme = false;

  const dispatch = createEventDispatcher();

  let loading = true;
  let saving = false;
  let shareUrl = "";
  let shareAddress = "";
  let existingShareId = null;
  let copied = false;

  // Settings
  let enablePassword = false;
  let password = "";
  let allowWrite = false;
  let expiration = "0"; // 0 = never, 86400000 = 1 day, etc.
  let errorMsg = "";
  let successMsg = "";

  onMount(async () => {
    if (!item || !activeDevice || !identity) {
      loading = false;
      return;
    }

    try {
      const linkId = activeDevice?.linkId || item?.linkId || "";
      const itemPath = item?.id || item?.name || "";
      if (linkId && itemPath) {
        const existing = await findExistingShare(identity, linkId, itemPath);

        if (existing) {
          existingShareId = existing.id;
          shareAddress = existing.address;
          shareUrl = sharedLinkUrl(shareAddress);
          enablePassword = !!existing.protected;
          allowWrite = !!existing.writable;
          if (existing.expiresAt > 0) {
            const diff = existing.expiresAt - Date.now();
            if (diff <= 86400000 * 2) expiration = "86400000";
            else if (diff <= 86400000 * 8) expiration = "604800000";
            else expiration = "2592000000";
          }
        }
      }
    } catch (e) {
      console.warn("[ShareModal] Error finding existing share:", e);
    } finally {
      loading = false;
    }
  });

  async function handleCreateOrUpdate() {
    if (!identity || !identity.mainPub) {
      errorMsg = "You must be signed in with a decentralized DID to create public links.";
      return;
    }

    saving = true;
    errorMsg = "";
    successMsg = "";

    try {
      const linkId = activeDevice?.linkId || item?.linkId || "";
      if (!linkId || linkId === "undefined") {
        throw new Error("Cannot share item: target device link ID is missing. Please ensure your device is connected.");
      }
      const itemPath = item.id || item.name;
      const isDir = !!(item.type === "folder" || item.isDir);
      
      let expiresAt = 0;
      if (expiration !== "0") {
        expiresAt = Date.now() + Number(expiration);
      }

      let coverData = null;
      const previewSrc = item?.preview || item?.image;
      if (previewSrc) {
        try {
          const resp = await fetch(previewSrc);
          if (resp.ok) {
            const blob = await resp.blob();
            if (blob && blob.size > 0 && blob.size < 500000) {
              coverData = await new Promise((resolve) => {
                const reader = new FileReader();
                reader.onloadend = () => resolve(reader.result);
                reader.onerror = () => resolve(null);
                reader.readAsDataURL(blob);
              });
            }
          }
        } catch (e) {
          console.warn("[ShareModal] Error capturing coverData blob:", e);
        }
      }

      const res = await createShare({
        identity,
        linkId,
        path: itemPath,
        name: item.name || item.value || "Shared Item",
        title: item.title || item.customAttributes?.title || "",
        description: item.description || item.customAttributes?.description || "",
        hasCover: !!(item.hasCover || item.preview || item.customAttributes?.cover || coverData),
        coverData,
        customAttributes: item.customAttributes || {},
        item,
        isDir,
        mimeType: item.mimeType || "",
        size: item.size || 0,
        password: enablePassword ? password : "",
        writable: allowWrite,
        expiresAt,
        existingShareId,
      });

      existingShareId = res.shareId;
      shareAddress = res.address;
      shareUrl = res.shareUrl;
      successMsg = "Public P2P link is live and active!";
      dispatch("shareCreated", res);
    } catch (err) {
      console.error("[ShareModal] Share error:", err);
      errorMsg = err.message || "Failed to create share link.";
    } finally {
      saving = false;
    }
  }

  async function handleRevoke() {
    if (!existingShareId || !identity) return;
    saving = true;
    errorMsg = "";
    successMsg = "";

    try {
      const revokedId = existingShareId;
      await revokeShare(identity, revokedId);
      existingShareId = null;
      shareUrl = "";
      shareAddress = "";
      successMsg = "Share link has been revoked and deleted.";
      dispatch("shareRevoked", { shareId: revokedId });
    } catch (err) {
      errorMsg = err.message || "Failed to revoke share link.";
    } finally {
      saving = false;
    }
  }

  function copyToClipboard() {
    if (!shareUrl) return;
    if (typeof navigator !== "undefined" && navigator.clipboard) {
      navigator.clipboard.writeText(shareUrl);
      copied = true;
      setTimeout(() => { copied = false; }, 2000);
    }
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
          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2"><circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><line x1="8.59" y1="13.51" x2="15.42" y2="17.49"/><line x1="15.41" y1="6.51" x2="8.59" y2="10.49"/></svg>
        </div>
        <div>
          <h3 class="modal-title">{item?.type === 'folder' ? $_("share_folder", {}, "Share Folder") : $_("share_file", {}, "Share File")}</h3>
          <p class="modal-sub">{item?.name || item?.value || "Item"} • {$_("direct_p2p_access", {}, "Direct P2P Access")}</p>
        </div>
      </div>
      <button class="close-btn" on:click={() => dispatch("close")} aria-label={$_("close", {}, "Close")}>
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
      </button>
    </div>

    <!-- Body -->
    <div class="modal-body">
      {#if loading}
        <div class="loading-wrap">
          <div class="spinner"></div>
          <p>{$_("looking_up_share", {}, "Looking up share status...")}</p>
        </div>
      {:else}
        {#if shareUrl}
          <!-- Generated Share Link Box -->
          <div class="share-link-section">
            <label class="section-label" for="share-url-input">{$_("public_p2p_link", {}, "Public P2P Link")}</label>
            <div class="link-input-group">
              <input
                id="share-url-input"
                class="link-input"
                type="text"
                readonly
                value={shareUrl}
              />
              <button class="btn-copy" on:click={copyToClipboard}>
                {copied ? $_("copied", {}, "Copied!") : $_("copy_link", {}, "Copy Link")}
              </button>
            </div>
            <p class="link-help">{$_("share_link_help", {}, "Anyone with this link can connect directly to your node over P2P WebRTC.")}</p>
          </div>
        {/if}

        <!-- Share Options -->
        <div class="options-group">
          <!-- Password Protection Toggle -->
          <div class="option-row">
            <div class="option-text">
              <span class="option-title">{$_("password_protection", {}, "Password Protection")}</span>
              <span class="option-desc">{$_("password_protection_desc", {}, "Require a password to decrypt and view this share")}</span>
            </div>
            <label class="switch">
              <input type="checkbox" bind:checked={enablePassword} />
              <span class="slider"></span>
            </label>
          </div>

          {#if enablePassword}
            <div class="password-input-wrap">
              <input
                class="text-input"
                type="password"
                placeholder={existingShareId ? $_("keep_or_new_password", {}, "Enter new password (or leave blank to keep)") : $_("set_share_password", {}, "Set share password")}
                bind:value={password}
              />
            </div>
          {/if}

          {#if item?.type === "folder" || item?.isDir}
            <!-- Writable Toggle for folders -->
            <div class="option-row">
              <div class="option-text">
                <span class="option-title">{$_("allow_uploads_editing", {}, "Allow Uploads & Editing")}</span>
                <span class="option-desc">{$_("allow_uploads_desc", {}, "Grant guests permission to upload files into this folder")}</span>
              </div>
              <label class="switch">
                <input type="checkbox" bind:checked={allowWrite} />
                <span class="slider"></span>
              </label>
            </div>
          {/if}

          <!-- Expiration -->
          <div class="option-row">
            <div class="option-text">
              <span class="option-title">{$_("link_expiration", {}, "Link Expiration")}</span>
              <span class="option-desc">{$_("link_expiration_desc", {}, "Automatically disable access after a time limit")}</span>
            </div>
            <select class="select-styled select-input" bind:value={expiration}>
              <option value="0">{$_("never_expires", {}, "Never Expires")}</option>
              <option value="86400000">{$_("expires_1_day", {}, "1 Day (24 Hours)")}</option>
              <option value="604800000">{$_("expires_7_days", {}, "7 Days (1 Week)")}</option>
              <option value="2592000000">{$_("expires_30_days", {}, "30 Days (1 Month)")}</option>
            </select>
          </div>
        </div>

        {#if errorMsg}
          <div class="alert error">{errorMsg}</div>
        {/if}
        {#if successMsg}
          <div class="alert success">{successMsg}</div>
        {/if}
      {/if}
    </div>

    <!-- Footer -->
    <div class="modal-footer">
      {#if existingShareId}
        <button class="btn-danger" on:click={handleRevoke} disabled={saving}>
          {saving ? $_("revoking", {}, "Revoking...") : $_("revoke_share", {}, "Revoke Link")}
        </button>
      {/if}
      <div class="footer-spacer"></div>
      <button class="btn-cancel" on:click={() => dispatch("close")}>{$_("close", {}, "Close")}</button>
      <button class="btn-primary" on:click={handleCreateOrUpdate} disabled={saving || loading}>
        {saving ? $_("saving", {}, "Saving...") : (existingShareId ? $_("update_share", {}, "Update Share") : $_("create_link", {}, "Create Link"))}
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
    color: #181720;
    border-color: #DCDAE4;
  }

  .modal-card.dark-theme {
    background: #252330;
    color: #ffffff;
    border-color: #3E3A4D;
  }

  .modal-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 18px 20px;
    border-bottom: 1px solid rgba(139, 128, 163, 0.15);
  }

  .light-theme .modal-header {
    border-bottom-color: #DCDAE4;
  }

  .dark-theme .modal-header {
    border-bottom-color: #3E3A4D;
  }

  .header-info {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .header-icon {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: rgba(142, 116, 189, 0.15);
    color: #7D60AC;
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
    color: #8B80A3;
    margin: 2px 0 0 0;
  }

  .close-btn {
    background: transparent;
    border: none;
    cursor: pointer;
    color: #8B80A3;
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

  .loading-wrap {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 30px;
    gap: 12px;
    color: #8B80A3;
  }

  .spinner {
    width: 32px;
    height: 32px;
    border: 3px solid rgba(139, 128, 163, 0.2);
    border-top-color: #7D60AC;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .share-link-section {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .section-label {
    font-size: 13px;
    font-weight: 600;
  }

  .link-input-group {
    display: flex;
    gap: 8px;
  }

  .link-input {
    flex: 1;
    padding: 9px 12px;
    border-radius: 8px;
    font-family: monospace;
    font-size: 13px;
    border: 1px solid rgba(139, 128, 163, 0.3);
    background: rgba(139, 128, 163, 0.08);
    color: inherit;
  }

  .btn-copy {
    padding: 9px 16px;
    background: #7D60AC;
    color: #ffffff;
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: background 0.15s;
  }

  .btn-copy:hover {
    background: #563F7A;
  }

  .link-help {
    font-size: 12px;
    color: #8B80A3;
    margin: 0;
  }

  .options-group {
    display: flex;
    flex-direction: column;
    gap: 14px;
    padding-top: 6px;
  }

  .option-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 12px;
  }

  .option-text {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .option-title {
    font-size: 13px;
    font-weight: 500;
  }

  .option-desc {
    font-size: 11px;
    color: #8B80A3;
  }

  .password-input-wrap {
    margin-top: -6px;
  }

  .text-input {
    width: 100%;
    padding: 8px 12px;
    border-radius: 8px;
    border: 1px solid rgba(139, 128, 163, 0.3);
    background: rgba(139, 128, 163, 0.08);
    color: inherit;
    font-size: 13px;
    box-sizing: border-box;
  }

  .select-input {
    appearance: none;
    -webkit-appearance: none;
    -moz-appearance: none;
    padding: 7px 32px 7px 10px;
    border-radius: 8px;
    border: 1px solid rgba(139, 128, 163, 0.3);
    background-color: rgba(139, 128, 163, 0.08);
    background-repeat: no-repeat;
    background-position: right 10px center;
    background-size: 13px 13px;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='13' height='13' viewBox='0 0 24 24' fill='none' stroke='%238B80A3' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    color: inherit;
    font-size: 13px;
    cursor: pointer;
  }

  .select-input option {
    background-color: #252330;
    color: #ffffff;
  }

  :global(.light-theme) .select-input option,
  :global(html:not(.omd-dark)) .select-input option {
    background-color: #ffffff;
    color: #181720;
  }

  /* Switches */
  .switch {
    position: relative;
    display: inline-block;
    width: 38px;
    height: 22px;
    flex-shrink: 0;
  }

  .switch input {
    opacity: 0;
    width: 0;
    height: 0;
  }

  .slider {
    position: absolute;
    cursor: pointer;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background-color: rgba(139, 128, 163, 0.3);
    transition: 0.2s;
    border-radius: 22px;
  }

  .slider:before {
    position: absolute;
    content: "";
    height: 16px;
    width: 16px;
    left: 3px;
    bottom: 3px;
    background-color: white;
    transition: 0.2s;
    border-radius: 50%;
  }

  input:checked + .slider {
    background-color: #7D60AC;
  }

  input:checked + .slider:before {
    transform: translateX(16px);
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

  .alert.success {
    background: rgba(41, 204, 84, 0.12);
    color: #29cc54;
    border: 1px solid rgba(41, 204, 84, 0.25);
  }

  /* Modal Footer */
  .modal-footer {
    display: flex;
    align-items: center;
    padding: 14px 20px;
    border-top: 1px solid rgba(139, 128, 163, 0.15);
    gap: 10px;
  }

  .light-theme .modal-footer {
    border-top-color: #DCDAE4;
  }

  .dark-theme .modal-footer {
    border-top-color: #3E3A4D;
  }

  .footer-spacer {
    flex: 1;
  }

  .btn-primary {
    padding: 9px 18px;
    background: #7D60AC;
    color: #ffffff;
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s;
  }

  .btn-primary:hover {
    background: #8E74BD;
  }

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

  .btn-danger {
    padding: 9px 14px;
    background: rgba(239, 68, 68, 0.12);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: #ef4444;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
  }

  .btn-danger:hover {
    background: rgba(239, 68, 68, 0.2);
  }
</style>
