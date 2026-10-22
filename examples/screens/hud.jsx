// In-game HUD: player frame with resource bars, buffs, boss bar, minimap,
// quest tracker, ability bar with cooldowns, ammo counter, crosshair.
import React from 'react';
import { Image, ImageBackground, StyleSheet, Text, View } from 'react-native';
import { colors, font } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'HUD' };

function Bar({ value, max, fill, trail, height = 14, ticks = 0, label }) {
  const pct = `${(value / max) * 100}%`;
  return (
    <View style={[styles.barTrack, { height }]}>
      {trail ? <View style={[styles.barFill, { width: `${(trail / max) * 100}%`, backgroundColor: 'rgba(255,255,255,0.35)' }]} /> : null}
      <View style={[styles.barFill, { width: pct, experimental_backgroundImage: fill }]}>
        <View style={styles.barSheen} />
      </View>
      {Array.from({ length: ticks }, (_, i) => (
        <View key={i} style={[styles.tick, { left: `${((i + 1) / (ticks + 1)) * 100}%` }]} />
      ))}
      {label ? (
        <View style={styles.barLabelWrap}>
          <Text style={styles.barLabel}>{label}</Text>
        </View>
      ) : null}
    </View>
  );
}

const BUFFS = [
  { icon: require('../assets/buff-haste.png'), time: '12s' },
  { icon: require('../assets/buff-ward.png'), time: '45s', stacks: 3 },
  { icon: require('../assets/buff-regen.png'), time: '8s' },
  { icon: require('../assets/buff-poison.png'), time: '4s', debuff: true },
];

const ABILITIES = [
  { key: '1', icon: require('../assets/item-sword.png'), ready: true },
  { key: '2', icon: require('../assets/item-shield.png'), cooldown: 0.65, secs: '7' },
  { key: '3', icon: require('../assets/item-bow.png') },
  { key: '4', icon: require('../assets/item-axe.png'), cooldown: 0.3, secs: '3' },
  { key: 'Q', icon: require('../assets/item-potion-health.png'), count: 5 },
  { key: 'R', icon: require('../assets/item-gem.png'), ultimate: true },
];

export default function App() {
  return (
    <ImageBackground source={require('../assets/bg-loading.jpg')} style={styles.root} resizeMode="cover">
      <View style={styles.vignette} />

      {/* Player frame */}
      <View style={styles.player}>
        <View style={styles.portraitRing}>
          <Image source={require('../assets/portrait-mage.png')} style={styles.portrait} />
          <View style={styles.levelBadge}>
            <Text style={styles.levelText}>42</Text>
          </View>
        </View>
        <View style={styles.playerBars}>
          <View style={styles.nameRow}>
            <Text style={styles.playerName}>Seren Ashvale</Text>
            <Text style={styles.playerClass}>Stormcaller</Text>
          </View>
          <Bar value={1240} max={1600} trail={1390} height={18} ticks={3} label="1,240 / 1,600" fill="linear-gradient(180deg, #ff7a8c 0%, #e0344f 55%, #a3132c 100%)" />
          <View style={{ height: 5 }} />
          <Bar value={610} max={900} height={12} label="610 / 900" fill="linear-gradient(180deg, #7cc4ff 0%, #2f7bf0 60%, #1a4fb0 100%)" />
          <View style={{ height: 5 }} />
          <Bar value={72} max={100} height={5} fill="linear-gradient(90deg, #ffd36b, #f2a93b)" />
          <View style={styles.buffs}>
            {BUFFS.map((b, i) => (
              <View key={i} style={[styles.buff, b.debuff && styles.debuff]}>
                <Image source={b.icon} style={styles.buffIcon} />
                {b.stacks ? (
                  <View style={styles.stack}>
                    <Text style={styles.stackText}>{b.stacks}</Text>
                  </View>
                ) : null}
                <Text style={[styles.buffTime, b.debuff && { color: '#ff9aa8' }]}>{b.time}</Text>
              </View>
            ))}
          </View>
        </View>
      </View>

      {/* Boss */}
      <View style={styles.boss}>
        <View style={styles.bossHeader}>
          <View style={styles.skullDot} />
          <Text style={styles.bossName}>Vharos, the Cinder Wyrm</Text>
          <Text style={styles.bossPhase}>Phase II</Text>
        </View>
        <View style={styles.bossTrack}>
          <View style={[styles.bossFill, { width: '58%' }]} />
          <View style={[styles.phaseMark, { left: '66%' }]} />
          <View style={[styles.phaseMark, { left: '33%' }]} />
        </View>
        <Text style={styles.bossPct}>58%</Text>
      </View>

      {/* Minimap */}
      <View style={styles.mapWrap}>
        <View style={styles.mapFrame}>
          <View style={styles.mapClip}>
            <Image source={require('../assets/minimap.png')} style={styles.mapImage} />
            <View style={styles.fov} />
            <View style={styles.playerArrow} />
            <View style={[styles.ping, { left: 52, top: 40, backgroundColor: colors.gold }]} />
            <View style={[styles.ping, { left: 128, top: 112, backgroundColor: colors.red }]} />
            <View style={[styles.ping, { left: 40, top: 120, backgroundColor: colors.cyan }]} />
          </View>
          <View style={styles.north}>
            <Text style={styles.northText}>N</Text>
          </View>
        </View>
        <Text style={styles.zone}>Ashen Reach</Text>
        <Text style={styles.zoneSub}>482, 1290 · 21:40</Text>
      </View>

      {/* Quest tracker */}
      <View style={styles.quests}>
        <Text style={styles.questKicker}>Main Quest</Text>
        <Text style={styles.questTitle}>Embers of the Old King</Text>
        <View style={styles.objective}>
          <View style={[styles.check, styles.checkDone]} />
          <Text style={[styles.objText, styles.objDone]}>Reach the Cinder Gate</Text>
        </View>
        <View style={styles.objective}>
          <View style={styles.check} />
          <Text style={styles.objText}>Extinguish the braziers</Text>
          <Text style={styles.objCount}>2/4</Text>
        </View>
        <View style={styles.objective}>
          <View style={styles.check} />
          <Text style={styles.objText} numberOfLines={1}>Defeat Vharos before the tide of flame consumes the bridge</Text>
        </View>
      </View>

      {/* Crosshair */}
      <View style={styles.crosshair}>
        <View style={[styles.chLine, { width: 2, height: 10, left: 13, top: 0 }]} />
        <View style={[styles.chLine, { width: 2, height: 10, left: 13, bottom: 0 }]} />
        <View style={[styles.chLine, { width: 10, height: 2, top: 13, left: 0 }]} />
        <View style={[styles.chLine, { width: 10, height: 2, top: 13, right: 0 }]} />
        <View style={styles.chDot} />
      </View>

      {/* Ability bar */}
      <View style={styles.abilities}>
        {ABILITIES.map((a) => (
          <View key={a.key} style={[styles.slot, a.ready && styles.slotReady, a.ultimate && styles.slotUlt]}>
            <Image source={a.icon} style={styles.slotIcon} />
            {a.cooldown ? (
              <View style={[styles.cooldown, { height: `${a.cooldown * 100}%` }]}>
                <Text style={styles.cdText}>{a.secs}</Text>
              </View>
            ) : null}
            {a.count ? <Text style={styles.count}>{a.count}</Text> : null}
            <View style={styles.keycap}>
              <Text style={styles.keyText}>{a.key}</Text>
            </View>
          </View>
        ))}
      </View>
      <View style={styles.xp}>
        <View style={[styles.xpFill, { width: '64%' }]} />
      </View>

      {/* Ammo */}
      <View style={styles.ammo}>
        <Text style={styles.weapon}>Thornspitter Mk II</Text>
        <View style={styles.ammoRow}>
          <Text style={styles.ammoMag}>24</Text>
          <Text style={styles.ammoReserve}>/ 180</Text>
        </View>
        <View style={styles.rounds}>
          {Array.from({ length: 30 }, (_, i) => (
            <View key={i} style={[styles.round, i >= 24 && styles.roundSpent]} />
          ))}
        </View>
        <Text style={styles.fireMode}>Burst · R to reload</Text>
      </View>
    </ImageBackground>
  );
}

const textShadow = { textShadowColor: 'rgba(0,0,0,0.85)', textShadowOffset: { width: 0, height: 1 }, textShadowRadius: 3 };

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0c1a20' },
  vignette: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'radial-gradient(ellipse at center, rgba(0,0,0,0) 45%, rgba(0,0,0,0.65) 100%)',
  },

  player: { position: 'absolute', left: 28, top: 26, flexDirection: 'row', alignItems: 'flex-start', gap: 14 },
  portraitRing: {
    width: 84,
    height: 84,
    borderRadius: 42,
    padding: 3,
    experimental_backgroundImage: 'linear-gradient(160deg, #ffe29a 0%, #c9902d 50%, #6e4512 100%)',
    boxShadow: '0 6px 18px rgba(0,0,0,0.6), 0 0 18px rgba(242,195,91,0.35)',
  },
  portrait: { width: 78, height: 78, borderRadius: 39 },
  levelBadge: {
    position: 'absolute',
    right: -4,
    bottom: -2,
    minWidth: 30,
    height: 24,
    paddingHorizontal: 6,
    borderRadius: 12,
    backgroundColor: '#1a1206',
    borderWidth: 2,
    borderColor: colors.gold,
    alignItems: 'center',
    justifyContent: 'center',
  },
  levelText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.goldLight },
  playerBars: { width: 300, paddingTop: 4 },
  nameRow: { flexDirection: 'row', alignItems: 'baseline', gap: 8, marginBottom: 6 },
  playerName: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.text, ...textShadow },
  playerClass: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.cyan, ...textShadow },

  barTrack: {
    borderRadius: 4,
    overflow: 'hidden',
    backgroundColor: 'rgba(6,8,14,0.78)',
    borderWidth: 1,
    borderColor: 'rgba(0,0,0,0.9)',
    boxShadow: '0 2px 6px rgba(0,0,0,0.5)',
  },
  barFill: { position: 'absolute', left: 0, top: 0, bottom: 0 },
  barSheen: { position: 'absolute', left: 0, right: 0, top: 0, height: '45%', backgroundColor: 'rgba(255,255,255,0.18)' },
  tick: { position: 'absolute', top: 0, bottom: 0, width: 1, backgroundColor: 'rgba(0,0,0,0.45)' },
  barLabelWrap: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center' },
  barLabel: { fontFamily: font, fontWeight: '700', fontSize: 10, color: '#fff', letterSpacing: 0.5, ...textShadow },

  buffs: { flexDirection: 'row', gap: 6, marginTop: 10 },
  buff: { width: 34, alignItems: 'center' },
  buffIcon: { width: 30, height: 30, borderRadius: 7, borderWidth: 1, borderColor: 'rgba(255,255,255,0.35)' },
  debuff: {},
  stack: {
    position: 'absolute',
    right: -2,
    top: -4,
    width: 15,
    height: 15,
    borderRadius: 8,
    backgroundColor: '#10162a',
    borderWidth: 1,
    borderColor: colors.cyan,
    alignItems: 'center',
    justifyContent: 'center',
  },
  stackText: { fontFamily: font, fontWeight: '700', fontSize: 9, color: colors.cyan },
  buffTime: { fontFamily: font, fontWeight: '600', fontSize: 10, color: colors.text, marginTop: 2, ...textShadow },

  boss: { position: 'absolute', top: 26, left: 450, width: 380, alignItems: 'center' },
  bossHeader: { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 6 },
  skullDot: { width: 8, height: 8, borderRadius: 2, backgroundColor: colors.red, transform: [{ rotate: '45deg' }], boxShadow: '0 0 8px rgba(255,77,106,0.9)' },
  bossName: { fontFamily: font, fontWeight: '700', fontSize: 15, letterSpacing: 1.2, textTransform: 'uppercase', color: '#ffe3d6', ...textShadow },
  bossPhase: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.orange, ...textShadow },
  bossTrack: {
    width: '100%',
    height: 12,
    borderRadius: 2,
    backgroundColor: 'rgba(10,4,6,0.85)',
    borderWidth: 1,
    borderColor: 'rgba(255,140,90,0.45)',
    overflow: 'hidden',
    boxShadow: '0 0 16px rgba(255,90,60,0.35)',
  },
  bossFill: {
    position: 'absolute',
    left: 0,
    top: 0,
    bottom: 0,
    experimental_backgroundImage: 'linear-gradient(90deg, #7a0f1e 0%, #e0344f 70%, #ff8a5c 100%)',
  },
  phaseMark: { position: 'absolute', top: 0, bottom: 0, width: 2, backgroundColor: 'rgba(255,230,200,0.7)' },
  bossPct: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.textDim, marginTop: 4, ...textShadow },

  mapWrap: { position: 'absolute', right: 28, top: 22, alignItems: 'center' },
  mapFrame: {
    width: 188,
    height: 188,
    borderRadius: 94,
    padding: 5,
    experimental_backgroundImage: 'linear-gradient(180deg, #3a4462 0%, #151b2e 100%)',
    boxShadow: '0 10px 30px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.15)',
  },
  mapClip: { flex: 1, borderRadius: 89, overflow: 'hidden', borderWidth: 2, borderColor: 'rgba(0,0,0,0.7)' },
  mapImage: { position: 'absolute', left: -40, top: -30, width: 256, height: 256, opacity: 0.92 },
  fov: {
    position: 'absolute',
    left: 49,
    top: 30,
    width: 76,
    height: 76,
    borderTopLeftRadius: 76,
    experimental_backgroundImage: 'linear-gradient(135deg, rgba(255,255,255,0.28), rgba(255,255,255,0))',
    transform: [{ rotate: '45deg' }],
  },
  playerArrow: {
    position: 'absolute',
    left: 82,
    top: 82,
    width: 12,
    height: 12,
    backgroundColor: '#fff',
    borderTopLeftRadius: 2,
    transform: [{ rotate: '45deg' }],
    boxShadow: '0 0 6px rgba(255,255,255,0.9)',
  },
  ping: { position: 'absolute', width: 9, height: 9, borderRadius: 5, borderWidth: 2, borderColor: 'rgba(0,0,0,0.6)' },
  north: {
    position: 'absolute',
    top: -4,
    left: 82,
    width: 24,
    height: 24,
    borderRadius: 12,
    backgroundColor: '#151b2e',
    borderWidth: 1,
    borderColor: colors.gold,
    alignItems: 'center',
    justifyContent: 'center',
  },
  northText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: colors.gold },
  zone: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text, marginTop: 8, letterSpacing: 1, textTransform: 'uppercase', ...textShadow },
  zoneSub: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textDim, marginTop: 2, ...textShadow },

  quests: {
    position: 'absolute',
    right: 28,
    top: 270,
    width: 250,
    paddingVertical: 12,
    paddingHorizontal: 14,
    borderLeftWidth: 3,
    borderLeftColor: colors.gold,
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(8,12,24,0.75) 0%, rgba(8,12,24,0.2) 100%)',
    borderTopRightRadius: 8,
    borderBottomRightRadius: 8,
  },
  questKicker: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.gold },
  questTitle: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text, marginTop: 3, marginBottom: 8, ...textShadow },
  objective: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 5 },
  check: { width: 10, height: 10, borderRadius: 2, borderWidth: 1.5, borderColor: colors.textDim },
  checkDone: { backgroundColor: colors.green, borderColor: colors.green },
  objText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.text, ...textShadow },
  objDone: { color: colors.textMute, textDecorationLine: 'line-through' },
  objCount: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.gold },

  crosshair: { position: 'absolute', left: 626, top: 346, width: 28, height: 28 },
  chLine: { position: 'absolute', backgroundColor: 'rgba(255,255,255,0.9)', borderRadius: 1, boxShadow: '0 0 3px rgba(0,0,0,0.8)' },
  chDot: { position: 'absolute', left: 12, top: 12, width: 4, height: 4, borderRadius: 2, backgroundColor: colors.red },

  abilities: { position: 'absolute', bottom: 34, left: 0, right: 0, flexDirection: 'row', justifyContent: 'center', gap: 10 },
  slot: {
    width: 58,
    height: 58,
    borderRadius: 10,
    backgroundColor: 'rgba(10,14,26,0.85)',
    borderWidth: 2,
    borderColor: 'rgba(148,170,255,0.25)',
    overflow: 'hidden',
    alignItems: 'center',
    justifyContent: 'center',
    boxShadow: '0 6px 14px rgba(0,0,0,0.55)',
  },
  slotReady: { borderColor: colors.goldLight, boxShadow: '0 0 0 2px rgba(242,195,91,0.35), 0 0 18px rgba(242,195,91,0.6)' },
  slotUlt: {
    width: 66,
    height: 66,
    marginTop: -8,
    borderRadius: 33,
    borderColor: colors.purple,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 40%, rgba(178,107,255,0.55) 0%, rgba(30,12,60,0.95) 70%)',
    boxShadow: '0 0 22px rgba(178,107,255,0.7)',
  },
  slotIcon: { width: 42, height: 42 },
  cooldown: { position: 'absolute', left: 0, right: 0, bottom: 0, backgroundColor: 'rgba(4,6,12,0.72)', alignItems: 'center', justifyContent: 'center' },
  cdText: { fontFamily: font, fontWeight: '700', fontSize: 18, color: '#fff', ...textShadow },
  count: { position: 'absolute', right: 5, bottom: 2, fontFamily: font, fontWeight: '700', fontSize: 12, color: '#fff', ...textShadow },
  keycap: {
    position: 'absolute',
    left: 3,
    top: 3,
    minWidth: 16,
    height: 16,
    borderRadius: 4,
    paddingHorizontal: 3,
    backgroundColor: 'rgba(0,0,0,0.7)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  keyText: { fontFamily: font, fontWeight: '700', fontSize: 10, color: colors.textDim },
  xp: { position: 'absolute', left: 390, right: 390, bottom: 18, height: 4, borderRadius: 2, backgroundColor: 'rgba(0,0,0,0.6)', overflow: 'hidden' },
  xpFill: { height: '100%', experimental_backgroundImage: 'linear-gradient(90deg, #8a4dff, #c39bff)' },

  ammo: { position: 'absolute', right: 36, bottom: 30, alignItems: 'flex-end' },
  weapon: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textDim, ...textShadow },
  ammoRow: { flexDirection: 'row', alignItems: 'baseline', gap: 6, marginTop: 2 },
  ammoMag: { fontFamily: font, fontWeight: '700', fontSize: 52, color: '#fff', letterSpacing: -1, ...textShadow },
  ammoReserve: { fontFamily: font, fontWeight: '600', fontSize: 20, color: colors.textDim, ...textShadow },
  rounds: { flexDirection: 'row', gap: 2, marginTop: 2 },
  round: { width: 4, height: 12, borderRadius: 1, backgroundColor: colors.goldLight },
  roundSpent: { backgroundColor: 'rgba(255,255,255,0.18)' },
  fireMode: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textMute, marginTop: 6, ...textShadow },
});
