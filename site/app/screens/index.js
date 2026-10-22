// The interactive game screens shown on /screens. Each one is designed at
// 1280×720 and takes `{ active }`: false while it is only a thumbnail or
// off-screen, so looping animations can pause.
import MainMenu from './main-menu.jsx';
import Hud from './hud.jsx';
import Inventory from './inventory.jsx';
import BattlePass from './battle-pass.jsx';
import Scoreboard from './scoreboard.jsx';
import Dialogue from './dialogue.jsx';
import Settings from './settings.jsx';
import Loading from './loading.jsx';
import Notifications from './notifications.jsx';

export const SCREENS = [
  { id: 'main-menu', title: 'Main menu', component: MainMenu, blurb: 'Title screen with a focus-driven menu stack, news card and controller hints.' },
  { id: 'hud', title: 'Combat HUD', component: Hud, blurb: 'Health, resources, ability cooldowns, minimap, buffs and a kill feed, all live.' },
  { id: 'inventory', title: 'Inventory', component: Inventory, blurb: 'Grid inventory with rarity glows, selection, tooltips and character stats.' },
  { id: 'battle-pass', title: 'Battle pass', component: BattlePass, blurb: 'Scrolling reward track with tiers, claim states and progress.' },
  { id: 'scoreboard', title: 'Scoreboard', component: Scoreboard, blurb: 'Team scoreboard with live score changes and per-player stats.' },
  { id: 'dialogue', title: 'Dialogue', component: Dialogue, blurb: 'Branching conversation with typewriter text and choice selection.' },
  { id: 'settings', title: 'Settings', component: Settings, blurb: 'Tabs, sliders, toggles and selectors that actually work with any input.' },
  { id: 'loading', title: 'Loading', component: Loading, blurb: 'Loading screen with progress, tips and ambient motion.' },
  { id: 'notifications', title: 'Notifications', component: Notifications, blurb: 'Toasts, achievements and party invites that stack and animate.' },
];

export const screenById = (id) => SCREENS.find((s) => s.id === id);
