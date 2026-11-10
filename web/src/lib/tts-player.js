import { writable } from "svelte/store";

export const ttsState = writable({
  activeId: null,
  state: "idle", // 'idle' | 'loading' | 'playing' | 'error'
  voiceStreamEnabled: false,
  error: null
});

class TTSPlayer {
  constructor() {
    this.currentAudio = null;
    this.activeId = null;
    this.abortController = null;

    // Streaming queue properties
    this.isStreaming = false;
    this.streamBuffer = "";
    this.sentenceQueue = [];
    this.audioQueue = [];
    this.isPlayingQueue = false;
    this.httpDrive = null;
    this.selectedVoice = null;
    this.speed = null;
  }

  setVoiceStreamEnabled(enabled) {
    ttsState.update(s => ({ ...s, voiceStreamEnabled: !!enabled }));
  }

  stop() {
    if (this.abortController) {
      this.abortController.abort();
      this.abortController = null;
    }

    if (this.currentAudio) {
      this.currentAudio.pause();
      this.currentAudio.removeAttribute("src");
      this.currentAudio.load();
      this.currentAudio = null;
    }

    this.activeId = null;
    this.isStreaming = false;
    this.streamBuffer = "";
    this.sentenceQueue = [];
    this.audioQueue = [];
    this.isPlayingQueue = false;

    ttsState.update(s => ({ ...s, activeId: null, state: "idle", error: null }));
  }

  /**
   * On-demand playback of a single message
   */
  async playMessage(messageId, text, httpDrive, options = {}) {
    if (!text || !text.trim()) return;

    // Toggle off if already playing this message
    if (this.activeId === messageId) {
      this.stop();
      return;
    }

    this.stop();
    this.activeId = messageId;
    this.httpDrive = httpDrive;
    this.abortController = new AbortController();

    ttsState.update(s => ({ ...s, activeId: messageId, state: "loading", error: null }));

    try {
      const voice = options.voice || this.selectedVoice || undefined;
      const speed = options.speed || this.speed || undefined;

      const fetchFn = (this.httpDrive && typeof this.httpDrive.fetch === "function")
        ? this.httpDrive.fetch.bind(this.httpDrive)
        : fetch;

      const payload = { text };
      if (voice) payload.voice = voice;
      if (speed) payload.speed = speed;

      const res = await fetchFn("/api/tts", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
        signal: this.abortController.signal
      });

      if (!res.ok) {
        throw new Error(`TTS request failed with status ${res.status}`);
      }

      const blob = await res.blob();
      const audioUrl = URL.createObjectURL(blob);

      const audio = new Audio(audioUrl);
      this.currentAudio = audio;

      audio.onplay = () => {
        if (this.activeId === messageId) {
          ttsState.update(s => ({ ...s, state: "playing" }));
        }
      };

      audio.onended = () => {
        URL.revokeObjectURL(audioUrl);
        if (this.activeId === messageId) {
          this.stop();
        }
      };

      audio.onerror = (e) => {
        URL.revokeObjectURL(audioUrl);
        if (this.activeId === messageId) {
          ttsState.update(s => ({ ...s, state: "error", error: "Audio playback error" }));
        }
      };

      await audio.play();
    } catch (err) {
      if (err.name === "AbortError") return;
      console.warn("[TTS] play error:", err);
      ttsState.update(s => ({ ...s, state: "error", error: err.message }));
      this.stop();
    }
  }

  /**
   * Starts a streaming TTS session for a live LLM response
   */
  startStreamingSession(messageId, httpDrive, options = {}) {
    this.stop();
    this.isStreaming = true;
    this.activeId = messageId;
    this.httpDrive = httpDrive;
    this.streamBuffer = "";
    this.sentenceQueue = [];
    this.audioQueue = [];
    this.isPlayingQueue = false;
    this.abortController = new AbortController();
    this.selectedVoice = options.voice || this.selectedVoice || null;
    this.speed = options.speed || this.speed || null;

    ttsState.update(s => ({ ...s, activeId: messageId, state: "loading", error: null }));
  }

  /**
   * Called as tokens arrive from the SSE stream
   */
  enqueueStreamDelta(delta) {
    if (!this.isStreaming || !delta) return;

    this.streamBuffer += delta;

    // Check for sentence boundaries (. ! ? or double newline)
    const match = this.streamBuffer.match(/([^\.!\?\n]+[\.!\?]+(?:\s+|\n+)|[^\n]+\n\n+)/);
    if (match) {
      const sentence = match[0].trim();
      this.streamBuffer = this.streamBuffer.slice(match.index + match[0].length);

      if (sentence.length > 2) {
        this._fetchAndQueueSentence(sentence);
      }
    }
  }

  /**
   * Called when SSE stream is done
   */
  endStreamingSession() {
    if (!this.isStreaming) return;

    const remaining = this.streamBuffer.trim();
    this.streamBuffer = "";
    if (remaining.length > 1) {
      this._fetchAndQueueSentence(remaining);
    }
    this.isStreaming = false;
  }

  async _fetchAndQueueSentence(sentence) {
    const queueItem = {
      text: sentence,
      blobPromise: null
    };
    this.sentenceQueue.push(queueItem);

    const fetchFn = (this.httpDrive && typeof this.httpDrive.fetch === "function")
      ? this.httpDrive.fetch.bind(this.httpDrive)
      : fetch;

    const payload = { text: sentence };
    if (this.selectedVoice) payload.voice = this.selectedVoice;
    if (this.speed) payload.speed = this.speed;

    queueItem.blobPromise = fetchFn("/api/tts", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: this.abortController ? this.abortController.signal : undefined
    })
      .then(res => (res.ok ? res.blob() : null))
      .catch(err => {
        if (err.name !== "AbortError") console.warn("[TTS] stream sentence error:", err);
        return null;
      });

    this._playNextInQueue();
  }

  async _playNextInQueue() {
    if (this.isPlayingQueue || this.sentenceQueue.length === 0) return;

    this.isPlayingQueue = true;
    const item = this.sentenceQueue.shift();

    try {
      const blob = await item.blobPromise;
      if (!blob || !this.activeId) {
        this.isPlayingQueue = false;
        if (this.sentenceQueue.length > 0) this._playNextInQueue();
        else if (!this.isStreaming) this.stop();
        return;
      }

      const audioUrl = URL.createObjectURL(blob);
      const audio = new Audio(audioUrl);
      this.currentAudio = audio;

      audio.onplay = () => {
        ttsState.update(s => ({ ...s, state: "playing" }));
      };

      audio.onended = () => {
        URL.revokeObjectURL(audioUrl);
        this.isPlayingQueue = false;
        if (this.sentenceQueue.length > 0) {
          this._playNextInQueue();
        } else if (!this.isStreaming) {
          this.stop();
        }
      };

      audio.onerror = () => {
        URL.revokeObjectURL(audioUrl);
        this.isPlayingQueue = false;
        if (this.sentenceQueue.length > 0) {
          this._playNextInQueue();
        } else if (!this.isStreaming) {
          this.stop();
        }
      };

      await audio.play();
    } catch (e) {
      this.isPlayingQueue = false;
      if (this.sentenceQueue.length > 0) this._playNextInQueue();
      else if (!this.isStreaming) this.stop();
    }
  }
}

export const ttsPlayer = new TTSPlayer();
