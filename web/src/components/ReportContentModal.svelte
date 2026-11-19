<script>
  import { createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n/index.js";

  export let open = false;
  export let isDarkTheme = true;
  export let httpDrive = null;
  export let kind = "chat";
  export let messageId = "";
  export let prompt = "";
  export let contentExcerpt = "";
  export let itemPath = "";
  export let model = "";

  const dispatch = createEventDispatcher();

  const REASONS = [
    { id: "offensive", labelKey: "report_reason_offensive", fallback: "Offensive or hateful content" },
    { id: "nsfw", labelKey: "report_reason_nsfw", fallback: "Sexually explicit / NSFW content" },
    { id: "harm", labelKey: "report_reason_harm", fallback: "Harassment, self-harm, or violence" },
    { id: "misleading", labelKey: "report_reason_misleading", fallback: "Misleading or harmful information" },
    { id: "other", labelKey: "report_reason_other", fallback: "Other" },
  ];

  let selectedReason = "offensive";
  let details = "";
  let submitting = false;
  let submitted = false;

  $: if (open) {
    // Reset only when freshly opened
  }

  function close() {
    submitted = false;
    details = "";
    selectedReason = "offensive";
    dispatch("close");
  }

  async function submitReport() {
    if (submitting) return;
    submitting = true;
    const payload = {
      kind,
      reason: selectedReason,
      details: details.trim(),
      message_id: messageId || "",
      prompt: String(prompt || "").slice(0, 500),
      content_excerpt: String(contentExcerpt || "").slice(0, 500),
      item_path: itemPath || "",
      model: model || "",
    };

    try {
      if (httpDrive && typeof httpDrive.fetch === "function") {
        await httpDrive.fetch("/api/report_content", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
      } else {
        await fetch("https://ai-agent.direct/api/report_content", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
      }
    } catch (_) {
      // Keep UX non-blocking if node is offline; also persist locally for telemetry/audit
    }

    try {
      const key = "omd_content_reports";
      const existing = JSON.parse(localStorage.getItem(key) || "[]");
      existing.push({ ...payload, ts: new Date().toISOString() });
      localStorage.setItem(key, JSON.stringify(existing.slice(-50)));
    } catch (_) {}

    submitting = false;
    submitted = true;
    dispatch("submitted", payload);
  }
</script>

{#if open}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="report-backdrop" class:light-theme={!isDarkTheme} on:click|self={close}>
    <div class="report-modal" role="dialog" aria-modal="true" aria-label={$_("report_ai_content", {}, "Report offensive content")}>
      <div class="report-head">
        <h3>🚩 {$_("report_ai_content", {}, "Report offensive AI content")}</h3>
        <button type="button" class="report-close" on:click={close} aria-label={$_("close", {}, "Close")}>✕</button>
      </div>

      {#if submitted}
        <div class="report-success">
          <p>{$_("report_submitted_msg", {}, "Thank you. Your report has been recorded and will be used to improve content filtering and moderation.")}</p>
          <button type="button" class="report-btn primary" on:click={close}>{$_("done", {}, "Done")}</button>
        </div>
      {:else}
        <div class="report-body">
          <p class="report-hint">
            {$_("report_ai_hint", {}, "Select a reason for flagging this AI-generated output:")}
          </p>
          <div class="report-reasons">
            {#each REASONS as r}
              <label class="reason-row" class:selected={selectedReason === r.id}>
                <input type="radio" name="report_reason" value={r.id} bind:group={selectedReason} />
                <span>{$_(r.labelKey, {}, r.fallback)}</span>
              </label>
            {/each}
          </div>
          <textarea
            class="report-details"
            rows="3"
            bind:value={details}
            placeholder={$_("report_details_placeholder", {}, "Optional details...")}
          ></textarea>
        </div>
        <div class="report-foot">
          <button type="button" class="report-btn" on:click={close}>{$_("cancel", {}, "Cancel")}</button>
          <button type="button" class="report-btn primary" disabled={submitting} on:click={submitReport}>
            {submitting ? $_("sending", {}, "Sending...") : $_("submit_report", {}, "Submit report")}
          </button>
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .report-backdrop {
    position: fixed;
    inset: 0;
    z-index: 9999;
    background: rgba(8, 8, 16, 0.72);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
  }

  .report-modal {
    width: 100%;
    max-width: 420px;
    background: #171723;
    color: #f4f4f8;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 14px;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
    overflow: hidden;
  }

  .report-backdrop.light-theme .report-modal {
    background: #ffffff;
    color: #18181b;
    border-color: rgba(0, 0, 0, 0.12);
  }

  .report-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 16px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.1);
  }

  .report-backdrop.light-theme .report-head {
    border-bottom-color: rgba(0, 0, 0, 0.08);
  }

  .report-head h3 {
    margin: 0;
    font-size: 15px;
    font-weight: 650;
  }

  .report-close {
    background: transparent;
    border: none;
    color: inherit;
    opacity: 0.7;
    cursor: pointer;
    font-size: 16px;
    padding: 4px 6px;
  }

  .report-body {
    padding: 14px 16px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .report-hint {
    margin: 0;
    font-size: 12.5px;
    opacity: 0.8;
  }

  .report-reasons {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .reason-row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 10px;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.1);
    font-size: 13px;
    cursor: pointer;
  }

  .report-backdrop.light-theme .reason-row {
    border-color: rgba(0, 0, 0, 0.1);
  }

  .reason-row.selected {
    border-color: #8b5cf6;
    background: rgba(139, 92, 246, 0.14);
  }

  .report-details {
    width: 100%;
    box-sizing: border-box;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.14);
    background: rgba(0, 0, 0, 0.24);
    color: inherit;
    padding: 8px 10px;
    font-size: 13px;
    resize: vertical;
  }

  .report-backdrop.light-theme .report-details {
    background: #f8f8fb;
    border-color: rgba(0, 0, 0, 0.14);
  }

  .report-foot {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    padding: 12px 16px;
    border-top: 1px solid rgba(255, 255, 255, 0.1);
  }

  .report-backdrop.light-theme .report-foot {
    border-top-color: rgba(0, 0, 0, 0.08);
  }

  .report-btn {
    border-radius: 8px;
    padding: 7px 14px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    border: 1px solid rgba(255, 255, 255, 0.18);
    background: transparent;
    color: inherit;
  }

  .report-btn.primary {
    background: #7c3aed;
    border-color: #7c3aed;
    color: #ffffff;
  }

  .report-btn:disabled {
    opacity: 0.6;
    cursor: default;
  }

  .report-success {
    padding: 18px 16px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    align-items: flex-end;
  }

  .report-success p {
    margin: 0;
    font-size: 13.5px;
    line-height: 1.45;
    width: 100%;
  }
</style>
