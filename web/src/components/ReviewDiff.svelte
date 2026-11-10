<script context="module">
  import * as monaco from 'monaco-editor';

  const DIFF_TOKENIZER = {
    tokenizer: {
      root: [
        [/^diff --git.*$/, 'diff.header'],
        [/^index [0-9a-f]{7,}\.\.[0-9a-f]{7,}.*$/, 'diff.index'],
        [/^--- .*$/, 'diff.old'],
        [/^\+\+\+ .*$/, 'diff.new'],
        [/^@@[^@]*@@.*$/, 'diff.hunk'],
        [/^\\ No newline.*$/, 'diff.comment'],
        [/^new file mode.*$/, 'diff.comment'],
        [/^deleted file mode.*$/, 'diff.comment'],
        [/^similarity index.*$/, 'diff.comment'],
        [/^Binary files .* differ$/, 'diff.comment'],
        [/^\+.*$/, 'diff.add'],
        [/^-.*$/, 'diff.removed']
      ]
    }
  };

  if (!monaco.languages.getLanguages().some((l) => l.id === 'omd-diff')) {
    monaco.languages.register({ id: 'omd-diff' });
    monaco.languages.setMonarchTokensProvider('omd-diff', DIFF_TOKENIZER);
  }

  monaco.editor.defineTheme('review-dark', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'diff.header', foreground: 'C084FC' },
      { token: 'diff.index', foreground: '8B80A3' },
      { token: 'diff.old', foreground: 'e06c75' },
      { token: 'diff.new', foreground: '98c379' },
      { token: 'diff.hunk', foreground: 'D8B4FE', fontStyle: 'bold' },
      { token: 'diff.add', foreground: '98c379' },
      { token: 'diff.removed', foreground: 'e06c75' },
      { token: 'diff.comment', foreground: '8B80A3', fontStyle: 'italic' }
    ],
    colors: {
      'editor.background': '#16131F',
      'editor.foreground': '#EFEBF8',
      'editor.lineHighlightBackground': '#211C2E',
      'editorLineNumber.foreground': '#5B5270',
      'editorLineNumber.activeForeground': '#C4B8DB',
      'editorWidget.background': '#16131F',
      'editorWidget.border': '#2D2640',
      'diffEditor.insertedLineBackground': '#0f2a17',
      'diffEditor.removedLineBackground': '#2a1414',
      'diffEditor.insertedTextBackground': '#1f4a2c',
      'diffEditor.removedTextBackground': '#521c1c',
      'diffEditor.diagonalFill': '#2D2640'
    }
  });
</script>

<script>
  import { onMount, onDestroy } from 'svelte';
  import { getLanguageFromFilename } from '../lib/lang.js';

  // 'unified' renders the raw git diff text with a custom diff tokenizer.
  // 'diff-editor' renders original vs modified in Monaco's diff editor.
  export let mode = 'unified';
  export let diff = '';
  export let original = '';
  export let modified = '';
  export let filename = '';

  let container;
  let editor;
  let originalModel = null;
  let modifiedModel = null;

  onMount(() => {
    if (mode === 'diff-editor') {
      const lang = getLanguageFromFilename(filename);
      originalModel = monaco.editor.createModel(original, lang, monaco.Uri.parse('inmemory://review-original'));
      modifiedModel = monaco.editor.createModel(modified, lang, monaco.Uri.parse('inmemory://review-modified'));
      editor = monaco.editor.createDiffEditor(container, {
        theme: 'review-dark',
        readOnly: true,
        renderSideBySide: false,
        automaticLayout: true,
        minimap: { enabled: false },
        scrollBeyondLastLine: false,
        fontFamily: '"Hack", "Hack Nerd Font", "JetBrains Mono", "Fira Code", Consolas, monospace',
        fontSize: 12,
        lineNumbersMinChars: 3,
        wordWrap: 'on'
      });
      editor.setModel({ original: originalModel, modified: modifiedModel });
      editor.getOriginalEditor().updateOptions({ wordWrap: 'on' });
      editor.getModifiedEditor().updateOptions({ wordWrap: 'on' });
    } else {
      editor = monaco.editor.create(container, {
        value: diff,
        language: 'omd-diff',
        theme: 'review-dark',
        readOnly: true,
        automaticLayout: true,
        minimap: { enabled: false },
        scrollBeyondLastLine: false,
        fontFamily: '"Hack", "Hack Nerd Font", "JetBrains Mono", "Fira Code", Consolas, monospace',
        fontSize: 12,
        lineNumbersMinChars: 3,
        wordWrap: 'on'
      });
    }
  });

  $: if (editor) {
    if (mode === 'diff-editor') {
      const lang = getLanguageFromFilename(filename);
      if (originalModel && originalModel.getValue() !== original) originalModel.setValue(original);
      if (modifiedModel && modifiedModel.getValue() !== modified) modifiedModel.setValue(modified);
      if (originalModel) monaco.editor.setModelLanguage(originalModel, lang);
      if (modifiedModel) monaco.editor.setModelLanguage(modifiedModel, lang);
    } else if (editor.getValue && editor.getValue() !== diff) {
      editor.setValue(diff);
    }
  }

  onDestroy(() => {
    if (editor) editor.dispose();
    if (originalModel) originalModel.dispose();
    if (modifiedModel) modifiedModel.dispose();
  });
</script>

<div class="review-diff" bind:this={container}></div>

<style>
  .review-diff {
    width: 100%;
    height: 100%;
    min-height: 0;
    background-color: var(--wx-background, #16131F);
  }
</style>
