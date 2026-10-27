// The site's sections, for the nav bar, its menu and the Features page's tabs.

// The pages under /features, each a live demo of part of the runtime.
export const FEATURE_PAGES = [
  { to: '/features/interactions', label: 'Interactions' },
  { to: '/features/input', label: 'Input' },
  { to: '/features/motion', label: 'Motion' },
  { to: '/features/vector', label: 'SVG & Skia' },
  { to: '/features/native', label: 'Native components' },
];

export const LINKS = [
  { to: '/use-cases', label: 'Use cases' },
  { to: '/examples', label: 'Examples' },
  { to: '/features', label: 'Features', children: FEATURE_PAGES },
  { to: '/playground', label: 'Playground' },
  { to: '/embed', label: 'Embed' },
];
