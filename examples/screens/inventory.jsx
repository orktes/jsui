// Inventory: tabs, equipment paper doll, rarity-bordered item grid with a
// focused slot, and an item tooltip card.
import React from 'react';
import { Image, StyleSheet, Text, View } from 'react-native';
import { colors, font, rarity } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Inventory' };

const icon = {
  sword: require('../assets/item-sword.png'),
  shield: require('../assets/item-shield.png'),
  hp: require('../assets/item-potion-health.png'),
  mp: require('../assets/item-potion-mana.png'),
  gem: require('../assets/item-gem.png'),
  bow: require('../assets/item-bow.png'),
  helmet: require('../assets/item-helmet.png'),
  ring: require('../assets/item-ring.png'),
  scroll: require('../assets/item-scroll.png'),
  boots: require('../assets/item-boots.png'),
  amulet: require('../assets/item-amulet.png'),
  axe: require('../assets/item-axe.png'),
};

// 6 x 4 grid; null = empty slot.
const GRID = [
  ['axe', 'legendary'], ['bow', 'epic'], ['hp', 'common', 12], ['mp', 'common', 7], ['gem', 'epic', 3], ['scroll', 'uncommon', 2],
  ['ring', 'rare'], ['sword', 'rare'], ['boots', 'uncommon'], ['amulet', 'legendary'], ['shield', 'common'], ['helmet', 'rare'],
  ['gem', 'rare', 14], ['scroll', 'common', 5], null, ['hp', 'uncommon', 3], null, null,
  null, null, null, null, null, null,
];
const SELECTED = 0;

function Slot({ item, selected }) {
  if (!item) return <View style={[styles.slot, styles.slotEmpty]} />;
  const [name, tier, count] = item;
  const r = rarity[tier];
  return (
    <View
      style={[
        styles.slot,
        {
          borderColor: r.color,
          boxShadow: `inset 0 0 14px ${r.glow}, 0 0 ${tier === 'legendary' ? 14 : 6}px ${r.glow}`,
          experimental_backgroundImage: `radial-gradient(circle at 50% 35%, ${r.glow} 0%, rgba(10,14,26,0) 70%)`,
        },
        selected && styles.slotSelected,
      ]}
    >
      <Image source={icon[name]} style={styles.itemIcon} />
      {count ? <Text style={styles.count}>{count}</Text> : null}
      <View style={[styles.rarityPip, { backgroundColor: r.color }]} />
    </View>
  );
}

function EquipSlot({ item, tier, label, style }) {
  const r = rarity[tier];
  return (
    <View style={[styles.equip, style]}>
      <View style={[styles.equipBox, { borderColor: r.color, boxShadow: `inset 0 0 12px ${r.glow}` }]}>
        <Image source={icon[item]} style={{ width: 40, height: 40 }} />
      </View>
      <Text style={styles.equipLabel}>{label}</Text>
    </View>
  );
}

function Stat({ label, value, delta, color }) {
  return (
    <View style={styles.statRow}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={[styles.statValue, color && { color }]}>{value}</Text>
      {delta ? <Text style={[styles.delta, { color: delta.startsWith('+') ? colors.green : colors.red }]}>{delta}</Text> : null}
    </View>
  );
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.header}>
        <Text style={styles.screenTitle}>Inventory</Text>
        <View style={styles.tabs}>
          {['Backpack', 'Equipment', 'Crafting', 'Codex'].map((t, i) => (
            <View key={t} style={[styles.tab, i === 0 && styles.tabActive]}>
              <Text style={[styles.tabText, i === 0 && styles.tabTextActive]}>{t}</Text>
            </View>
          ))}
        </View>
        <View style={{ flex: 1 }} />
        <View style={styles.headerStat}>
          <View style={styles.coin} />
          <Text style={styles.headerStatText}>48,215</Text>
        </View>
        <View style={styles.headerStat}>
          <Text style={styles.headerStatLabel}>Weight</Text>
          <Text style={styles.headerStatText}>84.5 / 120</Text>
        </View>
      </View>

      <View style={styles.body}>
        {/* Paper doll */}
        <View style={styles.doll}>
          <Image source={require('../assets/portrait-rogue.png')} style={styles.dollArt} resizeMode="cover" />
          <View style={styles.dollFade} />
          <EquipSlot item="helmet" tier="rare" label="Head" style={{ left: 18, top: 18 }} />
          <EquipSlot item="amulet" tier="legendary" label="Neck" style={{ right: 18, top: 18 }} />
          <EquipSlot item="sword" tier="rare" label="Main" style={{ left: 18, top: 128 }} />
          <EquipSlot item="shield" tier="common" label="Off" style={{ right: 18, top: 128 }} />
          <EquipSlot item="ring" tier="rare" label="Ring" style={{ left: 18, top: 238 }} />
          <EquipSlot item="boots" tier="uncommon" label="Feet" style={{ right: 18, top: 238 }} />
          <View style={styles.power}>
            <Text style={styles.powerLabel}>Gear Score</Text>
            <Text style={styles.powerValue}>1,384</Text>
            <View style={styles.powerBar}>
              <View style={styles.powerFill} />
            </View>
          </View>
        </View>

        {/* Grid */}
        <View style={styles.gridPanel}>
          <View style={styles.filters}>
            {['All', 'Weapons', 'Armor', 'Consumables', 'Materials'].map((f, i) => (
              <View key={f} style={[styles.filter, i === 0 && styles.filterActive]}>
                <Text style={[styles.filterText, i === 0 && { color: '#1a1206' }]}>{f}</Text>
              </View>
            ))}
          </View>
          <View style={styles.grid}>
            {GRID.map((it, i) => (
              <Slot key={i} item={it} selected={i === SELECTED} />
            ))}
          </View>
          <Text style={styles.capacity}>16 / 24 slots</Text>
        </View>

        {/* Tooltip */}
        <View style={styles.tooltip}>
          <View style={styles.tipHeader}>
            <View style={styles.tipIconBox}>
              <Image source={icon.axe} style={{ width: 56, height: 56 }} />
            </View>
            <View style={{ flex: 1 }}>
              <Text style={styles.tipName}>Cinderbrand, Oath of Ash</Text>
              <Text style={styles.tipType}>Legendary Two-Handed Axe</Text>
            </View>
          </View>
          <View style={styles.tipRule} />
          <View style={styles.dpsRow}>
            <Text style={styles.dps}>842</Text>
            <View>
              <Text style={styles.dpsLabel}>Damage per second</Text>
              <Text style={styles.upgrade}>▲ 126 vs equipped</Text>
            </View>
          </View>
          <Stat label="Physical damage" value="612 – 790" />
          <Stat label="Attack speed" value="1.20" delta="-0.15" />
          <Stat label="Strength" value="+48" color={colors.green} delta="+22" />
          <Stat label="Critical chance" value="+6.5%" color={colors.green} delta="+2.0%" />
          <Stat label="Fire damage" value="+18%" color={colors.orange} />
          <View style={styles.tipRule} />
          <Text style={styles.unique}>
            Unique: Every third strike erupts in cinders, dealing <Text style={{ color: colors.orange, fontWeight: '700' }}>240%</Text> weapon damage to nearby foes.
          </Text>
          <Text style={styles.flavor} numberOfLines={2}>
            "Forged in the last breath of the Old King, it remembers every oath ever broken over its edge."
          </Text>
          <View style={styles.tipFooter}>
            <Text style={styles.reqText}>Requires Level 40</Text>
            <View style={{ flex: 1 }} />
            <View style={styles.coinSmall} />
            <Text style={styles.price}>3,750</Text>
          </View>
        </View>
      </View>

      <View style={styles.footer}>
        {[['A', 'Equip', '#5fe08a'], ['X', 'Compare', '#6bb8ff'], ['Y', 'Salvage', '#ffd36b'], ['B', 'Close', '#ff6b81']].map(([k, l, c]) => (
          <View key={k} style={styles.hint}>
            <View style={[styles.hintKey, { backgroundColor: c }]}>
              <Text style={styles.hintKeyText}>{k}</Text>
            </View>
            <Text style={styles.hintText}>{l}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: colors.bg0,
    experimental_backgroundImage:
      'radial-gradient(circle at 20% 0%, rgba(79,140,255,0.18) 0%, rgba(7,10,20,0) 55%), radial-gradient(circle at 90% 100%, rgba(255,178,63,0.12) 0%, rgba(7,10,20,0) 50%)',
    paddingHorizontal: 40,
    paddingTop: 28,
  },
  header: { flexDirection: 'row', alignItems: 'center', gap: 28, height: 48 },
  screenTitle: { fontFamily: font, fontWeight: '700', fontSize: 26, letterSpacing: 4, textTransform: 'uppercase', color: colors.text },
  tabs: { flexDirection: 'row', gap: 4, padding: 4, borderRadius: 10, backgroundColor: 'rgba(255,255,255,0.04)', borderWidth: 1, borderColor: colors.line },
  tab: { paddingHorizontal: 16, paddingVertical: 8, borderRadius: 7 },
  tabActive: { backgroundColor: colors.panelRaised, boxShadow: '0 2px 8px rgba(0,0,0,0.4), inset 0 -2px 0 #f2c35b' },
  tabText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textMute },
  tabTextActive: { color: colors.text },
  headerStat: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  headerStatLabel: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textMute },
  headerStatText: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text },
  coin: { width: 16, height: 16, borderRadius: 8, experimental_backgroundImage: 'radial-gradient(circle at 35% 35%, #fff2c2 0%, #f2c35b 45%, #a8741f 100%)' },

  body: { flexDirection: 'row', gap: 20, marginTop: 22, flex: 1 },
  doll: {
    width: 300,
    height: 532,
    borderRadius: 16,
    overflow: 'hidden',
    backgroundColor: colors.panelSolid,
    borderWidth: 1,
    borderColor: colors.line,
  },
  dollArt: { position: 'absolute', left: 0, right: 0, top: 30, height: 360, opacity: 0.85 },
  dollFade: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(18,26,46,0.2) 0%, rgba(18,26,46,0) 40%, rgba(18,26,46,1) 75%)' },
  equip: { position: 'absolute', alignItems: 'center' },
  equipBox: {
    width: 60,
    height: 60,
    borderRadius: 12,
    borderWidth: 2,
    backgroundColor: 'rgba(8,12,22,0.8)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  equipLabel: { fontFamily: font, fontWeight: '600', fontSize: 10, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textDim, marginTop: 5 },
  power: { position: 'absolute', left: 24, right: 24, bottom: 24 },
  powerLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute },
  powerValue: { fontFamily: font, fontWeight: '700', fontSize: 36, color: colors.goldLight, marginTop: 2, textShadowColor: 'rgba(242,195,91,0.5)', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 14 },
  powerBar: { height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.08)', marginTop: 10, overflow: 'hidden' },
  powerFill: { width: '72%', height: '100%', borderRadius: 3, experimental_backgroundImage: 'linear-gradient(90deg, #a8741f, #ffe29a)' },

  gridPanel: { width: 522, height: 532, padding: 20, borderRadius: 16, backgroundColor: 'rgba(14,20,36,0.7)', borderWidth: 1, borderColor: colors.line },
  filters: { flexDirection: 'row', gap: 8, marginBottom: 18, flexWrap: 'wrap' },
  filter: { paddingHorizontal: 12, paddingVertical: 6, borderRadius: 14, borderWidth: 1, borderColor: colors.lineStrong },
  filterActive: { backgroundColor: colors.gold, borderColor: colors.gold },
  filterText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  slot: {
    width: 71,
    height: 71,
    borderRadius: 10,
    borderWidth: 1.5,
    backgroundColor: 'rgba(10,14,26,0.9)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  slotEmpty: { borderColor: 'rgba(148,170,255,0.10)', backgroundColor: 'rgba(10,14,26,0.45)' },
  slotSelected: {
    borderWidth: 2,
    borderColor: '#fff3cf',
    transform: [{ scale: 1.06 }],
    boxShadow: '0 0 0 3px rgba(255,178,63,0.35), 0 0 24px rgba(255,178,63,0.75), inset 0 0 18px rgba(255,178,63,0.5)',
    zIndex: 2,
  },
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
    borderColor: 'rgba(255,178,63,0.55)',
    borderTopWidth: 3,
    borderTopColor: rarity.legendary.color,
    boxShadow: '0 24px 60px rgba(0,0,0,0.7), 0 0 30px rgba(255,178,63,0.18)',
  },
  tipHeader: { flexDirection: 'row', gap: 12, alignItems: 'center' },
  tipIconBox: {
    width: 68,
    height: 68,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 1.5,
    borderColor: rarity.legendary.color,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 40%, rgba(255,178,63,0.45) 0%, rgba(40,20,4,0.9) 75%)',
  },
  tipName: { fontFamily: font, fontWeight: '700', fontSize: 18, lineHeight: 22, color: rarity.legendary.color },
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
  unique: { fontFamily: font, fontSize: 13, lineHeight: 19, color: '#ffd9a8' },
  flavor: { fontFamily: font, fontStyle: 'italic', fontSize: 12, lineHeight: 18, color: colors.textMute, marginTop: 10 },
  tipFooter: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 14 },
  reqText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim },
  coinSmall: { width: 12, height: 12, borderRadius: 6, backgroundColor: colors.gold },
  price: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.goldLight },

  footer: { position: 'absolute', left: 40, bottom: 22, flexDirection: 'row', gap: 22 },
  hint: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  hintKey: { width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center' },
  hintKeyText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#0b0f1a' },
  hintText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },
});
