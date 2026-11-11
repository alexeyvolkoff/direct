<script>
  import { createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n/index.js";
  import {
    gatewayConfig,
    DEFAULT_GATEWAY_ORIGIN,
    fetchGatewayManifest,
    isGatewayTosAccepted,
    acceptGatewayTos,
    activateGateway,
    resetGatewayToDefault,
  } from "../lib/gateway-config.js";

  export let open = false;
  export let initialUrl = "";
  export let isDarkTheme = true;

  const dispatch = createEventDispatcher();

  let inputUrl = "";
  let step = "input"; // "input" | "confirm"
  let loading = false;
  let errorMsg = "";
  let pendingManifest = null;
  let tosChecked = false;
  let logoBroken = false;
  let lastOpenState = false;

  $: if (open && !lastOpenState) {
    lastOpenState = true;
    errorMsg = "";
    loading = false;
    logoBroken = false;
    pendingManifest = null;
    if (initialUrl && initialUrl.trim()) {
      inputUrl = initialUrl.trim();
      step = "input";
      probeGateway();
    } else {
      inputUrl = $gatewayConfig?.isCustom ? $gatewayConfig.origin : "";
      step = "input";
    }
  } else if (!open && lastOpenState) {
    lastOpenState = false;
  }

  function closeModal() {
    if (loading) return;
    open = false;
    dispatch("close");
  }

  async function probeGateway() {
    const trimmed = String(inputUrl || "").trim();
    if (!trimmed) {
      errorMsg = $_("gateway_err_empty", {}, "Please enter a gateway server URL.");
      return;
    }
    loading = true;
    errorMsg = "";
    logoBroken = false;
    try {
      const manifest = await fetchGatewayManifest(trimmed);
      pendingManifest = manifest;
      tosChecked = isGatewayTosAccepted(manifest);
      step = "confirm";
    } catch (err) {
      errorMsg = err?.message || $_("gateway_err_unreachable", {}, "Could not connect to gateway server.");
    } finally {
      loading = false;
    }
  }

  function handleConfirmConnect() {
    if (!pendingManifest || !tosChecked) return;
    acceptGatewayTos(pendingManifest);
    const activated = activateGateway(pendingManifest);
    open = false;
    dispatch("switched", { gateway: activated });
    dispatch("close");
  }

  function handleResetDefault() {
    const def = resetGatewayToDefault();
    open = false;
    dispatch("switched", { gateway: def });
    dispatch("close");
  }

  $: previewLogo = pendingManifest
    ? (isDarkTheme ? pendingManifest.skin?.logoDark : pendingManifest.skin?.logoLight)
    : "";
</script>

{#if open}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="gw-modal-backdrop" on:click|self={closeModal}>
    <div class="gw-modal-card" role="dialog" aria-modal="true" aria-labelledby="gw-modal-title">
      <button class="gw-close-btn" type="button" on:click={closeModal} aria-label="Close">&times;</button>

      {#if step === "input"}
        <div class="gw-header">
          <div class="gw-icon-circle">
            <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="12" r="10" />
              <line x1="2" y1="12" x2="22" y2="12" />
              <path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z" />
            </svg>
          </div>
          <div>
            <h3 id="gw-modal-title" class="gw-title">{$_("gateway_switch_title", {}, "Connect to Gateway")}</h3>
            <p class="gw-subtitle">
              {$_("gateway_switch_sub", {}, "Enter the address of a compatible community or enterprise gateway.")}
            </p>
          </div>
        </div>

        <div class="gw-current-pill">
          <span class="gw-current-lbl">{$_("gateway_current", {}, "Active")}:</span>
          <strong class="gw-current-val">{$gatewayConfig.brandName}</strong>
          <code class="gw-current-url">{$gatewayConfig.origin}</code>
        </div>

        <form on:submit|preventDefault={probeGateway} class="gw-form">
          <label class="gw-label" for="gw-url-input">{$_("gateway_server_url", {}, "Gateway Server URL")}</label>
          <input
            id="gw-url-input"
            class="gw-input"
            type="url"
            inputmode="url"
            autocapitalize="off"
            autocorrect="off"
            spellcheck="false"
            placeholder="https://..."
            bind:value={inputUrl}
            disabled={loading}
          />

          <p class="gw-did-note">
            {$_("gateway_did_preserved_note", {}, "Your Decentralized Identity (DID) and local encryption keys remain unchanged.")}
          </p>

          {#if errorMsg}
            <div class="gw-error">{errorMsg}</div>
          {/if}

          <div class="gw-actions">
            {#if $gatewayConfig.isCustom}
              <button type="button" class="gw-btn gw-btn-ghost" on:click={handleResetDefault} disabled={loading}>
                {$_("gateway_reset_default", {}, "Reset to Default")}
              </button>
            {/if}
            <div class="gw-actions-right">
              <button type="button" class="gw-btn gw-btn-secondary" on:click={closeModal} disabled={loading}>
                {$_("cancel", {}, "Cancel")}
              </button>
              <button type="submit" class="gw-btn gw-btn-primary" disabled={loading}>
                {loading ? $_("checking", {}, "Checking…") : $_("continue", {}, "Continue")}
              </button>
            </div>
          </div>
        </form>
      {:else if step === "confirm" && pendingManifest}
        <div class="gw-header">
          <div class="gw-icon-circle">
            <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z" />
            </svg>
          </div>
          <div>
            <h3 id="gw-modal-title" class="gw-title">{$_("gateway_tos_title", {}, "Gateway Terms & Branding")}</h3>
            <p class="gw-subtitle">
              {$_("gateway_tos_sub", {}, "Review the gateway details and accept its Terms of Service before connecting.")}
            </p>
          </div>
        </div>

        <div class="gw-preview-box">
          {#if previewLogo && !logoBroken}
            <img
              src={previewLogo}
              alt={pendingManifest.brandName}
              class="gw-preview-logo"
              on:error={() => { logoBroken = true; }}
            />
          {/if}
          <div class="gw-preview-meta">
            <div class="gw-preview-name">{pendingManifest.brandName}</div>
            <div class="gw-preview-origin">{pendingManifest.origin}</div>
          </div>
        </div>

        <div class="gw-links-box">
          <a href={pendingManifest.tosUrl} target="_blank" rel="noopener noreferrer" class="gw-tos-link">
            <span>{$_("terms_of_service", {}, "Terms of Service")}</span>
            <span aria-hidden="true">&nearr;</span>
          </a>
          <a href={pendingManifest.privacyUrl} target="_blank" rel="noopener noreferrer" class="gw-tos-link">
            <span>{$_("privacy_policy", {}, "Privacy Policy")}</span>
            <span aria-hidden="true">&nearr;</span>
          </a>
        </div>

        {#if pendingManifest.origin !== DEFAULT_GATEWAY_ORIGIN}
          <label class="gw-checkbox-row">
            <input type="checkbox" bind:checked={tosChecked} />
            <span>
              {$_("gateway_tos_accept_prefix", {}, "I have read and agree to the Terms of Service and Privacy Policy of")}
              <strong>{pendingManifest.brandName}</strong>.
            </span>
          </label>
        {/if}

        <div class="gw-actions">
          <button
            type="button"
            class="gw-btn gw-btn-ghost"
            on:click={() => { step = "input"; errorMsg = ""; }}
          >
            &larr; {$_("back", {}, "Back")}
          </button>
          <div class="gw-actions-right">
            <button type="button" class="gw-btn gw-btn-secondary" on:click={closeModal}>
              {$_("cancel", {}, "Cancel")}
            </button>
            <button
              type="button"
              class="gw-btn gw-btn-primary"
              disabled={!tosChecked}
              on:click={handleConfirmConnect}
            >
              {$_("gateway_accept_connect", {}, "Accept & Connect")}
            </button>
          </div>
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .gw-modal-backdrop {
    position: fixed;
    inset: 0;
    z-index: 10050;
    background: rgba(8, 10, 16, 0.62);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
  }

  .gw-modal-card {
    position: relative;
    width: 100%;
    max-width: 460px;
    background: var(--bg-card, #1F2430);
    color: var(--text-primary, #EEF0F6);
    border: 1px solid var(--border-color, #32394B);
    border-radius: 16px;
    padding: 24px;
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.38);
  }

  .gw-close-btn {
    position: absolute;
    top: 14px;
    right: 16px;
    background: transparent;
    border: none;
    color: var(--text-muted, #7E869B);
    font-size: 24px;
    line-height: 1;
    cursor: pointer;
    padding: 4px;
  }

  .gw-close-btn:hover {
    color: var(--text-primary, #EEF0F6);
  }

  .gw-header {
    display: flex;
    align-items: flex-start;
    gap: 12px;
    margin-bottom: 16px;
    padding-right: 20px;
  }

  .gw-icon-circle {
    flex-shrink: 0;
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: var(--accent-soft, rgba(123, 134, 196, 0.18));
    color: var(--accent, #7B86C4);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .gw-title {
    margin: 0 0 4px;
    font-size: 17px;
    font-weight: 600;
    color: var(--text-primary, #EEF0F6);
  }

  .gw-subtitle {
    margin: 0;
    font-size: 13px;
    line-height: 1.45;
    color: var(--text-secondary, #AEB5C7);
  }

  .gw-current-pill {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px;
    padding: 9px 12px;
    border-radius: 10px;
    background: var(--bg-soft, rgba(123, 134, 196, 0.09));
    border: 1px solid var(--border-color, #32394B);
    font-size: 12.5px;
    margin-bottom: 16px;
  }

  .gw-current-lbl {
    color: var(--text-muted, #7E869B);
  }

  .gw-current-val {
    color: var(--text-primary, #EEF0F6);
    font-weight: 600;
  }

  .gw-current-url {
    margin-left: auto;
    font-size: 12px;
    color: var(--text-secondary, #AEB5C7);
    background: transparent;
  }

  .gw-form {
    display: flex;
    flex-direction: column;
  }

  .gw-label {
    font-size: 12.5px;
    font-weight: 600;
    color: var(--text-secondary, #AEB5C7);
    margin-bottom: 6px;
  }

  .gw-input {
    width: 100%;
    box-sizing: border-box;
    padding: 10px 12px;
    border-radius: 10px;
    border: 1px solid var(--border-color, #32394B);
    background: var(--bg-primary, #151821);
    color: var(--text-primary, #EEF0F6);
    font-size: 14px;
    outline: none;
  }

  .gw-input:focus {
    border-color: var(--accent, #7B86C4);
  }

  .gw-did-note {
    margin: 10px 0 0;
    font-size: 12px;
    line-height: 1.45;
    color: var(--text-muted, #7E869B);
  }

  .gw-error {
    margin-top: 12px;
    padding: 9px 12px;
    border-radius: 8px;
    background: var(--danger-bg, rgba(248, 59, 66, 0.14));
    color: var(--danger-text, #FF6A6F);
    font-size: 12.5px;
  }

  .gw-preview-box {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 14px;
    border-radius: 12px;
    background: var(--bg-primary, #151821);
    border: 1px solid var(--border-color, #32394B);
    margin-bottom: 12px;
  }

  .gw-preview-logo {
    max-height: 36px;
    max-width: 140px;
    object-fit: contain;
  }

  .gw-preview-meta {
    min-width: 0;
  }

  .gw-preview-name {
    font-size: 15px;
    font-weight: 600;
    color: var(--text-primary, #EEF0F6);
  }

  .gw-preview-origin {
    font-size: 12.5px;
    color: var(--text-secondary, #AEB5C7);
    word-break: break-all;
  }

  .gw-links-box {
    display: flex;
    gap: 10px;
    margin-bottom: 14px;
  }

  .gw-tos-link {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 9px 12px;
    border-radius: 9px;
    border: 1px solid var(--border-color, #32394B);
    background: var(--bg-soft, rgba(123, 134, 196, 0.08));
    color: var(--accent, #7B86C4);
    text-decoration: none;
    font-size: 13px;
    font-weight: 500;
  }

  .gw-tos-link:hover {
    border-color: var(--accent, #7B86C4);
  }

  .gw-checkbox-row {
    display: flex;
    align-items: flex-start;
    gap: 10px;
    font-size: 13px;
    line-height: 1.45;
    color: var(--text-secondary, #AEB5C7);
    cursor: pointer;
    margin-bottom: 4px;
  }

  .gw-checkbox-row input[type="checkbox"] {
    margin-top: 3px;
    accent-color: var(--accent, #5863A6);
  }

  .gw-actions {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin-top: 18px;
  }

  .gw-actions-right {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-left: auto;
  }

  .gw-btn {
    padding: 8px 14px;
    border-radius: 9px;
    font-size: 13.5px;
    font-weight: 500;
    cursor: pointer;
    border: 1px solid transparent;
    transition: opacity 0.15s ease, background 0.15s ease;
  }

  .gw-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .gw-btn-primary {
    background: var(--wx-color-primary, var(--accent, #5863A6));
    color: #FFFFFF;
  }

  .gw-btn-primary:hover:not(:disabled) {
    background: var(--wx-color-primary-hover, var(--accent-hover, #6874BA));
  }

  .gw-btn-secondary {
    background: transparent;
    border-color: var(--border-color, #32394B);
    color: var(--text-secondary, #AEB5C7);
  }

  .gw-btn-ghost {
    background: transparent;
    color: var(--text-muted, #7E869B);
    padding-left: 6px;
    padding-right: 6px;
  }

  .gw-btn-ghost:hover:not(:disabled) {
    color: var(--text-primary, #EEF0F6);
  }
</style>
