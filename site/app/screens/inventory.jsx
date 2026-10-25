// Interactive inventory (1280×720): the examples/screens/inventory.jsx
// reference with a live grid. Selection follows hover and focus (arrows /
// D-pad), the details card updates with the selected item, A / Enter / click
// equips (the icon flies to the paper doll and the gear score ticks) or uses
// a consumable, Y salvages, X compares with what is equipped, LB / RB (Q / E)
// switch categories with an animated reflow, LT / RT (1-4) switch tabs.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Image, LayoutAnimation, Pressable, StyleSheet, Text, View, Visibility, useGroupState, useInteraction } from 'react-native';
import { colors, font, rarity } from '../../../examples/theme.js';

export const feature = {
  title: 'Grid focus + LayoutAnimation',
  points: [
    'Arrows / D-pad walk the 6×4 grid and the paper doll with spatial focus; hover selects too, and the details card follows the selection.',
    'Category and tab switches reflow the grid with LayoutAnimation (items glide to their new cells, removed ones fade).',
    'Equipping flies the item to its doll slot, swaps the old piece into the cell and ticks the gear score; salvaging shrinks the item away.',
    'Sliding highlights on the tabs and filters are native springs.',
  ],
  keys: ['Arrows / D-pad: move', 'Enter / A / click: equip or use', 'X: compare with equipped', 'Y: salvage', 'Q E / LB RB: categories', '1-4 / LT RT: tabs'],
};

const ND = true;

const icon = {
  sword: require('../../../examples/assets/item-sword.png'),
  shield: require('../../../examples/assets/item-shield.png'),
  hp: require('../../../examples/assets/item-potion-health.png'),
  mp: require('../../../examples/assets/item-potion-mana.png'),
  gem: require('../../../examples/assets/item-gem.png'),
  bow: require('../../../examples/assets/item-bow.png'),
  helmet: require('../../../examples/assets/item-helmet.png'),
  ring: require('../../../examples/assets/item-ring.png'),
  scroll: require('../../../examples/assets/item-scroll.png'),
  boots: require('../../../examples/assets/item-boots.png'),
  amulet: require('../../../examples/assets/item-amulet.png'),
  axe: require('../../../examples/assets/item-axe.png'),
};

// kind -> category, equip slot, and the label of its headline number.
const KIND = {
  axe: { cat: 'Weapons', slot: 'main', main: 'Damage per second' },
  bow: { cat: 'Weapons', slot: 'main', main: 'Damage per second' },
  sword: { cat: 'Weapons', slot: 'main', main: 'Damage per second' },
  shield: { cat: 'Armor', slot: 'off', main: 'Armor' },
  helmet: { cat: 'Armor', slot: 'head', main: 'Armor' },
  boots: { cat: 'Armor', slot: 'feet', main: 'Armor' },
  ring: { cat: 'Armor', slot: 'ring', main: 'Item power' },
  amulet: { cat: 'Armor', slot: 'neck', main: 'Item power' },
  hp: { cat: 'Consumables', main: 'Health restored' },
  mp: { cat: 'Consumables', main: 'Mana restored' },
  scroll: { cat: 'Consumables', main: 'Charges' },
  gem: { cat: 'Materials', main: 'In stack' },
};

const G = colors.green;
const O = colors.orange;
// [id, kind, tier, count, name, type, main, stats, unique, flavor, req, price, weight]
const DB = {};
function def(id, kind, tier, name, typeName, main, stats, extra) {
  DB[id] = Object.assign({ id, kind, tier, name, typeName, main, stats }, extra);
}
def('cinderbrand', 'axe', 'legendary', 'Cinderbrand, Oath of Ash', 'Legendary Two-Handed Axe', 842, [
  ['Physical damage', '612 – 790'],
  ['Attack speed', '1.20', null, '-0.15'],
  ['Strength', '+48', G, '+22'],
  ['Critical chance', '+6.5%', G, '+2.0%'],
  ['Fire damage', '+18%', O],
], {
  unique: ['Unique: Every third strike erupts in cinders, dealing ', '240%', ' weapon damage to nearby foes.'],
  flavor: '"Forged in the last breath of the Old King, it remembers every oath ever broken over its edge."',
  req: 40, price: 3750, weight: 9.5,
});
def('whisperwind', 'bow', 'epic', 'Whisperwind Recurve', 'Epic Longbow', 768, [
  ['Physical damage', '540 – 702'],
  ['Attack speed', '1.45', null, '+0.10'],
  ['Dexterity', '+36', G, '+14'],
  ['Critical chance', '+4.0%', G, '-0.5%'],
], { unique: ['Arrows loosed while moving pierce ', '2', ' additional targets.'], flavor: '"Strung with a single hair from Saelith, the wind that never lands."', req: 38, price: 2180, weight: 4 });
def('draught', 'hp', 'common', 'Minor Healing Draught', 'Consumable · Potion', 450, [['Duration', '6 s'], ['Shared cooldown', '30 s']], { flavor: '"Tastes faintly of copper and cherries."', req: 1, price: 25, weight: 0.5 });
def('tonic', 'mp', 'common', 'Minor Mana Tonic', 'Consumable · Potion', 300, [['Duration', '6 s'], ['Shared cooldown', '30 s']], { flavor: '"Fizzes when shaken. Do not shake."', req: 1, price: 25, weight: 0.5 });
def('voidshard', 'gem', 'epic', 'Voidheart Shard', 'Material · Gem', 3, [['Socket bonus', '+12% spell power', colors.purple], ['Salvages into', '4 Void dust']], { flavor: '"It hums at a pitch only the dead can hear."', req: 30, price: 640, weight: 0.2 });
def('recall', 'scroll', 'uncommon', 'Scroll of Recall', 'Consumable · Scroll', 2, [['Cast time', '5 s'], ['Destination', 'Last shrine']], { flavor: '"Read it slowly. Arriving in pieces is frowned upon."', req: 10, price: 80, weight: 0.1 });
def('tideband', 'ring', 'rare', 'Band of the Tidecaller', 'Rare Ring', 118, [['Intellect', '+28', G, '+9'], ['Mana regeneration', '+6 / s', G, '+2 / s'], ['Frost resistance', '+12%', colors.cyan]], { flavor: '"Cold to the touch, even in the forge."', req: 32, price: 940, weight: 0.1 });
def('stormblade', 'sword', 'rare', 'Stormforged Blade', 'Rare One-Handed Sword', 702, [['Physical damage', '410 – 560'], ['Attack speed', '1.60', null, '+0.25'], ['Strength', '+30', G, '+4'], ['Lightning damage', '+10%', colors.cyan]], { flavor: '"Hums before a storm. Hums louder during one."', req: 34, price: 1420, weight: 4.5 });
def('treads', 'boots', 'uncommon', "Wayfarer's Treads", 'Uncommon Boots', 88, [['Armor', '88', null, '+18'], ['Movement speed', '+6%', G, '+2%']], { flavor: '"The soles remember every road."', req: 20, price: 210, weight: 2 });
def('oldking', 'amulet', 'legendary', 'Heart of the Old King', 'Legendary Amulet', 240, [['All attributes', '+24', G, '+6'], ['Cooldown reduction', '+8%', G, '+3%'], ['Fire resistance', '+20%', O]], { unique: ['Unique: Falling below 30% health releases a ', 'royal ward', ' for 4 s.'], flavor: '"It still beats, faintly, on cold nights."', req: 40, price: 4100, weight: 0.3 });
def('buckler', 'shield', 'common', 'Oaken Buckler', 'Common Shield', 96, [['Armor', '96', null, '-14'], ['Block chance', '+8%', G]], { flavor: '"Splintered, mended, splintered again."', req: 5, price: 40, weight: 5 });
def('emberhelm', 'helmet', 'rare', 'Helm of the Ember Watch', 'Rare Helmet', 164, [['Armor', '164', null, '+14'], ['Vitality', '+22', G, '+6'], ['Fire resistance', '+10%', O]], { flavor: '"The watch never ended. The watchers did."', req: 36, price: 1180, weight: 3.5 });
def('tidestone', 'gem', 'rare', 'Tidestone', 'Material · Gem', 14, [['Socket bonus', '+6% mana regeneration', colors.cyan], ['Salvages into', '2 Sea glass']], { flavor: '"Polished by ten thousand tides."', req: 20, price: 120, weight: 0.2 });
def('identify', 'scroll', 'common', 'Scroll of Identify', 'Consumable · Scroll', 5, [['Use', 'Reveals an item']], { flavor: '"Mostly reliable."', req: 1, price: 15, weight: 0.1 });
def('greater', 'hp', 'uncommon', 'Greater Healing Draught', 'Consumable · Potion', 900, [['Duration', '6 s'], ['Shared cooldown', '30 s']], { flavor: '"Brewed by the Sisters of the Ashen Well."', req: 20, price: 90, weight: 0.5 });
// Equipped at the start.
def('vanguard', 'helmet', 'rare', 'Visage of the Vanguard', 'Rare Helmet', 150, [['Armor', '150'], ['Vitality', '+16', G]], { flavor: '"Dented exactly where it should be."', req: 30, price: 960, weight: 3.5 });
def('embers', 'amulet', 'legendary', 'Amulet of Embers', 'Legendary Amulet', 228, [['All attributes', '+18', G], ['Fire damage', '+12%', O]], { flavor: '"Warm, always."', req: 38, price: 3600, weight: 0.3 });
def('ashen', 'sword', 'rare', 'Ashen Longsword', 'Rare One-Handed Sword', 716, [['Physical damage', '430 – 580'], ['Attack speed', '1.35'], ['Strength', '+26', G], ['Critical chance', '+4.5%', G]], { flavor: '"Grey as the morning after."', req: 34, price: 1500, weight: 4.5 });
def('kite', 'shield', 'common', 'Iron Kite Shield', 'Common Shield', 110, [['Armor', '110'], ['Block chance', '+6%', G]], { flavor: '"Heavy, honest, unremarkable."', req: 8, price: 60, weight: 6 });
def('valor', 'ring', 'rare', 'Signet of Valor', 'Rare Ring', 96, [['Strength', '+19', G], ['Health', '+140', G]], { flavor: '"Pressed into wax on a hundred oaths."', req: 28, price: 820, weight: 0.1 });
def('traveler', 'boots', 'uncommon', "Traveler's Boots", 'Uncommon Boots', 70, [['Armor', '70'], ['Movement speed', '+4%', G]], { flavor: '"Broken in, never broken."', req: 12, price: 150, weight: 2 });

const START_GRID = [
  ['cinderbrand'], ['whisperwind'], ['draught', 12], ['tonic', 7], ['voidshard', 3], ['recall', 2],
  ['tideband'], ['stormblade'], ['treads'], ['oldking'], ['buckler'], ['emberhelm'],
  ['tidestone', 14], ['identify', 5], null, ['greater', 3], null, null,
  null, null, null, null, null, null,
];
const START_EQUIP = { head: 'vanguard', neck: 'embers', main: 'ashen', off: 'kite', ring: 'valor', feet: 'traveler' };
const DOLL = [
  ['head', 'Head', { left: 18, top: 18 }],
  ['neck', 'Neck', { right: 18, top: 18 }],
  ['main', 'Main', { left: 18, top: 128 }],
  ['off', 'Off', { right: 18, top: 128 }],
  ['ring', 'Ring', { left: 18, top: 238 }],
  ['feet', 'Feet', { right: 18, top: 238 }],
];
const FILTERS = ['All', 'Weapons', 'Armor', 'Consumables', 'Materials'];
const TABS = ['Backpack', 'Equipment', 'Crafting', 'Codex'];

// Geometry (root padding 40/28, header 48 + 22): used for the equip flight.
const GRID_X = 40 + 300 + 20 + 20;
const GRID_Y = 28 + 48 + 22 + 20 + 47;
const CELL = 81;
const DOLL_X = 40;
const DOLL_Y = 28 + 48 + 22;
const dollPos = (slot) => {
  const d = DOLL.find((x) => x[0] === slot)[2];
  return { x: DOLL_X + (d.left != null ? d.left : 300 - 2 - d.right - 60), y: DOLL_Y + 1 + d.top };
};

let uid = 0;
const makeItem = (spec) => (spec ? { uid: ++uid, id: spec[0], count: spec[1] } : null);
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
const useValue = (v) => useRef(new Animated.Value(v)).current;

// ---------------------------------------------------------------------------

/** A number that counts to its new value: the runtime animates and formats
 * it (Animated.Text + Animated.format), no listener or re-render per frame. */
function Ticker({ value, style, duration = 700 }) {
  const v = useRef(new Animated.Value(value, { useNativeDriver: ND })).current;
  const text = useMemo(() => Animated.format(v, { decimals: 0, grouping: ',' }), [v]);
  useEffect(() => {
    Animated.timing(v, { toValue: value, duration, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
  }, [value]);
  return <Animated.Text style={style} text={text} />;
}

/** The hover popout of a grid slot: shown by the runtime from the slot's
 * group state (useGroupState().instant.hovered), no render on hover. */
const SlotPopout = memo(function SlotPopout({ itemId, count, below }) {
  const slot = useGroupState();
  const d = DB[itemId];
  const k = KIND[d.kind];
  const r = rarity[d.tier];
  const amount = k.cat === 'Materials' || d.kind === 'scroll' ? '×' + (count || d.main) : fmt(d.main);
  return (
    <Visibility
      visible={slot.instant.hovered}
      transition={{ duration: 120, easing: 'easeOut' }}
      effect="scale"
      scale={0.92}
      pointerEvents="none"
      style={[styles.popout, below ? styles.popoutBelow : styles.popoutAbove, { borderColor: r.color + '8c', borderTopColor: r.color }]}
    >
      <Text style={[styles.popName, { color: r.color }]} numberOfLines={1}>
        {d.name}
      </Text>
      <Text style={styles.popType} numberOfLines={1}>
        {d.typeName}
      </Text>
      <View style={styles.popRule} />
      <View style={styles.popRow}>
        <Text style={styles.popValue}>{amount}</Text>
        <Text style={styles.popLabel} numberOfLines={1}>
          {k.main}
        </Text>
      </View>
      <View style={[styles.popArrow, below ? styles.popArrowBelow : styles.popArrowAbove, { borderColor: r.color + '8c' }]} />
    </Visibility>
  );
});

const Slot = memo(function Slot({ item, index, selected, onSelect, onActivate, autoFocus, pulse, slotRef }) {
  const s = useValue(selected ? 1 : 0);
  useEffect(() => {
    Animated.spring(s, { toValue: selected ? 1 : 0, stiffness: 380, damping: 18, useNativeDriver: ND }).start();
  }, [selected]);
  // Hover and press come from the runtime: state styles, and the press
  // folded into the selection scale (1 / 1.06, minus 0.06 while pressed).
  const ix = useInteraction();
  const scale = useMemo(() => Animated.derive([s, ix.pressed], (a, p) => 1 + 0.06 * a - 0.06 * p), [s, ix]);
  const d = item && DB[item.id];
  const r = d && rarity[d.tier];
  return (
    <Pressable
      ref={slotRef}
      autoFocus={autoFocus}
      onHoverIn={() => onSelect(index)}
      onFocus={() => onSelect(index)}
      onPress={() => onActivate(index)}
      group
      interaction={ix}
      style={{ cursor: item ? 'pointer' : 'default' }}
      // The hovered slot draws over its neighbours, so its popout does too.
      hoveredStyle={styles.slotOnTop}
    >
      <Animated.View
        style={[
          styles.slot,
          !item && styles.slotEmpty,
          item && {
            borderColor: r.color,
            boxShadow: `inset 0 0 14px ${r.glow}, 0 0 ${d.tier === 'legendary' ? 14 : 6}px ${r.glow}`,
            experimental_backgroundImage: `radial-gradient(circle at 50% 35%, ${r.glow} 0%, rgba(10,14,26,0) 70%)`,
          },
          selected && (item ? styles.slotSelected : styles.slotSelectedEmpty),
          { transform: [{ scale }] },
        ]}
        groupHoveredStyle={!item && !selected ? styles.slotEmptyHover : undefined}
      >
        {item && d.tier === 'legendary' ? (
          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.legendGlow, { opacity: pulse }]} />
        ) : null}
        {item ? <Image source={icon[d.kind]} style={styles.itemIcon} /> : null}
        {item && item.count ? <Text style={styles.count}>{item.count}</Text> : null}
        {item ? <View style={[styles.rarityPip, { backgroundColor: r.color }]} /> : null}
      </Animated.View>
      {item ? <SlotPopout itemId={item.id} count={item.count} below={index < 6} /> : null}
    </Pressable>
  );
});

const EquipSlot = memo(function EquipSlot({ slot, label, pos, itemId, selected, onSelect, bump }) {
  const d = DB[itemId];
  const r = rarity[d.tier];
  return (
    <View style={[styles.equip, pos]}>
      <Pressable onHoverIn={() => onSelect(slot)} onFocus={() => onSelect(slot)} onPress={() => onSelect(slot)} style={{ cursor: 'pointer' }}>
        <Animated.View
          style={[
            styles.equipBox,
            { borderColor: r.color, boxShadow: `inset 0 0 12px ${r.glow}` },
            selected && { borderColor: '#fff3cf', boxShadow: `inset 0 0 14px ${r.glow}, 0 0 0 3px rgba(255,178,63,0.35), 0 0 18px rgba(255,178,63,0.6)` },
            { transform: [{ scale: bump.interpolate({ inputRange: [0, 1], outputRange: [1, 1.22] }) }] },
          ]}
        >
          <Image source={icon[d.kind]} style={{ width: 40, height: 40 }} />
          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, { borderRadius: 10, backgroundColor: r.color, opacity: bump.interpolate({ inputRange: [0, 1], outputRange: [0, 0.5] }) }]} />
        </Animated.View>
      </Pressable>
      <Text style={styles.equipLabel}>{label}</Text>
    </View>
  );
});

function Stat({ label, value, delta, color }) {
  return (
    <View style={styles.statRow}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={[styles.statValue, color && { color }]}>{value}</Text>
      {delta ? <Text style={[styles.delta, { color: delta.startsWith('+') ? colors.green : colors.red }]}>{delta}</Text> : null}
    </View>
  );
}

function Tooltip({ itemId, count, equippedId, compare, anim, isEquipped }) {
  const d = DB[itemId];
  const k = KIND[d.kind];
  const r = rarity[d.tier];
  const eq = equippedId && DB[equippedId];
  const diff = eq && !isEquipped ? d.main - eq.main : 0;
  const gear = !!k.slot;
  return (
    <Animated.View
      style={[
        styles.tooltip,
        {
          borderColor: r.color + '8c',
          borderTopColor: r.color,
          boxShadow: `0 24px 60px rgba(0,0,0,0.7), 0 0 30px ${r.glow.replace(/[\d.]+\)$/, '0.18)')}`,
          opacity: anim.interpolate({ inputRange: [0, 1], outputRange: [0.4, 1] }),
          transform: [{ translateX: anim.interpolate({ inputRange: [0, 1], outputRange: [10, 0] }) }],
        },
      ]}
    >
      <View style={styles.tipHeader}>
        <View style={[styles.tipIconBox, { borderColor: r.color, experimental_backgroundImage: `radial-gradient(circle at 50% 40%, ${r.glow} 0%, rgba(20,14,8,0.9) 75%)` }]}>
          <Image source={icon[d.kind]} style={{ width: 56, height: 56 }} />
        </View>
        <View style={{ flex: 1 }}>
          <Text style={[styles.tipName, { color: r.color }]}>{d.name}</Text>
          <Text style={styles.tipType}>{d.typeName}</Text>
        </View>
      </View>
      <View style={styles.tipRule} />
      <View style={styles.dpsRow}>
        <Text style={styles.dps}>{k.cat === 'Materials' || d.kind === 'scroll' ? '×' + (count || d.main) : fmt(d.main)}</Text>
        <View>
          <Text style={styles.dpsLabel}>{k.main}</Text>
          {isEquipped ? (
            <Text style={[styles.upgrade, { color: colors.gold }]}>● Equipped</Text>
          ) : gear && eq ? (
            <Text style={[styles.upgrade, { color: diff >= 0 ? colors.green : colors.red }]}>
              {diff >= 0 ? '▲' : '▼'} {Math.abs(diff)} vs equipped
            </Text>
          ) : count ? (
            <Text style={[styles.upgrade, { color: colors.textDim }]}>{count} in backpack</Text>
          ) : null}
        </View>
      </View>
      {d.stats.map(([l, v, c, dl]) => (
        <Stat key={l} label={l} value={v} color={c} delta={isEquipped ? null : dl} />
      ))}
      {compare && eq && !isEquipped ? (
        <View style={styles.compare}>
          <Text style={styles.compareKicker}>Currently equipped</Text>
          <View style={styles.compareRow}>
            <Image source={icon[eq.kind]} style={{ width: 28, height: 28 }} />
            <Text style={[styles.compareName, { color: rarity[eq.tier].color }]} numberOfLines={1}>
              {eq.name}
            </Text>
            <Text style={styles.compareValue}>{fmt(eq.main)}</Text>
          </View>
        </View>
      ) : null}
      <View style={styles.tipRule} />
      {d.unique ? (
        <Text style={styles.unique}>
          {d.unique[0]}
          <Text style={{ color: colors.orange, fontWeight: '700' }}>{d.unique[1]}</Text>
          {d.unique[2]}
        </Text>
      ) : null}
      <Text style={styles.flavor} numberOfLines={2}>
        {d.flavor}
      </Text>
      <View style={styles.tipFooter}>
        <Text style={styles.reqText}>Requires Level {d.req}</Text>
        <View style={{ flex: 1 }} />
        <View style={styles.coinSmall} />
        <Text style={styles.price}>{fmt(d.price)}</Text>
      </View>
    </Animated.View>
  );
}

function HintButton({ k, label, color, onPress, flash }) {
  return (
    <Pressable focusable={false} onPress={onPress} group style={{ cursor: 'pointer' }}>
      <View style={styles.hint} groupHoveredStyle={styles.opaque}>
        <Animated.View style={[styles.hintKey, { backgroundColor: color, transform: [{ scale: flash.interpolate({ inputRange: [0, 1], outputRange: [1, 1.3] }) }] }]}>
          <Text style={styles.hintKeyText}>{k}</Text>
        </Animated.View>
        <Text style={styles.hintText} groupHoveredStyle={styles.hintTextHot}>
          {label}
        </Text>
      </View>
    </Pressable>
  );
}

// ---------------------------------------------------------------------------

export default function Inventory({ active = true }) {
  const [grid, setGrid] = useState(() => START_GRID.map(makeItem));
  const [equip, setEquip] = useState(START_EQUIP);
  const [filter, setFilter] = useState(0);
  const [tab, setTab] = useState(0);
  const [sel, setSel] = useState(0); // grid cell index or a doll slot name
  const [tipId, setTipId] = useState('cinderbrand');
  const [compare, setCompare] = useState(false);
  const [gold, setGold] = useState(48215);
  const [gs, setGs] = useState(1384);
  const [toast, setToast] = useState(null);
  const [flight, setFlight] = useState(null);

  const tipAnim = useValue(1);
  const pulse = useValue(0.3);
  const tabX = useValue(0);
  const filterX = useValue(0);
  const enter = useValue(active ? 0 : 1);
  const toastT = useValue(0);
  const fly = useValue(0);
  const bumps = useRef(Object.fromEntries(DOLL.map((d) => [d[0], new Animated.Value(0)]))).current;
  const flash = useRef({ A: new Animated.Value(0), X: new Animated.Value(0), Y: new Animated.Value(0), B: new Animated.Value(0) }).current;

  useEffect(() => {
    if (active) Animated.timing(enter, { toValue: 1, duration: 600, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
  }, []);
  useEffect(() => {
    if (!active) return undefined;
    const l = Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, { toValue: 1, duration: 1100, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        Animated.timing(pulse, { toValue: 0.3, duration: 1100, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
      ]),
    );
    l.start();
    return () => l.stop();
  }, [active]);

  // Visible cells: the tab's dataset, filtered, padded to 24 cells.
  const cells = useMemo(() => {
    let items;
    if (tab === 0) items = grid.filter(Boolean);
    else if (tab === 1) items = DOLL.map(([slot]) => ({ uid: 'eq-' + slot, id: equip[slot] }));
    else if (tab === 2) items = grid.filter((it) => it && (KIND[DB[it.id].kind].cat === 'Materials' || DB[it.id].kind === 'scroll'));
    else items = Object.keys(DB).map((id) => ({ uid: 'cx-' + id, id }));
    const cat = FILTERS[filter];
    if (cat !== 'All') items = items.filter((it) => KIND[DB[it.id].kind].cat === cat);
    if (tab === 0 && cat === 'All') return grid; // keep the player's own arrangement
    const out = items.slice(0, 24);
    while (out.length < 24) out.push(null);
    return out;
  }, [grid, equip, filter, tab]);

  const selItem = typeof sel === 'number' ? cells[sel] : { id: equip[sel] };
  const selDef = selItem && DB[selItem.id];

  // The details card follows the selection (empty cells keep the last item).
  useEffect(() => {
    if (!selItem || selItem.id === tipId) return;
    setTipId(selItem.id);
    tipAnim.setValue(0);
    Animated.spring(tipAnim, { toValue: 1, stiffness: 300, damping: 24, useNativeDriver: ND }).start();
  }, [selItem && selItem.id]);

  const tipDef = DB[tipId];
  const tipSlot = KIND[tipDef.kind].slot;
  const tipCount = typeof sel === 'number' && selItem && selItem.id === tipId ? selItem.count : null;
  const tipEquipped = typeof sel === 'string' || tab === 1 || (tipSlot && equip[tipSlot] === tipId);

  const say = (text, color = colors.gold) => {
    setToast({ text, color, at: Date.now() });
    toastT.setValue(0);
    Animated.sequence([
      Animated.spring(toastT, { toValue: 1, stiffness: 260, damping: 18, useNativeDriver: ND }),
      Animated.delay(1400),
      Animated.timing(toastT, { toValue: 0, duration: 260, useNativeDriver: ND }),
    ]).start();
  };
  const press = (k) => {
    flash[k].setValue(1);
    Animated.timing(flash[k], { toValue: 0, duration: 380, useNativeDriver: ND }).start();
  };

  const onSelect = useCallback((i) => setSel(i), []);
  // After a category / tab switch the focused cell may be gone: focus the first one.
  const firstSlot = useRef(null);
  const refocus = useRef(false);
  useEffect(() => {
    if (!refocus.current) return;
    refocus.current = false;
    const r = firstSlot.current;
    if (r && r.focus) r.focus();
  }, [filter, tab]);

  const setFilterAnimated = (i) => {
    const n = (i + FILTERS.length) % FILTERS.length;
    LayoutAnimation.configureNext({ duration: 320, update: { type: 'spring', springDamping: 0.8 }, create: { type: 'easeInEaseOut', property: 'opacity' }, delete: { type: 'easeInEaseOut', property: 'opacity' } });
    setFilter(n);
    setSel(0);
    refocus.current = true;
  };
  const setTabAnimated = (i) => {
    const n = (i + TABS.length) % TABS.length;
    LayoutAnimation.configureNext({ duration: 300, create: { type: 'easeInEaseOut', property: 'opacity' }, update: { type: 'easeInEaseOut' }, delete: { type: 'easeInEaseOut', property: 'opacity' } });
    setTab(n);
    setFilter(0);
    setSel(0);
    refocus.current = true;
  };
  useEffect(() => {
    Animated.spring(tabX, { toValue: TAB_OFFSETS[tab], stiffness: 300, damping: 26, useNativeDriver: ND }).start();
  }, [tab]);
  useEffect(() => {
    Animated.spring(filterX, { toValue: FILTER_OFFSETS[filter], stiffness: 300, damping: 26, useNativeDriver: ND }).start();
  }, [filter]);

  // A / Enter / click: equip gear, use consumables.
  const activate = useCallback(
    (index) => {
      press('A');
      const cell = typeof index === 'number' ? cells[index] : null;
      if (!cell) return;
      const d = DB[cell.id];
      const k = KIND[d.kind];
      if (tab !== 0) {
        say(tab === 1 ? `${d.name} is equipped` : d.name, rarity[d.tier].color);
        return;
      }
      if (k.slot) {
        const old = equip[k.slot];
        if (old === cell.id) return;
        const gi = grid.indexOf(cell);
        const from = { x: GRID_X + (gi % 6) * CELL, y: GRID_Y + Math.floor(gi / 6) * CELL };
        const to = dollPos(k.slot);
        setFlight({ kind: d.kind, from, to, tier: d.tier });
        fly.setValue(0);
        Animated.timing(fly, { toValue: 1, duration: 460, easing: Easing.inOut(Easing.cubic), useNativeDriver: ND }).start(() => {
          setFlight(null);
          setEquip((e) => Object.assign({}, e, { [k.slot]: cell.id }));
          bumps[k.slot].setValue(1);
          Animated.spring(bumps[k.slot], { toValue: 0, stiffness: 240, damping: 9, useNativeDriver: ND }).start();
          setGs((g) => g + Math.round((d.main - DB[old].main) * 0.6));
        });
        // The previously equipped piece takes the cell.
        LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));
        setGrid((g) => g.map((it) => (it === cell ? makeItem([old]) : it)));
        say(`Equipped ${d.name}`, rarity[d.tier].color);
      } else if (k.cat === 'Consumables') {
        setGrid((g) => g.map((it) => (it === cell ? (it.count > 1 ? Object.assign({}, it, { count: it.count - 1 }) : null) : it)));
        say(d.kind === 'scroll' ? `Read ${d.name}` : `Drank ${d.name} · +${d.main}`, d.kind === 'mp' ? colors.cyan : colors.green);
      } else say(`${d.name} · socket it at a forge`, rarity[d.tier].color);
    },
    [cells, grid, equip, tab],
  );

  const salvage = () => {
    press('Y');
    if (tab !== 0 || typeof sel !== 'number') return;
    const cell = cells[sel];
    if (!cell) return;
    const d = DB[cell.id];
    LayoutAnimation.configureNext({ duration: 300, delete: { type: 'easeInEaseOut', property: 'scaleXY' }, create: { type: 'easeInEaseOut', property: 'opacity' }, update: { type: 'easeInEaseOut' } });
    setGrid((g) => g.map((it) => (it === cell ? null : it)));
    const value = Math.round(d.price * 0.25) * (cell.count || 1);
    setGold((x) => x + value);
    say(`Salvaged ${d.name} · +${fmt(value)} gold`, colors.goldLight);
  };

  const close = () => {
    press('B');
    Animated.sequence([
      Animated.timing(enter, { toValue: 0, duration: 260, easing: Easing.in(Easing.cubic), useNativeDriver: ND }),
      Animated.delay(450),
      Animated.timing(enter, { toValue: 1, duration: 520, easing: Easing.out(Easing.cubic), useNativeDriver: ND }),
    ]).start();
  };

  const onKeyDown = (e) => {
    const k = (e.key || '').toLowerCase();
    if (k === 'q') setFilterAnimated(filter - 1);
    else if (k === 'e') setFilterAnimated(filter + 1);
    else if (k >= '1' && k <= '4') setTabAnimated(Number(k) - 1);
    else if (k === 'x' || k === 'c') {
      press('X');
      setCompare((c) => !c);
    } else if (k === 'y' || k === 'delete' || k === 'backspace') salvage();
    else return;
    e.preventDefault();
  };
  const onGamepadButton = (e) => {
    if (!e.pressed) return;
    const b = e.buttonIndex;
    if (b === 4) setFilterAnimated(filter - 1);
    else if (b === 5) setFilterAnimated(filter + 1);
    else if (b === 6) setTabAnimated(tab - 1);
    else if (b === 7) setTabAnimated(tab + 1);
    else if (b === 2) {
      press('X');
      setCompare((c) => !c);
    } else if (b === 3) salvage();
    else return;
    e.preventDefault();
  };

  const used = grid.filter(Boolean).length;
  const weight = grid.reduce((w, it) => w + (it ? DB[it.id].weight * (it.count || 1) * 0.35 : 0), 0) + 52.1;
  const aLabel = tab !== 0 ? 'Inspect' : selDef && KIND[selDef.kind].slot ? 'Equip' : selDef && KIND[selDef.kind].cat === 'Consumables' ? 'Use' : 'Inspect';

  return (
    <View style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onGamepadButton}>
      <Animated.View style={{ flex: 1, opacity: enter, transform: [{ scale: enter.interpolate({ inputRange: [0, 1], outputRange: [0.97, 1] }) }] }}>
        <View style={styles.header}>
          <Text style={styles.screenTitle}>Inventory</Text>
          <View style={styles.tabs}>
            <Animated.View style={[styles.tabActive, { width: TAB_WIDTHS[tab], transform: [{ translateX: tabX }] }]} />
            {TABS.map((t, i) => (
              <Pressable key={t} focusable={false} onPress={() => setTabAnimated(i)} group style={[styles.tab, { width: TAB_WIDTHS[i] }]}>
                <Text style={[styles.tabText, i === tab && styles.tabTextActive]} groupHoveredStyle={styles.tabTextActive}>
                  {t}
                </Text>
              </Pressable>
            ))}
          </View>
          <View style={styles.trigger}>
            <Text style={styles.triggerText}>LT</Text>
            <Text style={styles.triggerText}>RT</Text>
          </View>
          <View style={{ flex: 1 }} />
          <View style={styles.headerStat}>
            <View style={styles.coin} />
            <Ticker value={gold} style={styles.headerStatText} />
          </View>
          <View style={styles.headerStat}>
            <Text style={styles.headerStatLabel}>Weight</Text>
            <Text style={styles.headerStatText}>{weight.toFixed(1)} / 120</Text>
          </View>
        </View>

        <View style={styles.body}>
          {/* Paper doll */}
          <View style={styles.doll}>
            <Image source={require('../../../examples/assets/portrait-rogue.png')} style={styles.dollArt} resizeMode="cover" />
            <View style={styles.dollFade} />
            {DOLL.map(([slot, label, pos]) => (
              <EquipSlot key={slot} slot={slot} label={label} pos={pos} itemId={equip[slot]} selected={sel === slot} onSelect={onSelect} bump={bumps[slot]} />
            ))}
            <View style={styles.power}>
              <Text style={styles.powerLabel}>Gear Score</Text>
              <Ticker value={gs} style={styles.powerValue} />
              <View style={styles.powerBar}>
                <View style={[styles.powerFill, { width: `${Math.min(100, (gs / 1920) * 100)}%` }]} />
              </View>
            </View>
          </View>

          {/* Grid */}
          <View style={styles.gridPanel}>
            <View style={styles.filters}>
              <View style={styles.bumper}>
                <Text style={styles.bumperText}>LB</Text>
              </View>
              <Animated.View style={[styles.filterActiveBg, { width: FILTER_WIDTHS[filter], transform: [{ translateX: filterX }] }]} />
              {FILTERS.map((f, i) => (
                <Pressable key={f} focusable={false} onPress={() => setFilterAnimated(i)} group style={{ cursor: 'pointer' }}>
                  <View style={[styles.filter, { width: FILTER_WIDTHS[i] }, i === filter && styles.filterActive]} groupHoveredStyle={i !== filter ? styles.filterHover : undefined}>
                    <Text style={[styles.filterText, i === filter && { color: '#1a1206' }]}>{f}</Text>
                  </View>
                </Pressable>
              ))}
              <View style={styles.bumper}>
                <Text style={styles.bumperText}>RB</Text>
              </View>
            </View>
            <View style={styles.grid}>
              {cells.map((it, i) => (
                <Slot
                  key={tab === 0 && filter === 0 ? 'c' + i : it ? it.uid : 'e' + i}
                  slotRef={i === 0 ? firstSlot : undefined}
                  item={it}
                  index={i}
                  selected={sel === i}
                  onSelect={onSelect}
                  onActivate={activate}
                  autoFocus={active && i === 0}
                  pulse={pulse}
                />
              ))}
            </View>
            <Text style={styles.capacity}>{tab === 0 ? `${used} / 24 slots` : tab === 3 ? `${Object.keys(DB).length} entries discovered` : `${cells.filter(Boolean).length} items`}</Text>
          </View>

          {/* Details */}
          <Tooltip itemId={tipId} count={tipCount} equippedId={tipSlot ? equip[tipSlot] : null} compare={compare} anim={tipAnim} isEquipped={tipEquipped} />
        </View>

        <View style={styles.footer}>
          <HintButton k="A" label={aLabel} color="#5fe08a" flash={flash.A} onPress={() => typeof sel === 'number' && activate(sel)} />
          <HintButton k="X" label={compare ? 'Hide compare' : 'Compare'} color="#6bb8ff" flash={flash.X} onPress={() => (press('X'), setCompare((c) => !c))} />
          <HintButton k="Y" label="Salvage" color="#ffd36b" flash={flash.Y} onPress={salvage} />
          <HintButton k="B" label="Close" color="#ff6b81" flash={flash.B} onPress={close} />
        </View>
      </Animated.View>

      {flight ? (
        <Animated.View
          pointerEvents="none"
          style={[
            styles.flight,
            {
              boxShadow: `0 0 24px ${rarity[flight.tier].glow}`,
              transform: [
                { translateX: fly.interpolate({ inputRange: [0, 1], outputRange: [flight.from.x, flight.to.x] }) },
                { translateY: fly.interpolate({ inputRange: [0, 0.5, 1], outputRange: [flight.from.y, Math.min(flight.from.y, flight.to.y) - 50, flight.to.y] }) },
                { scale: fly.interpolate({ inputRange: [0, 0.5, 1], outputRange: [1, 1.35, 0.85] }) },
                { rotate: fly.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '-360deg'] }) },
              ],
            },
          ]}
        >
          <Image source={icon[flight.kind]} style={{ width: 50, height: 50 }} />
        </Animated.View>
      ) : null}

      {toast ? (
        <Animated.View
          pointerEvents="none"
          style={[styles.toast, { borderColor: toast.color, opacity: toastT, transform: [{ translateY: toastT.interpolate({ inputRange: [0, 1], outputRange: [16, 0] }) }] }]}
        >
          <View style={[styles.toastDot, { backgroundColor: toast.color }]} />
          <Text style={styles.toastText}>{toast.text}</Text>
        </Animated.View>
      ) : null}
    </View>
  );
}

// Tab / filter pill geometry for the sliding highlights.
const TAB_WIDTHS = [104, 112, 96, 80];
const TAB_OFFSETS = TAB_WIDTHS.map((_, i) => TAB_WIDTHS.slice(0, i).reduce((a, b) => a + b + 4, 0));
const FILTER_WIDTHS = [44, 78, 62, 102, 86];
const FILTER_OFFSETS = FILTER_WIDTHS.map((_, i) => 36 + 6 + FILTER_WIDTHS.slice(0, i).reduce((a, b) => a + b + 6, 0));

const styles = StyleSheet.create({
  root: {
    width: 1280,
    height: 720,
    overflow: 'hidden',
    backgroundColor: colors.bg0,
    experimental_backgroundImage:
      'radial-gradient(circle at 20% 0%, rgba(79,140,255,0.18) 0%, rgba(7,10,20,0) 55%), radial-gradient(circle at 90% 100%, rgba(255,178,63,0.12) 0%, rgba(7,10,20,0) 50%)',
    paddingHorizontal: 40,
    paddingTop: 28,
  },
  header: { flexDirection: 'row', alignItems: 'center', gap: 28, height: 48 },
  screenTitle: { fontFamily: font, fontWeight: '700', fontSize: 26, letterSpacing: 4, textTransform: 'uppercase', color: colors.text },
  tabs: { flexDirection: 'row', gap: 4, padding: 4, borderRadius: 10, backgroundColor: 'rgba(255,255,255,0.04)', borderWidth: 1, borderColor: colors.line },
  tab: { paddingVertical: 8, borderRadius: 7, alignItems: 'center', cursor: 'pointer' },
  tabActive: { position: 'absolute', left: 4, top: 4, bottom: 4, borderRadius: 7, backgroundColor: colors.panelRaised, boxShadow: '0 2px 8px rgba(0,0,0,0.4), inset 0 -2px 0 #f2c35b' },
  tabText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textMute },
  tabTextActive: { color: colors.text },
  trigger: { flexDirection: 'row', gap: 4, marginLeft: -16 },
  triggerText: { fontFamily: font, fontWeight: '700', fontSize: 10, color: colors.textMute, paddingHorizontal: 5, paddingVertical: 2, borderRadius: 4, borderWidth: 1, borderColor: colors.line },
  headerStat: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  headerStatLabel: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textMute },
  headerStatText: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text },
  coin: { width: 16, height: 16, borderRadius: 8, experimental_backgroundImage: 'radial-gradient(circle at 35% 35%, #fff2c2 0%, #f2c35b 45%, #a8741f 100%)' },

  body: { flexDirection: 'row', gap: 20, marginTop: 22, flex: 1 },
  doll: { width: 300, height: 532, borderRadius: 16, overflow: 'hidden', backgroundColor: colors.panelSolid, borderWidth: 1, borderColor: colors.line },
  dollArt: { position: 'absolute', left: 0, right: 0, top: 30, height: 360, opacity: 0.85 },
  dollFade: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(18,26,46,0.2) 0%, rgba(18,26,46,0) 40%, rgba(18,26,46,1) 75%)' },
  equip: { position: 'absolute', alignItems: 'center' },
  equipBox: { width: 60, height: 60, borderRadius: 12, borderWidth: 2, backgroundColor: 'rgba(8,12,22,0.8)', alignItems: 'center', justifyContent: 'center' },
  equipLabel: { fontFamily: font, fontWeight: '600', fontSize: 10, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textDim, marginTop: 5 },
  power: { position: 'absolute', left: 24, right: 24, bottom: 24 },
  powerLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute },
  powerValue: { fontFamily: font, fontWeight: '700', fontSize: 36, color: colors.goldLight, marginTop: 2, textShadowColor: 'rgba(242,195,91,0.5)', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 14 },
  powerBar: { height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.08)', marginTop: 10, overflow: 'hidden' },
  powerFill: { height: '100%', borderRadius: 3, experimental_backgroundImage: 'linear-gradient(90deg, #a8741f, #ffe29a)' },

  gridPanel: { width: 522, height: 532, padding: 20, borderRadius: 16, backgroundColor: 'rgba(14,20,36,0.7)', borderWidth: 1, borderColor: colors.line },
  filters: { flexDirection: 'row', alignItems: 'center', gap: 6, marginBottom: 18, height: 29 },
  bumper: { width: 30, height: 20, borderRadius: 6, borderWidth: 1, borderColor: colors.lineStrong, alignItems: 'center', justifyContent: 'center' },
  bumperText: { fontFamily: font, fontWeight: '700', fontSize: 9, color: colors.textMute },
  filterActiveBg: { position: 'absolute', left: 0, top: 0, height: 29, borderRadius: 14, backgroundColor: colors.gold, boxShadow: '0 0 14px rgba(242,195,91,0.45)' },
  filter: { height: 29, alignItems: 'center', justifyContent: 'center', borderRadius: 14, borderWidth: 1, borderColor: colors.lineStrong },
  filterActive: { borderColor: 'transparent' },
  filterHover: { borderColor: 'rgba(242,195,91,0.6)', backgroundColor: 'rgba(242,195,91,0.08)' },
  filterText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  slot: { width: 71, height: 71, borderRadius: 10, borderWidth: 1.5, backgroundColor: 'rgba(10,14,26,0.9)', alignItems: 'center', justifyContent: 'center' },
  slotEmpty: { borderColor: 'rgba(148,170,255,0.10)', backgroundColor: 'rgba(10,14,26,0.45)' },
  slotSelected: {
    borderWidth: 2,
    borderColor: '#fff3cf',
    boxShadow: '0 0 0 3px rgba(255,178,63,0.35), 0 0 24px rgba(255,178,63,0.75), inset 0 0 18px rgba(255,178,63,0.5)',
    zIndex: 2,
  },
  slotEmptyHover: { borderColor: 'rgba(148,170,255,0.3)' },
  slotOnTop: { zIndex: 10 },
  popout: {
    position: 'absolute',
    left: (71 - 220) / 2,
    width: 220,
    paddingHorizontal: 12,
    paddingTop: 10,
    paddingBottom: 11,
    borderRadius: 10,
    backgroundColor: 'rgba(16,12,8,0.97)',
    borderWidth: 1,
    borderTopWidth: 2,
    boxShadow: '0 14px 34px rgba(0,0,0,0.7)',
  },
  popoutAbove: { bottom: 71 + 12 },
  popoutBelow: { top: 71 + 12 },
  popName: { fontFamily: font, fontWeight: '700', fontSize: 14, lineHeight: 18 },
  popType: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textDim, marginTop: 2 },
  popRule: { height: 1, backgroundColor: 'rgba(255,178,63,0.22)', marginVertical: 8 },
  popRow: { flexDirection: 'row', alignItems: 'baseline', gap: 8 },
  popValue: { fontFamily: font, fontWeight: '700', fontSize: 20, color: '#fff', letterSpacing: -0.3 },
  popLabel: { flexShrink: 1, fontFamily: font, fontWeight: '600', fontSize: 10, letterSpacing: 0.8, textTransform: 'uppercase', color: colors.textMute },
  popArrow: { position: 'absolute', left: 110 - 6, width: 12, height: 12, backgroundColor: 'rgba(16,12,8,0.97)', transform: [{ rotate: '45deg' }] },
  popArrowAbove: { bottom: -7, borderRightWidth: 1, borderBottomWidth: 1 },
  popArrowBelow: { top: -7, borderLeftWidth: 1, borderTopWidth: 1 },
  slotSelectedEmpty: { borderColor: 'rgba(255,243,207,0.5)', boxShadow: '0 0 0 2px rgba(255,178,63,0.2)', zIndex: 2 },
  legendGlow: { borderRadius: 9, boxShadow: 'inset 0 0 20px rgba(255,178,63,0.55)' },
  itemIcon: { width: 50, height: 50 },
  count: { position: 'absolute', right: 6, bottom: 3, fontFamily: font, fontWeight: '700', fontSize: 12, color: '#fff', textShadowColor: '#000', textShadowOffset: { width: 0, height: 1 }, textShadowRadius: 2 },
  rarityPip: { position: 'absolute', left: 6, top: 6, width: 5, height: 5, borderRadius: 3 },
  capacity: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute, marginTop: 'auto', textAlign: 'right' },

  tooltip: {
    flex: 1,
    alignSelf: 'flex-start',
    padding: 18,
    borderRadius: 14,
    backgroundColor: 'rgba(16,12,8,0.96)',
    borderWidth: 1,
    borderTopWidth: 3,
  },
  tipHeader: { flexDirection: 'row', gap: 12, alignItems: 'center' },
  tipIconBox: { width: 68, height: 68, borderRadius: 12, alignItems: 'center', justifyContent: 'center', borderWidth: 1.5 },
  tipName: { fontFamily: font, fontWeight: '700', fontSize: 18, lineHeight: 22 },
  tipType: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim, marginTop: 3 },
  tipRule: { height: 1, backgroundColor: 'rgba(255,178,63,0.22)', marginVertical: 12 },
  dpsRow: { flexDirection: 'row', alignItems: 'center', gap: 12, marginBottom: 8 },
  dps: { fontFamily: font, fontWeight: '700', fontSize: 38, color: '#fff', letterSpacing: -0.5 },
  upgrade: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.green, marginTop: 3 },
  dpsLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1, textTransform: 'uppercase', color: colors.textMute },
  statRow: { flexDirection: 'row', alignItems: 'center', paddingVertical: 3 },
  statLabel: { flex: 1, fontFamily: font, fontSize: 13, color: colors.textDim },
  statValue: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
  delta: { fontFamily: font, fontWeight: '600', fontSize: 11, marginLeft: 8, width: 44, textAlign: 'right' },
  compare: { marginTop: 10, padding: 10, borderRadius: 10, backgroundColor: 'rgba(79,140,255,0.08)', borderWidth: 1, borderColor: 'rgba(107,184,255,0.35)' },
  compareKicker: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.4, textTransform: 'uppercase', color: '#6bb8ff' },
  compareRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 6 },
  compareName: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 13 },
  compareValue: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text },
  unique: { fontFamily: font, fontSize: 13, lineHeight: 19, color: '#ffd9a8', marginBottom: 10 },
  flavor: { fontFamily: font, fontStyle: 'italic', fontSize: 12, lineHeight: 18, color: colors.textMute },
  tipFooter: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 14 },
  reqText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim },
  coinSmall: { width: 12, height: 12, borderRadius: 6, backgroundColor: colors.gold },
  price: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.goldLight },

  footer: { position: 'absolute', left: 0, bottom: 22, flexDirection: 'row', gap: 22 },
  hint: { flexDirection: 'row', alignItems: 'center', gap: 8, opacity: 0.95 },
  hintKey: { width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center' },
  hintKeyText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#0b0f1a' },
  opaque: { opacity: 1 },
  hintTextHot: { color: colors.text },
  hintText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },

  flight: { position: 'absolute', left: 0, top: 0, width: 71, height: 71, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(10,14,26,0.6)' },
  toast: {
    position: 'absolute',
    right: 40,
    bottom: 18,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 16,
    paddingVertical: 10,
    borderRadius: 10,
    borderWidth: 1,
    backgroundColor: 'rgba(12,16,30,0.96)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.6)',
  },
  toastDot: { width: 8, height: 8, borderRadius: 4 },
  toastText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
});
