// The interactive game screens shown on /screens. Each one is designed at
// 1280×720 and takes `{ active }`: false while it is only a thumbnail or
// off-screen, so looping animations can pause.
//
// Each module exports its component as default and may export `feature`:
// { title, points: [...], keys: [...] } describing the JSUI capability it
// spotlights. Namespace imports keep a missing `feature` export harmless.
import * as MainMenu from './main-menu.jsx';
import * as Hud from './hud.jsx';
import * as Inventory from './inventory.jsx';
import * as BattlePass from './battle-pass.jsx';
import * as Scoreboard from './scoreboard.jsx';
import * as Dialogue from './dialogue.jsx';
import * as Settings from './settings.jsx';
import * as Loading from './loading.jsx';
import * as Notifications from './notifications.jsx';

const mod = (m) => ({ component: m.default, feature: m.feature || null });

export const SCREENS = [
  {
    id: 'main-menu',
    title: 'Main menu',
    ...mod(MainMenu),
    accent: '#f2c35b',
    blurb: 'Title screen with a focus-driven menu stack, news carousel and controller hints.',
  },
  {
    id: 'hud',
    title: 'Combat HUD',
    ...mod(Hud),
    accent: '#ff4d6a',
    blurb: 'Health, resources, ability cooldowns, minimap, buffs and a kill feed, all live.',
  },
  {
    id: 'inventory',
    title: 'Inventory',
    ...mod(Inventory),
    accent: '#b26bff',
    blurb: 'Grid inventory with rarity glows, selection, tooltips and character stats.',
  },
  {
    id: 'battle-pass',
    title: 'Battle pass',
    ...mod(BattlePass),
    accent: '#4fd1ff',
    blurb: 'Scrolling reward track with tiers, claim states and progress.',
  },
  {
    id: 'scoreboard',
    title: 'Scoreboard',
    ...mod(Scoreboard),
    accent: '#4ade80',
    blurb: 'Team scoreboard with live score changes and per-player stats.',
  },
  {
    id: 'dialogue',
    title: 'Dialogue',
    ...mod(Dialogue),
    accent: '#4fd1ff',
    blurb: 'Branching conversation with typewriter text, skill checks and a trust meter.',
  },
  {
    id: 'settings',
    title: 'Settings',
    ...mod(Settings),
    accent: '#f2c35b',
    blurb: 'Tabs, sliders, toggles, selectors and dropdowns that work with any input.',
  },
  {
    id: 'loading',
    title: 'Loading',
    ...mod(Loading),
    accent: '#f2c27a',
    blurb: 'Loading screen that moves through phases, rotates tips and waits for you.',
  },
  {
    id: 'notifications',
    title: 'Notifications',
    ...mod(Notifications),
    accent: '#ffb23f',
    blurb: 'Toasts, achievements, loot and party invites that stack, time out and animate.',
  },
];

export const screenById = (id) => SCREENS.find((s) => s.id === id);
