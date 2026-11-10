<script>
  import { createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n/index.js";
  import { WebRTCDataChannelDrive } from "../lib/webrtc-drive.js";

  export let open = false;
  export let linkId = "";
  export let deviceName = "";
  export let identity = null;
  export let isDarkTheme = false;

  const dispatch = createEventDispatcher();

  let user = "";
  let password = "";
  let loading = false;
  let errorMsg = "";
  let successMsg = "";
  let modalDrive = null;

  $: if (open && linkId) {
    errorMsg = "";
    successMsg = "";
    password = "";
    initDrive();
  }

  function initDrive() {
    if (!linkId || !linkId.trim()) return;
    const cleanId = linkId.trim();

    if (typeof window !== "undefined" && window.deviceRegistry?.get(cleanId)) {
      modalDrive = window.deviceRegistry.get(cleanId).drive;
    }
    if (!modalDrive) {
      modalDrive = new WebRTCDataChannelDrive(cleanId);
      modalDrive.didPub = identity?.mainPub || null;
    }
    const tryFetchNodeName = async () => {
      try {
        const info = await modalDrive.authInfo();
        if (info?.result?.name) {
          modalDrive.rootName = info.result.name;
          if (!deviceName || deviceName === cleanId) {
            deviceName = info.result.name;
          }
        }
      } catch (_) {}
    };

    if (!modalDrive.isConnected) {
      modalDrive.connect()
        .then(() => tryFetchNodeName())
        .catch((err) => console.warn("[PairModal] P2P pre-connect error:", err));
    } else {
      tryFetchNodeName();
    }
  }

  function handleClose() {
    if (loading) return;
    open = false;
    dispatch("close");
  }

  async function handlePair() {
    if (!linkId || !linkId.trim()) {
      errorMsg = "LinkID is required";
      return;
    }
    if (!user.trim()) {
      errorMsg = "Username is required";
      return;
    }

    loading = true;
    errorMsg = "";
    successMsg = "";

    try {
      if (!modalDrive) {
        initDrive();
      }

      if (!modalDrive.isConnected) {
        const connected = await modalDrive.waitUntilConnected(15000);
        if (!connected) {
          throw new Error("Could not establish P2P connection to device. Make sure device is online.");
        }
      }


      const didPub = identity?.mainPub || "";
      const res = await modalDrive.login(user.trim(), password, didPub);

      if (res.status !== "ok") {
        throw new Error(res.error || "Authentication failed. Invalid username or password.");
      }

      const cleanName = res.result?.name || modalDrive.rootName || deviceName || "My Node (" + linkId.substring(0, 4) + ")";

      if (identity && typeof identity.publishDevice === "function") {
        await identity.publishDevice(linkId.trim(), cleanName, "node", ["files", "terminal"]);
      }

      successMsg = $_("pair_success", {}, "Устройство успешно подключено!");
      dispatch("paired", { linkId: linkId.trim(), name: cleanName, drive: modalDrive });



      setTimeout(() => {
        open = false;
        dispatch("close");
      }, 1000);

    } catch (err) {
      console.error("[PairModal] Pairing error:", err);
      errorMsg = err.message || "Failed to pair device";
    } finally {
      loading = false;
    }
  }
</script>

{#if open}
  <div class="modal-backdrop" on:click|self={handleClose} class:dark={isDarkTheme}>
    <div class="modal-card">
      <div class="modal-header">
        <div class="header-icon">
          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="2" y="2" width="20" height="8" rx="2" ry="2"></rect>
            <rect x="2" y="14" width="20" height="8" rx="2" ry="2"></rect>
            <line x1="6" y1="6" x2="6.01" y2="6"></line>
            <line x1="6" y1="18" x2="6.01" y2="18"></line>
          </svg>
        </div>
        <div class="header-text">
          <h3>{$_("pair_device_title", {}, "Connect Storage Device")}</h3>
          <p class="header-sub">{deviceName ? deviceName : $_("omd_node", {}, "Disk Direct Node")} ({linkId})</p>
        </div>
        <button class="close-btn" on:click={handleClose} aria-label={$_("close", {}, "Close")}>
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="18" y1="6" x2="6" y2="18"></line>
            <line x1="6" y1="6" x2="18" y2="18"></line>
          </svg>
        </button>
      </div>

      <form on:submit|preventDefault={handlePair} class="modal-body">
        <p class="modal-desc">
          {$_("pair_device_subtitle", {}, "Enter host system username and password to authenticate and add to your trusted DID device list.")}
        </p>

        <div class="form-group">
          <label for="pair-user">{$_("username", {}, "Username")}</label>
          <input
            id="pair-user"
            type="text"
            bind:value={user}
            placeholder="username"
            disabled={loading || !!successMsg}
            required
            autocomplete="username"
          />
        </div>

        <div class="form-group">
          <label for="pair-pass">{$_("password", {}, "Password")}</label>
          <input
            id="pair-pass"
            type="password"
            bind:value={password}
            placeholder="••••••••"
            disabled={loading || !!successMsg}
            required
            autocomplete="current-password"
          />
        </div>

        {#if errorMsg}
          <div class="alert alert-error">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="12" cy="12" r="10"></circle>
              <line x1="12" y1="8" x2="12" y2="12"></line>
              <line x1="12" y1="16" x2="12.01" y2="16"></line>
            </svg>
            <span>{errorMsg}</span>
          </div>
        {/if}

        {#if successMsg}
          <div class="alert alert-success">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"></path>
              <polyline points="22 4 12 14.01 9 11.01"></polyline>
            </svg>
            <span>{successMsg}</span>
          </div>
        {/if}

        <div class="modal-actions">
          <button type="button" class="btn btn-secondary" on:click={handleClose} disabled={loading}>
            {$_("cancel", {}, "Cancel")}
          </button>
          <button type="submit" class="btn btn-primary" disabled={loading || !user.trim() || !!successMsg}>
            {#if loading}
              <span class="spinner"></span>
              <span>{$_("connecting", {}, "Connecting...")}</span>
            {:else}
              <span>{$_("pair_connect_btn", {}, "Connect")}</span>
            {/if}
          </button>
        </div>
      </form>
    </div>
  </div>
{/if}

<style>
  .modal-backdrop {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.65);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100000;
    padding: 1rem;
  }

  .modal-card {
    background: #ffffff;
    color: #1e293b;
    border-radius: 14px;
    width: 100%;
    max-width: 440px;
    box-shadow: 0 20px 30px -10px rgba(0, 0, 0, 0.4);
    overflow: hidden;
    animation: modal-in 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  .dark .modal-card {
    background: #1e293b;
    color: #f8fafc;
    border: 1px solid #334155;
  }

  @keyframes modal-in {
    from { opacity: 0; transform: scale(0.96) translateY(8px); }
    to { opacity: 1; transform: scale(1) translateY(0); }
  }

  .modal-header {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    padding: 1.25rem 1.5rem;
    border-bottom: 1px solid #e2e8f0;
  }
  .dark .modal-header {
    border-bottom-color: #334155;
  }

  .header-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: #eff6ff;
    color: #0ea5e9;
    flex-shrink: 0;
  }
  .dark .header-icon {
    background: rgba(14, 165, 233, 0.15);
    color: #38bdf8;
  }

  .header-text {
    flex: 1;
    min-width: 0;
  }
  .header-text h3 {
    margin: 0;
    font-size: 1.1rem;
    font-weight: 600;
  }
  .header-sub {
    margin: 0.15rem 0 0;
    font-size: 0.8125rem;
    color: #64748b;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  }
  .dark .header-sub {
    color: #94a3b8;
  }

  .close-btn {
    background: none;
    border: none;
    cursor: pointer;
    color: #64748b;
    padding: 0.35rem;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s;
  }
  .close-btn:hover {
    background: #f1f5f9;
    color: #0f172a;
  }
  .dark .close-btn:hover {
    background: #334155;
    color: #f8fafc;
  }

  .modal-body {
    padding: 1.5rem;
  }

  .modal-desc {
    margin: 0 0 1.25rem;
    font-size: 0.875rem;
    line-height: 1.45;
    color: #64748b;
  }
  .dark .modal-desc {
    color: #94a3b8;
  }

  .form-group {
    margin-bottom: 1.1rem;
  }
  .form-group label {
    display: block;
    font-size: 0.8125rem;
    font-weight: 500;
    margin-bottom: 0.4rem;
    color: #475569;
  }
  .dark .form-group label {
    color: #cbd5e1;
  }

  .form-group input {
    width: 100%;
    box-sizing: border-box;
    padding: 0.65rem 0.85rem;
    border-radius: 8px;
    border: 1px solid #cbd5e1;
    background: #ffffff;
    color: #0f172a;
    font-size: 0.9375rem;
    outline: none;
    transition: border-color 0.15s, box-shadow 0.15s;
  }
  .form-group input:focus {
    border-color: #0ea5e9;
    box-shadow: 0 0 0 3px rgba(14, 165, 233, 0.2);
  }
  .dark .form-group input {
    background: #0f172a;
    border-color: #334155;
    color: #f8fafc;
  }
  .dark .form-group input:focus {
    border-color: #38bdf8;
    box-shadow: 0 0 0 3px rgba(56, 189, 248, 0.25);
  }

  .alert {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.65rem 0.85rem;
    border-radius: 8px;
    font-size: 0.8125rem;
    margin-bottom: 1.25rem;
  }
  .alert-error {
    background: #fef2f2;
    color: #ef4444;
    border: 1px solid #fecaca;
  }
  .dark .alert-error {
    background: rgba(239, 68, 68, 0.15);
    border-color: rgba(239, 68, 68, 0.3);
  }
  .alert-success {
    background: #f0fdf4;
    color: #22c55e;
    border: 1px solid #bbf7d0;
  }
  .dark .alert-success {
    background: rgba(34, 197, 94, 0.15);
    border-color: rgba(34, 197, 94, 0.3);
  }

  .modal-actions {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 0.75rem;
    margin-top: 1.5rem;
  }

  .btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.5rem;
    padding: 0.65rem 1.2rem;
    border-radius: 8px;
    font-size: 0.875rem;
    font-weight: 500;
    cursor: pointer;
    border: none;
    transition: all 0.15s ease;
  }
  .btn:disabled {
    opacity: 0.6;
    cursor: not-allowed;
  }

  .btn-secondary {
    background: #f1f5f9;
    color: #475569;
  }
  .btn-secondary:hover:not(:disabled) {
    background: #e2e8f0;
    color: #0f172a;
  }
  .dark .btn-secondary {
    background: #334155;
    color: #cbd5e1;
  }
  .dark .btn-secondary:hover:not(:disabled) {
    background: #475569;
    color: #f8fafc;
  }

  .btn-primary {
    background: #0ea5e9;
    color: #ffffff;
  }
  .btn-primary:hover:not(:disabled) {
    background: #0284c7;
  }
  .dark .btn-primary {
    background: #38bdf8;
    color: #0f172a;
    font-weight: 600;
  }
  .dark .btn-primary:hover:not(:disabled) {
    background: #0ea5e9;
  }

  .spinner {
    width: 14px;
    height: 14px;
    border: 2px solid currentColor;
    border-top-color: transparent;
    border-radius: 50%;
    animation: spin 0.7s linear infinite;
  }
  @keyframes spin {
    to { transform: rotate(360deg); }
  }
</style>
