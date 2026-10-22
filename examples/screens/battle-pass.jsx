// Store / battle pass: season header with progress, featured offer cards
// (discount ribbon, owned, locked states) and a reward tier track.
import React from 'react';
import { Image, ImageBackground, StyleSheet, Text, View } from 'react-native';
import { colors, font, rarity } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Battle pass / store' };

const OFFERS = [
  { art: require('../assets/card-ember.png'), icon: require('../assets/item-axe.png'), name: 'Cinderforged Bundle', tier: 'legendary', price: '1,400', old: '2,000', discount: '-30%', tag: 'Featured' },
  { art: require('../assets/card-tide.png'), icon: require('../assets/item-potion-mana.png'), name: 'Tidecaller Elixirs', tier: 'rare', price: '450', tag: 'Daily' },
  { art: require('../assets/card-verdant.png'), icon: require('../assets/item-bow.png'), name: 'Verdant Longbow', tier: 'epic', owned: true },
  { art: require('../assets/card-void.png'), icon: require('../assets/item-gem.png'), name: 'Voidheart Relic', tier: 'epic', price: '900', locked: 'Reach tier 50' },
];

const TIERS = [
  { n: 35, free: 'potion-health', prem: 'scroll', claimed: true },
  { n: 36, prem: 'ring', claimed: true },
  { n: 37, free: 'gem', prem: 'helmet', current: true },
  { n: 38, prem: 'boots' },
  { n: 39, free: 'potion-mana', prem: 'amulet' },
  { n: 40, prem: 'sword', milestone: true },
  { n: 41, free: 'scroll', prem: 'shield' },
  { n: 42, prem: 'gem' },
];

const ICONS = {
  'potion-health': require('../assets/item-potion-health.png'),
  'potion-mana': require('../assets/item-potion-mana.png'),
  scroll: require('../assets/item-scroll.png'),
  ring: require('../assets/item-ring.png'),
  gem: require('../assets/item-gem.png'),
  helmet: require('../assets/item-helmet.png'),
  boots: require('../assets/item-boots.png'),
  amulet: require('../assets/item-amulet.png'),
  sword: require('../assets/item-sword.png'),
  shield: require('../assets/item-shield.png'),
};

function OfferCard({ art, icon, name, tier, price, old, discount, tag, owned, locked }) {
  const r = rarity[tier];
  return (
    <View style={[styles.card, { borderColor: r.color, boxShadow: `0 16px 36px rgba(0,0,0,0.55), 0 0 22px ${r.glow}` }]}>
      <ImageBackground source={art} style={styles.cardArt} resizeMode="cover">
        <View style={styles.artFade} />
        <Image source={icon} style={styles.cardIcon} />
        {tag ? (
          <View style={[styles.tag, { backgroundColor: tag === 'Daily' ? colors.cyan : colors.gold }]}>
            <Text style={styles.tagText}>{tag}</Text>
          </View>
        ) : null}
        {discount ? (
          <View style={styles.ribbonWrap}>
            <View style={styles.ribbon}>
              <Text style={styles.ribbonText}>{discount}</Text>
            </View>
          </View>
        ) : null}
      </ImageBackground>
      <View style={styles.cardBody}>
        <Text style={[styles.cardRarity, { color: r.color }]}>{r.label}</Text>
        <Text style={styles.cardName} numberOfLines={1}>{name}</Text>
        {owned ? (
          <View style={[styles.priceBtn, styles.ownedBtn]}>
            <View style={styles.tick} />
            <Text style={[styles.priceText, { color: colors.green }]}>Owned</Text>
          </View>
        ) : (
          <View style={styles.priceBtn}>
            <View style={styles.gemSmall} />
            <Text style={styles.priceText}>{price}</Text>
            {old ? <Text style={styles.oldPrice}>{old}</Text> : null}
          </View>
        )}
      </View>
      {locked ? (
        <View style={styles.lockOverlay}>
          <View style={styles.lockBody}>
            <View style={styles.lockShackle} />
          </View>
          <Text style={styles.lockText}>{locked}</Text>
        </View>
      ) : null}
    </View>
  );
}

function Reward({ name, premium, claimed }) {
  if (!name) return <View style={[styles.reward, styles.rewardEmpty]} />;
  return (
    <View style={[styles.reward, premium && styles.rewardPremium, claimed && { opacity: 0.45 }]}>
      <Image source={ICONS[name]} style={styles.rewardIcon} />
      {claimed ? (
        <View style={styles.claimed}>
          <View style={styles.tick} />
        </View>
      ) : null}
    </View>
  );
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.header}>
        <View>
          <Text style={styles.kicker}>Season 4 · Emberfall</Text>
          <Text style={styles.title}>Battle Pass</Text>
        </View>
        <View style={styles.premium}>
          <Text style={styles.premiumText}>Premium</Text>
        </View>
        <View style={{ flex: 1 }} />
        <View style={styles.tierBox}>
          <Text style={styles.tierLabel}>Tier</Text>
          <Text style={styles.tierNum}>37</Text>
          <Text style={styles.tierMax}>/ 100</Text>
        </View>
        <View style={styles.xpBox}>
          <View style={styles.xpTop}>
            <Text style={styles.xpText}>6,420 / 10,000 XP</Text>
            <Text style={styles.timer}>Ends in 12d 4h</Text>
          </View>
          <View style={styles.xpTrack}>
            <View style={styles.xpFill} />
          </View>
        </View>
      </View>

      <View style={styles.sectionRow}>
        <Text style={styles.section}>Featured Offers</Text>
        <Text style={styles.refresh}>Refreshes in 06:41:12</Text>
      </View>
      <View style={styles.cards}>
        {OFFERS.map((o) => (
          <OfferCard key={o.name} {...o} />
        ))}
      </View>

      <View style={styles.sectionRow}>
        <Text style={styles.section}>Rewards Track</Text>
      </View>
      <View style={styles.track}>
        <View style={styles.trackLabels}>
          <Text style={styles.trackLabel}>Free</Text>
          <Text style={[styles.trackLabel, { color: colors.gold }]}>Premium</Text>
        </View>
        {TIERS.map((t) => (
          <View key={t.n} style={[styles.tierCol, t.current && styles.tierColCurrent]}>
            <Reward name={t.free} claimed={t.claimed} />
            <Reward name={t.prem} premium claimed={t.claimed} />
            <View style={[styles.tierNode, t.claimed && styles.tierNodeDone, t.current && styles.tierNodeCurrent, t.milestone && styles.tierNodeMilestone]}>
              <Text style={[styles.tierNodeText, (t.current || t.claimed) && { color: '#1a1206' }]}>{t.n}</Text>
            </View>
          </View>
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    paddingHorizontal: 48,
    paddingTop: 30,
    backgroundColor: '#0a0712',
    experimental_backgroundImage:
      'radial-gradient(circle at 80% -10%, rgba(255,122,58,0.28) 0%, rgba(10,7,18,0) 55%), linear-gradient(180deg, #120c1e 0%, #07060d 100%)',
  },
  header: { flexDirection: 'row', alignItems: 'center', gap: 18 },
  kicker: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 2.4, textTransform: 'uppercase', color: colors.orange },
  title: { fontFamily: font, fontWeight: '700', fontSize: 34, color: '#fff', marginTop: 2 },
  premium: {
    marginTop: 18,
    paddingHorizontal: 10,
    paddingVertical: 4,
    borderRadius: 5,
    experimental_backgroundImage: 'linear-gradient(90deg, #ffe29a, #f2a93b)',
    boxShadow: '0 0 14px rgba(242,169,59,0.55)',
  },
  premiumText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: '#2a1500' },
  tierBox: { flexDirection: 'row', alignItems: 'baseline', gap: 6 },
  tierLabel: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textMute, textTransform: 'uppercase', letterSpacing: 1.5 },
  tierNum: { fontFamily: font, fontWeight: '700', fontSize: 40, color: colors.goldLight },
  tierMax: { fontFamily: font, fontWeight: '600', fontSize: 16, color: colors.textMute },
  xpBox: { width: 300 },
  xpTop: { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 6 },
  xpText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.text },
  timer: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.orange },
  xpTrack: { height: 10, borderRadius: 5, backgroundColor: 'rgba(255,255,255,0.08)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.08)', overflow: 'hidden' },
  xpFill: { width: '64.2%', height: '100%', borderRadius: 5, experimental_backgroundImage: 'linear-gradient(90deg, #ff7a3a 0%, #ffd36b 100%)', boxShadow: '0 0 10px rgba(255,160,80,0.8)' },

  sectionRow: { flexDirection: 'row', alignItems: 'baseline', justifyContent: 'space-between', marginTop: 26, marginBottom: 12 },
  section: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 2, textTransform: 'uppercase', color: colors.textDim },
  refresh: { fontFamily: font, fontSize: 12, color: colors.textMute },

  cards: { flexDirection: 'row', gap: 20 },
  card: { flex: 1, borderRadius: 14, borderWidth: 1.5, overflow: 'hidden', backgroundColor: '#100c18' },
  cardArt: { height: 150, alignItems: 'center', justifyContent: 'center' },
  artFade: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(16,12,24,0) 50%, rgba(16,12,24,1) 100%)' },
  cardIcon: { width: 96, height: 96, transform: [{ rotate: '-8deg' }] },
  tag: { position: 'absolute', left: 10, top: 10, paddingHorizontal: 8, paddingVertical: 3, borderRadius: 4 },
  tagText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1, textTransform: 'uppercase', color: '#0b0f1a' },
  ribbonWrap: { position: 'absolute', right: 0, top: 0, width: 90, height: 90, overflow: 'hidden' },
  ribbon: {
    position: 'absolute',
    right: -34,
    top: 18,
    width: 130,
    height: 24,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: colors.red,
    transform: [{ rotate: '45deg' }],
    boxShadow: '0 2px 6px rgba(0,0,0,0.5)',
  },
  ribbonText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#fff' },
  cardBody: { paddingHorizontal: 14, paddingBottom: 14, paddingTop: 4 },
  cardRarity: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase' },
  cardName: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.text, marginTop: 4 },
  priceBtn: {
    marginTop: 12,
    height: 36,
    borderRadius: 8,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 7,
    backgroundColor: 'rgba(255,255,255,0.06)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.12)',
  },
  ownedBtn: { borderColor: 'rgba(74,222,128,0.5)', backgroundColor: 'rgba(74,222,128,0.10)' },
  gemSmall: { width: 10, height: 10, borderRadius: 2, transform: [{ rotate: '45deg' }], experimental_backgroundImage: 'linear-gradient(135deg, #d6b8ff, #8a4dff)' },
  priceText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: '#fff' },
  oldPrice: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute, textDecorationLine: 'line-through' },
  tick: { width: 6, height: 11, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.green, transform: [{ rotate: '45deg' }], marginTop: -3 },
  lockOverlay: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(6,4,12,0.72)', alignItems: 'center', justifyContent: 'center', gap: 10 },
  lockBody: { width: 26, height: 20, borderRadius: 4, backgroundColor: colors.textDim, marginTop: 10 },
  lockShackle: { position: 'absolute', left: 5, top: -12, width: 16, height: 16, borderWidth: 3, borderBottomWidth: 0, borderColor: colors.textDim, borderTopLeftRadius: 8, borderTopRightRadius: 8 },
  lockText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  track: {
    flexDirection: 'row',
    gap: 10,
    padding: 14,
    borderRadius: 14,
    backgroundColor: 'rgba(255,255,255,0.03)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.07)',
  },
  trackLabels: { width: 70, paddingTop: 28, gap: 53 },
  trackLabel: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textDim },
  tierCol: { flex: 1, alignItems: 'center', gap: 8, paddingVertical: 6, borderRadius: 10 },
  tierColCurrent: { backgroundColor: 'rgba(242,195,91,0.10)', borderWidth: 1, borderColor: 'rgba(242,195,91,0.45)' },
  reward: {
    width: 58,
    height: 58,
    borderRadius: 10,
    backgroundColor: 'rgba(255,255,255,0.05)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.10)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  rewardEmpty: { backgroundColor: 'transparent', borderColor: 'rgba(255,255,255,0.05)' },
  rewardPremium: { borderColor: 'rgba(242,195,91,0.55)', experimental_backgroundImage: 'linear-gradient(180deg, rgba(242,195,91,0.20), rgba(242,195,91,0.02))' },
  rewardIcon: { width: 40, height: 40 },
  claimed: { position: 'absolute', right: -5, top: -5, width: 18, height: 18, borderRadius: 9, backgroundColor: '#0c2416', borderWidth: 1, borderColor: colors.green, alignItems: 'center', justifyContent: 'center' },
  tierNode: { width: 30, height: 30, borderRadius: 15, alignItems: 'center', justifyContent: 'center', backgroundColor: '#1b1626', borderWidth: 1, borderColor: 'rgba(255,255,255,0.15)' },
  tierNodeDone: { backgroundColor: colors.goldDeep, borderColor: colors.goldDeep },
  tierNodeCurrent: { backgroundColor: colors.gold, borderColor: '#fff3cf', boxShadow: '0 0 14px rgba(242,195,91,0.9)' },
  tierNodeMilestone: { borderColor: colors.purple, borderWidth: 2, boxShadow: '0 0 12px rgba(178,107,255,0.6)' },
  tierNodeText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: colors.textDim },
});
