<script>
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';
  import * as monaco from 'monaco-editor';
  import { getLanguageFromFilename } from '../lib/lang.js';

  export let value = '';
  export let language = 'javascript';
  export let filename = '';
  export let showHeader = true;

  const dispatch = createEventDispatcher();

  let container;
  let editor;

  $: currentLanguage = filename ? getLanguageFromFilename(filename) : language;

  onMount(() => {
    monaco.editor.defineTheme('figma-dark', {
      base: 'vs-dark',
      inherit: true,
      rules: [],
      colors: {
        'editor.background': '#252330',
        'editor.lineHighlightBackground': '#312E3E',
        'editorLineNumber.foreground': '#5B5270',
        'editorLineNumber.activeForeground': '#C4B8DB',
        'editor.selectionBackground': '#581C8788'
      }
    });

    editor = monaco.editor.create(container, {
      value,
      language: currentLanguage,
      theme: 'figma-dark',
      fontFamily: '"Hack", "Hack Nerd Font", "JetBrains Mono", "Fira Code", Consolas, monospace',
      automaticLayout: true,
      readOnly: false, // Enable editing
      minimap: { enabled: true }
    });

    editor.onDidChangeModelContent(() => {
      value = editor.getValue();
      dispatch('change', value);
    });
  });

  // Update editor if value prop changes (e.g., streaming in over WebRTC)
  $: if (editor && value != null && String(value) !== editor.getValue()) {
    editor.setValue(String(value));
  }

  // Update language if it changes
  $: if (editor && currentLanguage) {
    const model = editor.getModel();
    if (model) {
      monaco.editor.setModelLanguage(model, currentLanguage);
    }
  }

  onDestroy(() => {
    if (editor) {
      editor.dispose();
    }
  });
</script>

<div class="editor-wrapper">
  {#if showHeader}
  <div class="editor-header">
    <span class="filename">{filename}</span>
    <button class="close-btn" on:click={() => dispatch('close')} title="Close">
      <svg viewBox="0 0 24 24" width="24" height="24" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round">
        <line x1="18" y1="6" x2="6" y2="18"></line>
        <line x1="6" y1="6" x2="18" y2="18"></line>
      </svg>
    </button>
  </div>
  {/if}
  <div bind:this={container} class="editor-container"></div>
</div>

<style>
  .editor-wrapper {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background-color: var(--wx-background, #252330);
  }

  .editor-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    background-color: var(--wx-background-alt, #1F1D28);
    padding: 8px 16px;
    border-bottom: 1px solid var(--border-color, #3E3A4D);
  }

  .filename {
    color: #fff;
    font-size: 14px;
    font-family: monospace;
    font-weight: 500;
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

  .editor-container {
    flex: 1;
    min-height: 0; /* Important for flex child to not overflow */
    width: 100%;
    background-color: var(--wx-background, #252330);
  }
</style>
