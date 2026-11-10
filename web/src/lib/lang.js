const EXT_MAP = {
  'js': 'javascript',
  'jsx': 'javascript',
  'mjs': 'javascript',
  'cjs': 'javascript',
  'ts': 'typescript',
  'tsx': 'typescript',
  'json': 'json',
  'html': 'html',
  'htm': 'html',
  'css': 'css',
  'scss': 'scss',
  'less': 'less',
  'md': 'markdown',
  'markdown': 'markdown',
  'xml': 'xml',
  'yml': 'yaml',
  'yaml': 'yaml',
  'cpp': 'cpp',
  'c': 'c',
  'h': 'cpp',
  'cc': 'cpp',
  'hpp': 'cpp',
  'py': 'python',
  'rb': 'ruby',
  'php': 'php',
  'go': 'go',
  'rs': 'rust',
  'java': 'java',
  'kt': 'kotlin',
  'sh': 'shell',
  'bash': 'shell',
  'zsh': 'shell',
  'sql': 'sql',
  'svelte': 'html',
  'vue': 'html',
  'txt': 'plaintext',
  'log': 'plaintext'
};

export function getLanguageFromFilename(name) {
  if (!name) return 'plaintext';
  const fileName = String(name).split('/').pop();
  const ext = fileName.split('.').pop().toLowerCase();
  return EXT_MAP[ext] || 'plaintext';
}
