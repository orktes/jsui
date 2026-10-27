// Hash routing between the UI and the page. The page owns location.hash; the
// UI asks for changes with Host.postMessage('route', path) and hears about
// back / forward / typed URLs through the 'route' host message. Paths look
// like "/examples/hud". Links from before the site had sections (/screens,
// /vector, ...) still work: they are rewritten to their new path, and the
// page's URL is replaced (not pushed), so Back does not bounce on them.
import { createContext, startTransition, useCallback, useContext, useEffect, useState } from 'react';
import { Host, useHostMessage } from 'react-native';

const RouterContext = createContext({ path: '/', go: () => {} });

export function normalize(path) {
  const p = String(path || '/').replace(/^#/, '').trim();
  if (!p || p === '/') return '/';
  return ('/' + p.replace(/^\/+/, '')).replace(/\/+$/, '');
}

// Old top-level pages and where they live now.
const MOVED = { screens: 'examples', interactions: 'features/interactions', input: 'features/input', motion: 'features/motion', vector: 'features/vector' };

/** The current path for `path`, following pages that moved. */
export function canonical(path) {
  const p = normalize(path);
  const [head, ...rest] = p.split('/').filter(Boolean);
  return MOVED[head] ? normalize([MOVED[head], ...rest].join('/')) : p;
}

export function useRouterState() {
  const [path, setPath] = useState('/');
  // Page changes are transitions: React renders the new page in time slices
  // across frames while the current one stays live, then commits it in one.
  useHostMessage('route', (p) => {
    const next = canonical(p);
    if (next !== normalize(p)) Host.postMessage('route:replace', next);
    startTransition(() => setPath(next));
  });
  // Ask the page for the current route once mounted (messages sent before
  // the listener existed would be lost).
  useEffect(() => Host.postMessage('ready', {}), []);
  const go = useCallback((p) => {
    const next = canonical(p);
    startTransition(() => setPath(next));
    Host.postMessage('route', next);
  }, []);
  return { path, go };
}

export const RouterProvider = RouterContext.Provider;
export const useRouter = () => useContext(RouterContext);

// The page's ScrollView: { to(y) } scrolls it to y in the page's content.
const PageScrollContext = createContext({ to: () => {} });
export const PageScrollProvider = PageScrollContext.Provider;
export const usePageScroll = () => useContext(PageScrollContext);

/** Splits "/examples/hud" into ["examples", "hud"]. */
export const segments = (path) => normalize(path).split('/').filter(Boolean);
