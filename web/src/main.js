import './styles/omd.css';
import { getDomainBrandTitle } from './lib/app-mode.js';

// Clean any unparsed server template macros from DOM (e.g. Capacitor/static client loads)
if (typeof document !== 'undefined') {
  try {
    const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
    const nodesToRemove = [];
    let node;
    while ((node = walker.nextNode())) {
      if (node.nodeValue && (node.nodeValue.includes('{if ') || node.nodeValue.includes('{end '))) {
        nodesToRemove.push(node);
      }
    }
    for (const n of nodesToRemove) {
      if (n.parentNode) n.parentNode.removeChild(n);
    }
    if (!document.title || document.title.includes('{TITLE}') || document.title.includes('{APPTITLE}')) {
      document.title = getDomainBrandTitle();
    }
  } catch (_) {}
}

import { mount } from 'svelte';
import App from './App.svelte';
import { mountMobileGoogleBridgeIfRequested } from './lib/identity/googleMobileAuth.js';

const appTarget = document.getElementById('app');
const app = mountMobileGoogleBridgeIfRequested(appTarget)
  ? null
  : mount(App, {
      target: appTarget,
    });

export default app;