<script>
  import { onMount, onDestroy } from 'svelte';
  import { Terminal } from '@xterm/xterm';
  import { FitAddon } from '@xterm/addon-fit';
  import { CanvasAddon } from '@xterm/addon-canvas';
  import '@xterm/xterm/css/xterm.css';

  import { WebRTCDataChannelDrive } from '../lib/webrtc-drive.js';
  import { SshClient } from '../lib/ssh-client.js';
  import { deviceRegistry } from '../lib/device-registry.js';
  import { getSshCredential, saveSshCredential } from '../lib/identity/ssh-credentials.js';
  import { getDomainBrandTitle } from '../lib/app-mode.js';

  export let targetLinkId = "";
  export let port = 22;
  export let service = "terminal";
  export let user = "";
  export let pass = "";
  export let initialPath = "";
  // Optional callback receiving live session metadata:
  // { cwd, title, branch, procTitle } — replaces two-way bind: props.
  // Under Svelte 5 legacy mode, a child writing into props that the parent
  // binds inside a keyed {#each} block permanently wedges the parent's
  // reactive updates (tabs/sidebar/status stop rendering), so metadata is
  // pushed via callback instead.
  export let onmeta = null;
  export let onClose = null; // Optional close callback
  export let showCloseButton = false;
  // FS-RPC drive (no port/service), used to run the quiet per-session init via
  // the node Exec op instead of spraying hook lines over the shell.
  export let fsDrive = null;
  // Per-terminal tag namespacing the init script file, so concurrent sessions
  // on the same device cannot overwrite each other's init.
  export let initTag = "";

  $: deviceObj = targetLinkId ? (deviceRegistry.getByPrefix(targetLinkId) || deviceRegistry.get(targetLinkId)) : null;
  $: deviceName = deviceObj?.name || targetLinkId;

  // Escape a path segment for embedding inside double quotes, so that the
  // literal "$HOME/..." form survives special characters like $, ` or \.
  function dqEscape(p) {
    return String(p)
      .replace(/\\/g, "\\\\")
      .replace(/\$/g, "\\$")
      .replace(/`/g, "\\`");
  }

  // Shell hook: at every prompt reports cwd + git branch via the invisible
  // OSC 7777;cwd;branch sequence (bash + zsh; other shells fall back to the
  // in-browser tracker). Both lines are harmless in the other shell.
  const SHELL_HOOK =
    "__omd_meta(){ " +
    "if [ \"${PWD#$HOME}\" != \"$PWD\" ]; then __omd_c=\"~${PWD#$HOME}\"; else __omd_c=\"$PWD\"; fi; " +
    "printf '\\033]7777;%s;%s\\007' \"$__omd_c\" \"$(git symbolic-ref --short -q HEAD 2>/dev/null)\"; };";
  const SHELL_HOOK_BASH = SHELL_HOOK + "PROMPT_COMMAND=\"__omd_meta${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"";
  const SHELL_HOOK_ZSH = SHELL_HOOK + "precmd_functions=(__omd_meta $precmd_functions)";

  // The tilde form is built with ~ + ${PWD#$HOME} because a literal `~`
  // inside ${PWD/#$HOME/~} gets re-expanded to $HOME by tilde expansion.

  // Logical cwd tracker (best-effort, for the browser debug console only).
  // The real shell is authoritative; we just follow the commands the user types.
  let debugCwd = "~";
  let debugCwdPrev = "~";
  let inputBuf = "";

  // Authoritative metadata delivered by the shell hooks via OSC 7777;cwd;branch.
  let metaCwd = "";
  let metaBranch = "";
  let prevDocTitle = "";
  // Guards the reactive document.title write: in Svelte 5 pending effects can
  // flush AFTER onDestroy restored the previous title, re-writing "~" into it.
  let alive = true;

  // bash emits its default prompt title (user@host: cwd) on every prompt via
  // PS1's OSC 0 — that would drown out any real process title. Treat titles
  // matching this pattern as the shell default and ignore them.
  const SHELL_TITLE_RE = /^[^@\s]+@[^@:\s]+:\s*/;

  // Tab title: a real process-set title (e.g. OpenCode in a TUI) wins;
  // otherwise "<cwd> (<branch>)" from the shell hook.
  let liveProcTitle = "";
  $: effectiveCwd = metaCwd || debugCwd;
  $: tabTitle =
    liveProcTitle && !SHELL_TITLE_RE.test(liveProcTitle)
      ? liveProcTitle
      : effectiveCwd + (metaBranch ? " (" + metaBranch + ")" : "");
  $: if (typeof document !== "undefined" && alive) document.title = tabTitle;

  // Push session metadata to the host once per change (deduped by signature).
  // cwd is only published once the session is live, so the initial startPath
  // survives while the login prompt is still shown.
  let lastMetaSig = "";
  $: emitMeta(isConnected ? effectiveCwd : null, tabTitle, metaBranch, liveProcTitle);
  function emitMeta(cwd, metaTitle, metaBranchName, proc) {
    const sig = cwd + "\u0000" + metaTitle + "\u0000" + metaBranchName + "\u0000" + proc;
    if (sig === lastMetaSig) return;
    lastMetaSig = sig;
    if (onmeta) onmeta({ cwd, title: metaTitle, branch: metaBranchName, procTitle: proc });
  }

  function resolveCwd(target) {
    let t = (target || "").trim();
    if (
      (t.startsWith('"') && t.endsWith('"')) ||
      (t.startsWith("'") && t.endsWith("'"))
    ) {
      t = t.slice(1, -1).trim();
    }
    const prev = debugCwd;
    if (t === "-") {
      debugCwd = debugCwdPrev;
      debugCwdPrev = prev;
      return debugCwd;
    }
    debugCwdPrev = prev;
    if (!t || t === "~") {
      debugCwd = "~";
      return debugCwd;
    }
    let base;
    if (t.startsWith("~")) {
      base = "~";
      t = t.slice(1);
    } else if (t.startsWith("/")) {
      base = "";
    } else {
      base = debugCwd;
    }
    const homeRel = base === "~" || base.startsWith("~");
    const basePath = homeRel ? (base === "~" ? "" : base.slice(1)) : base;
    const parts = [];
    for (const seg of (basePath + "/" + t).split("/")) {
      if (!seg || seg === ".") continue;
      if (seg === "..") {
        parts.pop();
        continue;
      }
      parts.push(seg);
    }
    debugCwd =
      parts.length > 0
        ? (homeRel ? "~" : "") + "/" + parts.join("/")
        : homeRel
          ? "~"
          : "/";
    return debugCwd;
  }

  // The line the user is currently editing, as rendered by xterm. After Tab
  // completion the shell rewrote it with the completed command (readline
  // echoes the whole line), so this is the authoritative text.
  function lineFromBuffer() {
    try {
      const buf = term.buffer.active;
      const line = buf.getLine(buf.baseY + buf.cursorY);
      if (!line) return null;
      const text = line.translateToString(true);
      return text && text.trim() ? text : null;
    } catch (e) {
      return null;
    }
  }

  function handleSubmittedLine(lineText) {
    // Prefer the buffer line (includes Tab completion) over the raw typed
    // text; fall back to inputBuf when the echo hasn't arrived yet.
    const typed = inputBuf.trim();
    let cmd = typed;
    if (typed && lineText) {
      const idx = lineText.lastIndexOf(typed);
      if (idx >= 0) {
        cmd = lineText.slice(idx).trim();
      }
    }
    // Best-effort: resolve the LAST cd of a chained command
    // (cd a; cd b / cd a && cd b / ls && cd b), ignoring the rest.
    let last = "";
    for (const seg of cmd.split(/[;&|]+/)) {
      const s = seg.trim();
      if (s === "cd" || s.startsWith("cd ")) last = s;
    }
    if (!last) return;
    let rest = last.slice(2).trim();
    const tokens = rest.split(/\s+/);
    while (tokens.length && tokens[0].startsWith("-") && tokens[0] !== "-") {
      tokens.shift();
    }
    rest = tokens.join(" ");
    const cwd = resolveCwd(rest);
    console.log("[Terminal] cd ->", cwd);
  }

  let terminalContainer;
  let term;
  let fitAddon;
  let canvasAddon;
  let resizeObserver;
  let client = null;
  let drive = null;
  let isConnected = false;
  let channelReady = false;
  let errorMsg = "";
  let promptError = "";
  let statusMsg = "";
  let initLoading = false;
  let showPrompt = false;
  let credsUser = user;
  let credsPass = pass;
  // Whether a successful manual login should be remembered in the SSH vault
  // (Security & Keys → SSH Access) for quick-login next time.
  let remember = true;

  // Shell output activity tracking: used to delay the init `source` line until
  // the login shell has printed its first prompt. Bytes typed before readline
  // initializes are kernel-echoed, and that early echo line survives the
  // in-script line erase (reproduced in pty + seen on the real device: a bare
  // ` source ...` line left above the prompt).
  let shellOutputSeen = false;
  let lastShellOutputAt = 0;
  let outTail = "";

  onMount(() => {
    const curr = typeof document !== "undefined" ? document.title : "";
    prevDocTitle = (!curr || curr === "~" || curr.startsWith("~")) ? "Disk Direct" : curr;
    term = new Terminal({
      cursorBlink: true,
      fontFamily: 'Consolas, "Ubuntu Mono", Menlo, "Courier New", monospace',
      fontSize: 14,
      letterSpacing: 0,
      customGlyphs: true,
      theme: {
        background: '#1F2430',
        foreground: '#F6F7FA',
        cursor: '#929CD4',
        selectionBackground: 'rgba(123, 134, 196, 0.3)'
      }
    });

    // Forcefully disable DEC 2026 (Synchronized Output mode) in xterm.js core.
    // Over fragmented P2P/TURN data channels, BSU (?2026h) sets decPrivateModes.synchronizedOutput = true,
    // which causes xterm's renderRows() to abort and freeze the screen indefinitely.
    try {
      const coreService = term._core?.coreService;
      if (coreService && coreService.decPrivateModes) {
        Object.defineProperty(coreService.decPrivateModes, 'synchronizedOutput', {
          get: () => false,
          set: () => {},
          configurable: true
        });
      }
    } catch (err) {}

    fitAddon = new FitAddon();
    term.loadAddon(fitAddon);

    term.open(terminalContainer);
    try {
      canvasAddon = new CanvasAddon();
      term.loadAddon(canvasAddon);
    } catch (e) {
      console.warn("Canvas addon failed to load", e);
    }
    fitAddon.fit();

    // Регистрируем обработчик (замени 1337 на код, который шлет твой opencode)
    term.parser.registerOscHandler(1337, (data) => {
      console.log("Получена кастомная команда от opencode:", data);
      return true; 
    });

    // OSC 52 (Clipboard copy)
    term.parser.registerOscHandler(52, (data) => {
      try {
        // format: "c;BASE64" or ";BASE64"
        const parts = data.split(';');
        if (parts.length >= 2) {
          const base64Data = parts[1];
          const binString = atob(base64Data);
          const bytes = Uint8Array.from(binString, (m) => m.codePointAt(0));
          const text = new TextDecoder().decode(bytes);
          navigator.clipboard.writeText(text).catch(err => {
            console.error("OSC 52 Clipboard write failed:", err);
          });
          return true;
        }
      } catch (e) {
        console.error("OSC 52 Parse error:", e);
      }
      return false;
    });

    // OSC 7777;cwd;branch — emitted by our shell hook at every prompt.
    // xterm hands over the payload without the leading "7777;" identifier.
    term.parser.registerOscHandler(7777, (data) => {
      try {
        const sep = data.indexOf(";");
        const cwdPart = sep >= 0 ? data.slice(0, sep) : data;
        const branchPart = sep >= 0 ? data.slice(sep + 1) : "";
        if (!cwdPart) return false;
        metaCwd = cwdPart;
        metaBranch = branchPart;
        console.log("[Terminal] shell cwd =", cwdPart, "| branch:", branchPart || "-");
      } catch (e) {
        console.error("OSC 7777 parse error:", e);
        return false;
      }
      return true;
    });

    // Process-set window title (OSC 0/2), e.g. OpenCode in a TUI. Exposed raw
    // to Workspace for the status bar; the tab title formula additionally
    // ignores bash's default prompt pattern (user@host: cwd).
    term.onTitleChange((title) => {
      liveProcTitle = String(title || "");
      console.log("[Terminal] process title:", liveProcTitle);
    });

    term.attachCustomKeyEventHandler((e) => {
      if (e.key === 'Enter' && e.shiftKey && e.type === 'keydown') {
        // Send Alt+Enter (\x1b\r) as a fallback for Shift+Enter
        if (client && !client.closed) {
          client.write('\x1b\r');
        }
        return false; // prevent default handling
      }
      return true;
    });

    term.onData((data) => {
      if (client && !client.closed) {
        client.write(data);
      }
      // Track the logical cwd by following the commands the user submits.
      for (const ch of data) {
        if (ch === "\r") {
          handleSubmittedLine(lineFromBuffer());
          inputBuf = "";
        } else if (ch === "\x1b" || ch === "\u0003" || ch === "\u000c") {
          inputBuf = "";
        } else if (ch === "\x7f" || ch === "\b") {
          inputBuf = inputBuf.slice(0, -1);
        } else if (ch !== "\t") {
          inputBuf += ch;
        }
      }
    });

    term.onBinary((data) => {
      if (client && !client.closed) {
        client.write(data);
      }
    });

    resizeObserver = new ResizeObserver(() => {
      if (fitAddon) {
        fitAddon.fit();
        if (client && !client.closed) {
          client.resize(term.cols, term.rows);
        }
      }
    });
    resizeObserver.observe(terminalContainer);

    if (targetLinkId) {
      startSession();
    } else {
      errorMsg = "No target device (linkID)";
    }
  });

  // Opens the session automatically: loads the SSH module and establishes the
  // P2P channel to the device. Auth happens separately (auto if creds given,
  // otherwise the login prompt stays on top of the live channel).
  async function startSession() {
    errorMsg = "";
    promptError = "";
    statusMsg = "Loading SSH module...";

    const streamDecoder = new TextDecoder('utf-8');

    try {
      client = new SshClient({
        onData: (msg) => {
          shellOutputSeen = true;
          lastShellOutputAt = Date.now();
          try {
            const s = typeof msg === "string" ? msg : streamDecoder.decode(msg, { stream: true });
            outTail = (outTail + s
              .replace(/\x1b\][^\x07]*\x07/g, "")
              .replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "")
              .replace(/[\x00-\x08\x0b-\x1f\x7f]/g, "")).slice(-300);
            term.write(s);
          } catch (e) {
            console.error("[XTERM WRITE ERROR]:", e);
          }
        },
        onStatus: (s) => { statusMsg = s; },
        onError: (e) => {
          if (errorMsg === "") errorMsg = String(e);
        }
      });
      await client.init();

      statusMsg = "Opening P2P channel...";
      drive = new WebRTCDataChannelDrive(targetLinkId, "/api/signaling", port, service);

      const pendingSendQueue = [];
      const flushQueue = () => {
        const dc = drive ? drive.dc : null;
        if (dc && dc.readyState === 'open') {
          while (pendingSendQueue.length > 0) {
            const queued = pendingSendQueue.shift();
            dc.send(queued);
          }
        }
      };

      client.bind({
        send: (bytes) => {
          let packet;
          if (bytes instanceof Uint8Array || ArrayBuffer.isView(bytes)) {
            packet = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
          } else {
            packet = bytes;
          }
          const len = packet.byteLength !== undefined ? packet.byteLength : (packet.length || 0);
          console.log(`[P2P DC SEND] ${len} bytes to WebRTC`);

          const dc = drive ? drive.dc : null;
          if (dc && dc.readyState === 'open') {
            try {
              flushQueue();
              dc.send(packet);
            } catch (e) {
              console.error("[Terminal P2P DC send error]:", e);
            }
          } else {
            console.log(`[P2P DC QUEUED] ${len} bytes (DC not open yet)`);
            pendingSendQueue.push(packet);
          }
        }
      });

      drive.onRawMessage = (data) => {
        const len = data instanceof ArrayBuffer ? data.byteLength : (data.length || 0);
        console.log(`[P2P DC RECV] ${len} bytes from WebRTC`);
        if (client && !client.closed) {
          client.push(data);
        }
      };
      await drive.connect();

      const dc = drive.dc;
      // drive.connect() resolves after signaling, not after DC open
      for (let i = 0; i < 120 && dc.readyState !== 'open'; i++) {
        await new Promise((r) => setTimeout(r, 100));
      }
      if (dc.readyState !== 'open') throw new Error('P2P channel not open');
      flushQueue();

      channelReady = true;
      statusMsg = "Channel ready";

      if (user && pass) {
        credsUser = user;
        credsPass = pass;
        await authenticate(user, pass);
      } else {
        // Quick-login: try credentials remembered in the SSH vault before
        // falling back to the manual prompt. authenticate() reports failures
        // itself (re-opens the prompt), so a rejection here is not fatal.
        const saved = targetLinkId ? getSshCredential(targetLinkId) : null;
        if (saved && saved.user) {
          credsUser = saved.user;
          credsPass = saved.pass || "";
        }
        if (saved && saved.user && saved.pass) {
          console.log("[Terminal] quick-login with saved SSH credentials");
          try {
            await authenticate(saved.user, saved.pass);
            return;
          } catch (e) {
            console.warn("[Terminal] quick-login failed, showing prompt:", e);
          }
        } else {
          showPrompt = true;
        }
      }
    } catch (err) {
      errorMsg = "Connection failed: " + (err.message || err);
      term.write('\r\n\x1b[31m' + errorMsg + '\x1b[0m\r\n');
    }
  }

  // True once shell output looks like a prompt (default bash/sh/zsh/fish and
  // common prompt glyphs). Best-effort: the caller caps the wait anyway.
  function shellPromptSeen() {
    const t = outTail.replace(/\s+$/, "");
    return shellOutputSeen && /[$#%❯»›>]$/.test(t);
  }

  async function authenticate(u, p) {
    if (!channelReady) return;
    showPrompt = false;
    promptError = "";
    statusMsg = "Authenticating...";
    try {
      await client.login(u, p);
      isConnected = true;
      fitAddon.fit();
      shellOutputSeen = false;
      lastShellOutputAt = 0;
      outTail = "";
      await client.openShell();
      // Resize only after the channel exists (pty starts at 80x24 default).
      client.resize(term.cols, term.rows);
      // Keep the system greeting (motd) visible: no `clear` here anymore.
      // Per-session init (locale, metadata hooks, initial folder) is written to
      // $HOME via the node Exec op and sourced with a single short line, so the
      // long hook code doesn't spray over the greeting. If the FS drive is
      // unreachable, fall back to typing the same lines inline (no `clear`).
      const tag = initTag ? initTag.replace(/[^A-Za-z0-9_-]/g, "") : "";
      const scriptName = ".omd_init_" + (tag || "term") + ".sh";
      // Resolve the initial folder to the real on-disk path: the logical path
      // ("/Knowledge/data") is relative to the device's share tree, and only
      // the node knows where each share is actually mounted. If no path was
      // passed, the login shell simply starts in the user home.
      let cdLine = "";
      if (initialPath && initialPath !== "/") {
        const rel = String(initialPath).replace(/^\//, "");
        let realPath = "";
        if (fsDrive && fsDrive.isConnected) {
          try {
            // Cap the P2P round-trip: a stalled DataChannel must not delay
            // session setup (the $HOME-relative fallback is fine).
            realPath = await Promise.race([
              fsDrive.resolvePath("/" + rel),
              new Promise((_, rej) => setTimeout(() => rej(new Error("resolvePath timeout")), 5000))
            ]);
          } catch (err) {
            console.warn("[Terminal] resolvePath failed, using $HOME-relative:", err);
          }
        }
        if (realPath) {
          cdLine = 'cd "' + dqEscape(realPath) + '"';
          debugCwd = realPath;
        } else {
          cdLine = 'cd "$HOME/' + dqEscape(rel) + '"';
          debugCwd = "~/" + rel;
        }
        console.log("[Terminal] initial folder:", debugCwd);
      } else {
        debugCwd = "~";
      }
      const initLines = [];
      initLines.push("export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8");
      initLines.push(SHELL_HOOK_BASH);
      initLines.push(SHELL_HOOK_ZSH);
      if (cdLine) initLines.push(cdLine);
      // Self-cleanup: the script removes itself after sourcing.
      initLines.push('rm -f "$HOME/' + scriptName + '"');
      let initDone = false;
      initLoading = true;
      // Flush the DOM so the banner is painted before the blocking P2P round-trip.
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      if (fsDrive && fsDrive.isConnected) {
        try {
          // Hide the `source ...` call itself: the script's first output moves
          // the cursor up onto the echoed command line and erases it, so only
          // the fresh prompt remains on screen. (Used only on this path — when
          // the lines are typed inline below there is nothing to hide.)
          const scriptLines = ["printf '\\033[1A\\r\\033[2K'"].concat(initLines);
          const payload = btoa(scriptLines.join("\n"));
          // Cap the write: if FS-RPC stalls (P2P wakeup, half-dead channel),
          // fall back to typing the hooks inline instead of hanging forever.
          await Promise.race([
            fsDrive.exec(
              "",
              'printf %s "' + payload + '" | base64 -d > "$HOME/' + scriptName + '"'
            ),
            new Promise((_, rej) => setTimeout(() => rej(new Error("exec timeout")), 8000))
          ]);
          initDone = true;
        } catch (err) {
          console.warn("[Terminal] init script write failed, typing hooks inline:", err);
        }
      }
      // Type into the shell only after its first prompt: bytes sent earlier
      // are kernel-echoed before readline initializes, and that early echo
      // line survives the in-script line erase (seen on the real device: a
      // bare ` source ...` line above the prompt). Ready = prompt-looking
      // output + a short quiet gap; 5s cap, then send anyway (best effort).
      {
        const deadline = Date.now() + 5000;
        while (Date.now() < deadline) {
          if (shellPromptSeen() && Date.now() - lastShellOutputAt > 300) break;
          await new Promise((r) => setTimeout(r, 40));
        }
      }
      if (initDone) {
        // Leading space: keeps the line out of bash history when
        // HISTCONTROL=ignorespace/ignoreboth is set (Ubuntu default).
        client.write(' source "$HOME/' + scriptName + '"\r');
      } else {
        for (const line of initLines) client.write(line + "\r");
      }
      initLoading = false;
      console.log("[Terminal] cwd =", debugCwd);
      statusMsg = "";
      term.focus();
    } catch (err) {
      promptError = String(err.message || err);
      showPrompt = true;
      initLoading = false;
      statusMsg = "Channel ready";
    }
  }

  async function startWithCreds() {
    const u = credsUser.trim();
    if (!u || !credsPass) return;
    try {
      await authenticate(u, credsPass);
      // Success — remember for quick-login unless the user opted out.
      if (remember && targetLinkId) {
        saveSshCredential(targetLinkId, u, credsPass).catch((e) =>
          console.warn("[Terminal] failed to remember SSH credentials:", e)
        );
      }
    } catch (e) {
      // authenticate() handles its own error UI; nothing to add here.
    }
  }

  function handleClose() {
    alive = false;
    if (typeof document !== "undefined") {
      try {
        document.title = (prevDocTitle && prevDocTitle !== "~" && !prevDocTitle.startsWith("~")) ? prevDocTitle : getDomainBrandTitle();
      } catch (e) {}
    }
    if (onClose) onClose();
  }

  onDestroy(() => {
    alive = false;
    if (resizeObserver) {
      try { resizeObserver.disconnect(); } catch (e) {}
    }
    if (canvasAddon) {
      try { canvasAddon.dispose(); } catch (e) {}
    }
    if (fitAddon) {
      try { fitAddon.dispose(); } catch (e) {}
    }
    if (term) {
      try { term.dispose(); } catch (e) {}
    }
    if (client) {
      try { client.close(); } catch (e) {}
    }
    if (drive) {
      try { drive.close(); } catch (e) {}
    }
    if (typeof document !== "undefined") {
      try {
        document.title = (prevDocTitle && prevDocTitle !== "~" && !prevDocTitle.startsWith("~")) ? prevDocTitle : getDomainBrandTitle();
      } catch (e) {}
    }
  });
</script>

<div class="terminal-app">
  {#if showCloseButton}
    <div class="terminal-toolbar">
      <div class="title">{tabTitle}</div>
      <button class="close-btn" on:click={handleClose}>
        <svg viewBox="0 0 24 24" width="24" height="24" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round">
          <line x1="18" y1="6" x2="6" y2="18"></line>
          <line x1="6" y1="6" x2="18" y2="18"></line>
        </svg>
      </button>
    </div>
  {/if}
  <div class="terminal-wrapper" bind:this={terminalContainer}></div>
  {#if initLoading}
    <div class="init-banner">
      <span class="init-dots"><i></i><i></i><i></i></span> Setting up session
    </div>
  {/if}
  {#if showPrompt}
    <div class="error-overlay">
      <form class="prompt-box" on:submit|preventDefault={startWithCreds}>
        <div class="prompt-title">SSH Login</div>
        <div class="prompt-hint">Device: {deviceName}</div>
        {#if promptError}
          <div class="prompt-error">{promptError}</div>
        {/if}
        <label class="prompt-label" for="ssh-username">User</label>
        <input id="ssh-username" class="prompt-input" type="text" bind:value={credsUser} placeholder="username" autocomplete="username" />
        <label class="prompt-label" for="ssh-password">Password</label>
        <input id="ssh-password" class="prompt-input" type="password" bind:value={credsPass} placeholder="password" autocomplete="current-password" />
        <label class="prompt-check">
          <input type="checkbox" bind:checked={remember} />
          Remember on this device
        </label>
        <button class="prompt-btn" type="submit">Connect</button>
      </form>
    </div>
  {:else if !channelReady && !errorMsg}
    <div class="loading-overlay">
      <div class="loading-box">
        <div class="spinner"></div>
        <div>Connecting to Terminal{statusMsg ? ' (' + statusMsg + ')' : ''}...</div>
      </div>
    </div>
  {/if}
  {#if errorMsg}
    <div class="error-overlay">
      <div class="prompt-box">
        <div class="prompt-title" style="color: #ff6b6b">Connection Error</div>
        <div style="color: #ccc; font-size: 13px; word-break: break-word;">{errorMsg}</div>
        <button class="prompt-btn" style="margin-top: 12px;" on:click={startSession}>Retry</button>
      </div>
    </div>
  {/if}
</div>

<style>
  .terminal-app {
    position: relative;
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background-color: var(--wx-background, #1F2430);
  }

  .terminal-wrapper {
    display: flex;
    flex-direction: column;
    height: 100%;
    width: 100%;
    background-color: var(--wx-background, #1F2430);
  }

  :global(.xterm),
  :global(.xterm .xterm-viewport),
  :global(.xterm .xterm-screen),
  :global(.xterm .xterm-rows) {
    background-color: var(--wx-background, #1F2430) !important;
  }

  .terminal-toolbar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 8px 16px;
    background-color: #242A38;
    color: #fff;
    border-bottom: 1px solid #32394B;
  }

  .terminal-toolbar .title {
    font-weight: 500;
    font-size: 14px;
  }

  .close-btn {
    background: none;
    border: none;
    color: #aaa;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 4px;
    border-radius: 4px;
    transition: all 0.2s;
  }

  .close-btn:hover {
    background-color: rgba(255, 255, 255, 0.1);
    color: #fff;
  }

  .terminal-wrapper {
    flex: 1;
    width: 100%;
    overflow: hidden;
  }

  .error-overlay, .loading-overlay {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
  }

  .loading-box {
    display: flex;
    align-items: center;
    gap: 12px;
    background: #242A38;
    padding: 16px 24px;
    border-radius: 8px;
    border: 1px solid #32394B;
    color: #fff;
    font-size: 14px;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
  }

  .spinner {
    width: 18px;
    height: 18px;
    border: 2px solid rgba(255, 255, 255, 0.3);
    border-top-color: #929CD4;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .init-banner {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    z-index: 90;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    height: 26px;
    background: rgba(29, 24, 41, 0.92);
    color: #AEB5C7;
    font-size: 12px;
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
    letter-spacing: 0.02em;
    border-bottom: 1px solid rgba(139, 128, 163, 0.28);
    backdrop-filter: blur(4px);
    animation: banner-in 0.2s ease;
  }

  @keyframes banner-in {
    from { opacity: 0; transform: translateY(-100%); }
    to { opacity: 1; transform: translateY(0); }
  }

  .init-dots {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    height: 1em;
  }
  .init-dots i {
    display: block;
    width: 4px;
    height: 4px;
    border-radius: 50%;
    background: #929CD4;
    animation: dot-blink 1.4s infinite;
    font-style: normal;
  }
  .init-dots i:nth-child(2) { animation-delay: 0.2s; }
  .init-dots i:nth-child(3) { animation-delay: 0.4s; }

  @keyframes dot-blink {
    0%, 20% { opacity: 0.2; }
    50% { opacity: 1; }
    80%, 100% { opacity: 0.2; }
  }

  /* Исправление поведения сетки внутри контейнера */
  :global(.xterm) {
    height: 100%;
  }

  /* Нативный скроллбар xterm всегда виден из-за overflow-y: scroll,
     даже когда терминал пуст (ползунок растягивается на всю высоту).
     Показываем его только при переполнении и стилизуем под тему. */
  :global(.xterm .xterm-viewport) {
    overflow-y: auto;
    scrollbar-width: thin;
    scrollbar-color: rgba(255, 255, 255, 0.25) transparent;
  }

  :global(.xterm .xterm-viewport::-webkit-scrollbar) {
    width: 8px;
  }

  :global(.xterm .xterm-viewport::-webkit-scrollbar-track) {
    background: transparent;
  }

  :global(.xterm .xterm-viewport::-webkit-scrollbar-thumb) {
    background: rgba(255, 255, 255, 0.25);
    border-radius: 4px;
  }

  :global(.xterm .xterm-viewport::-webkit-scrollbar-thumb:hover) {
    background: rgba(255, 255, 255, 0.4);
  }

  .prompt-box {
    display: flex;
    flex-direction: column;
    gap: 8px;
    background: var(--wx-background-alt, #191D28);
    border: 1px solid var(--border-color, #32394B);
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.6);
    border-radius: 12px;
    padding: 24px;
    width: 340px;
    max-width: 90vw;
  }

  .prompt-title {
    font-weight: 600;
    font-size: 16px;
    margin-bottom: 2px;
    color: var(--wx-color-font, #fff);
  }

  .prompt-label {
    font-size: 12px;
    color: var(--wx-color-font-alt, #7E869B);
    margin-top: 4px;
  }

  .prompt-input {
    background: var(--wx-background, #1F2430);
    border: 1px solid var(--border-color, #32394B);
    border-radius: 6px;
    color: var(--wx-color-font, #fff);
    padding: 9px 12px;
    font-size: 14px;
    outline: none;
    transition: border-color 0.2s;
  }

  .prompt-input:focus {
    border-color: var(--wx-color-primary, #5863A6);
  }

  .prompt-hint {
    font-size: 12px;
    color: var(--wx-color-font-alt, #7E869B);
    margin-bottom: 4px;
  }

  .prompt-check {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    color: var(--wx-color-font-alt, #7E869B);
    margin-top: 10px;
    cursor: pointer;
    user-select: none;
  }

  .prompt-check input {
    accent-color: var(--wx-color-primary, #5863A6);
  }

  .prompt-error {
    font-size: 12px;
    color: #ff6b6b;
    background: rgba(255, 107, 107, 0.1);
    border: 1px solid rgba(255, 107, 107, 0.2);
    padding: 8px;
    border-radius: 6px;
    margin-bottom: 4px;
  }

  .prompt-btn {
    margin-top: 12px;
    background: var(--wx-color-primary, #5863A6);
    border: none;
    border-radius: 6px;
    color: #fff;
    font-size: 14px;
    font-weight: 500;
    padding: 10px 16px;
    cursor: pointer;
    transition: background-color 0.2s;
  }

  .prompt-btn:hover {
    background: var(--wx-color-primary-hover, #7B86C4);
  }
</style>
