// Hash routing between the UI and the page. The page owns location.hash; the
// UI asks for changes with Host.postMessage('route', path) and hears about
// back / forward / typed URLs through the 'route' host message. Paths look
// like "/screens/hud".
import { createContext, useCallback, useContext, useEffect, useState } from 'react';
import { Host, useHostMessage } from 'react-native';

const RouterContext = createContext({ path: '/', go: () => {} });

export function normalize(path) {
  const p = String(path || '/').replace(/^#/, '').trim();
  if (!p || p === '/') return '/';
  return ('/' + p.replace(/^\/+/, '')).replace(/\/+$/, '');
}

export function useRouterState() {
  const [path, setPath] = useState('/');
  useHostMessage('route', (p) => setPath(normalize(p)));
  // Ask the page for the current route once mounted (messages sent before
  // the listener existed would be lost).
  useEffect(() => Host.postMessage('ready', {}), []);
  const go = useCallback((p) => {
    const next = normalize(p);
    setPath(next);
    Host.postMessage('route', next);
  }, []);
  return { path, go };
}

export const RouterProvider = RouterContext.Provider;
export const useRouter = () => useContext(RouterContext);

/** Splits "/screens/hud" into ["screens", "hud"]. */
export const segments = (path) => normalize(path).split('/').filter(Boolean);
