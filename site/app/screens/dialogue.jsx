// Dialogue (interactive). Feature spotlight: rich text and i18n.
//
// A branching conversation with Seren Ashvale. Lines are nested <Text> runs
// (italic, keyword, place-name spans) revealed by a native typewriter
// (Animated.Text reveal: no React render per character) (A / Enter /
// click skips). Choices slide in and are picked with focus (D-pad / arrows /
// mouse / 1-4), skill checks roll a d20, trust moves with your answers, the
// portrait swaps when you talk, and the conversation loops when it ends.
// The language switch (L / gamepad Y / click) re-renders everything in
// English, Arabic (right-to-left: mirrored layout, bidi text, Arabic shaping)
// or Japanese; the Noto fonts for those load on demand with Fonts.load.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Fonts, Image, ImageBackground, Pressable, StyleSheet, Text, View, useInteraction } from 'react-native';
import { colors, font } from '../../../examples/theme.js';

export const feature = {
  title: 'Rich text & i18n',
  points: [
    'Every line is nested <Text> runs (italic, keyword and place spans) typed on natively: laid out once, revealed without a re-render per character',
    'Switch to Arabic (AR) for right-to-left: the layout mirrors, bidi text flows right to left and Arabic letters join and shape',
    'Japanese (JA) too: the Noto fonts for both load on demand with Fonts.load and fall back per glyph from Inter',
    'Branching choices with d20 skill checks, a trust meter and swapping portraits',
  ],
  keys: ['Enter / A / click: skip or continue', '↑ ↓ / D-pad: pick a reply, 1–4: reply directly', 'L / Y: switch language'],
};

const MAGE = require('../../../examples/assets/portrait-mage.png');
const ROGUE = require('../../../examples/assets/portrait-rogue.png');
const BG = require('../../../examples/assets/bg-menu.jpg');

const LANGS = [
  { id: 'en', label: 'EN' },
  { id: 'ar', label: 'AR', family: 'Noto Sans Arabic', files: [require('../assets/fonts/NotoSansArabic-Regular.ttf'), require('../assets/fonts/NotoSansArabic-Bold.ttf')] },
  { id: 'ja', label: 'JA', family: 'Noto Sans JP', files: [require('../assets/fonts/NotoSansJP-Regular.ttf'), require('../assets/fonts/NotoSansJP-Bold.ttf')] },
];
const FAMILY = { en: font, ar: `${font}, Noto Sans Arabic`, ja: `${font}, Noto Sans JP` };
const fontPromises = {};
function loadLang(l) {
  if (!l.files) return Promise.resolve();
  if (!fontPromises[l.id]) fontPromises[l.id] = Promise.all([Fonts.load(l.family, l.files[0], { weight: 400 }), Fonts.load(l.family, l.files[1], { weight: 700 })]);
  return fontPromises[l.id];
}

const UI = {
  en: { you: 'You', seren: 'Seren', trust: 'Trust', quest: 'Quest updated', leave: 'Leave', locked: 'LOCKED', success: 'SUCCESS', failed: 'FAILED', skip: 'skip', cont: 'continue', over: 'start over', choose: 'Choose a reply', Persuade: 'Persuade', Intimidate: 'Intimidate', lockMsg: 'Intimidate 11 — you are not frightening enough. Yet.' },
  ar: { you: 'أنتِ', seren: 'سيرين', trust: 'الثقة', quest: 'تحديث المهمة', leave: 'مغادرة', locked: 'مقفل', success: 'نجاح', failed: 'فشل', skip: 'تخطٍّ', cont: 'متابعة', over: 'من البداية', choose: 'اختاري ردًّا', Persuade: 'إقناع', Intimidate: 'ترهيب', lockMsg: 'ترهيب 11 — لستِ مخيفة بما يكفي. بعد.' },
  ja: { you: 'あなた', seren: 'セレン', trust: '信頼', quest: 'クエスト更新', leave: '立ち去る', locked: 'ロック', success: '成功', failed: '失敗', skip: 'スキップ', cont: '続ける', over: '最初から', choose: '返答を選ぶ', Persuade: '説得', Intimidate: '威圧', lockMsg: '威圧 11 — まだ十分に恐ろしくない。今はまだ。' },
};

const SPEAKERS = {
  seren: {
    name: { en: 'Seren Ashvale', ar: 'سيرين آشفيل', ja: 'セレン・アッシュヴェイル' },
    title: { en: 'Stormcaller of the Ninth Spire', ar: 'مستدعية العواصف في البرج التاسع', ja: '第九尖塔の嵐呼び' },
    img: MAGE,
    color: colors.cyan,
    glow: 'rgba(79,209,255,0.6)',
  },
  you: {
    name: { en: 'Valkyrie', ar: 'فالكيري', ja: 'ヴァルキリー' },
    title: { en: 'Warden-errant', ar: 'حارسة جوّالة', ja: '流浪の番人' },
    img: ROGUE,
    color: colors.gold,
    glow: 'rgba(242,195,91,0.6)',
  },
};

// Lines use a tiny markup: *em*, {keyword}, ^place^.
const T = {
  gateQ: { en: 'Tell me what happened at the Cinder Gate.', ar: 'أخبريني بما حدث عند بوابة الرماد.', ja: '燼の門で何があったのか教えてくれ。' },
  persuade: { en: 'The Wardens will not hold the bridge without your fire.', ar: 'لن يصمد الحرّاس على الجسر من دون نارك.', ja: 'あなたの炎なしでは、番人たちは橋を守りきれない。' },
  threat: { en: 'Hand over the Ember Key, or I take it.', ar: 'سلّميني مفتاح الجمر، وإلا أخذته بنفسي.', ja: '残り火の鍵を渡せ。さもなくば奪うまでだ。' },
  bye: { en: 'We will speak again, Seren.', ar: 'سنتحدّث مجددًا يا سيرين.', ja: 'また話そう、セレン。' },
};
const NODES = {
  start: {
    line: {
      en: "The gate did not fall, traveller. It was *opened*. Someone carried the {Ember Key} past three Wardens and not one of them remembers a face. If the ^Old King's^ fire wakes before the tide turns, there will be no bridge left to defend.",
      ar: 'البوابة لم تسقط أيتها المسافرة. لقد *فُتحت*. أحدهم حمل {مفتاح الجمر} أمام ثلاثة من الحرّاس، ولا يتذكّر أيٌّ منهم وجهًا. إن استيقظت نار ^الملك القديم^ قبل أن ينقلب المدّ، فلن يبقى جسرٌ ندافع عنه.',
      ja: '門は落ちたのではない、旅の人。*開けられた*のよ。何者かが{残り火の鍵}を持って三人の番人の前を通り過ぎた。なのに誰一人、その顔を覚えていない。潮が変わる前に^古王^の炎が目覚めれば、守るべき橋は残らない。',
    },
    choices: [
      { text: T.gateQ, next: 'gate' },
      { text: T.persuade, check: { skill: 'Persuade', dc: 14, bonus: 4 }, pass: 'persuaded', fail: 'rebuffed' },
      { text: T.threat, check: { skill: 'Intimidate', dc: 18 }, locked: true },
      { text: T.bye, leave: true, next: 'farewell' },
    ],
  },
  gate: {
    line: {
      en: 'At the third bell the lanterns went out. When they burned again the Wardens stood at their posts, and the {Ember Key} was gone. Only ash on the stones, in the shape of a *hand*.',
      ar: 'عند الجرس الثالث انطفأت الفوانيس. وحين اشتعلت من جديد كان الحرّاس في مواقعهم، و{مفتاح الجمر} قد اختفى. لم يبقَ سوى رمادٍ على الحجارة، على هيئة *يد*.',
      ja: '三つ目の鐘で灯籠が消えた。再び灯ったとき、番人たちは持ち場に立っていたけれど、{残り火の鍵}は消えていた。石の上には灰だけ。*手*の形をした灰が。',
    },
    choices: [
      { text: { en: 'Ash? Whose magic leaves ash behind?', ar: 'رماد؟ سحرُ مَن يترك رمادًا خلفه؟', ja: '灰？灰を残す魔法とは、誰のものだ？' }, next: 'ash', trust: 5 },
      { text: T.persuade, check: { skill: 'Persuade', dc: 14, bonus: 4 }, pass: 'persuaded', fail: 'rebuffed' },
      { text: { en: 'This is not my fight.', ar: 'هذه ليست معركتي.', ja: 'これは私の戦いではない。' }, leave: true, next: 'farewell', trust: -5 },
    ],
  },
  ash: {
    line: {
      en: 'Old fire. *Older* than the Spire. Only the ^Cinder Choir^ ever sang it, and they drowned with the King. Unless the tide gave something back.',
      ar: 'نارٌ قديمة. *أقدم* من البرج. لم ينشدها إلا ^جوقة الرماد^، وقد غرقوا مع الملك. إلا إذا أعاد المدّ شيئًا.',
      ja: '古い炎よ。尖塔よりも*古い*。それを歌えたのは^燼の聖歌隊^だけ。彼らは王と共に沈んだ。潮が何かを返したのでなければ。',
    },
    choices: [
      { text: { en: 'Then we walk the causeway tonight. Together.', ar: 'إذن نسير على الممرّ الليلة. معًا.', ja: 'なら今夜、堤道を行こう。一緒に。' }, next: 'persuaded', trust: 10 },
      { text: { en: 'Drowned singers. Wonderful.', ar: 'مغنّون غرقى. رائع.', ja: '溺れた歌い手か。素晴らしい。' }, next: 'start', trust: -5 },
      { text: { en: 'I need to think about this.', ar: 'أحتاج أن أفكّر في الأمر.', ja: '少し考えさせてくれ。' }, leave: true, next: 'farewell' },
    ],
  },
  persuaded: {
    line: {
      en: '...You sound like my brother, before the bridge. *Very well*. My fire is yours until the {Ember Key} is found. Meet me at the ^Sunken Causeway^ at dusk.',
      ar: '...تتكلّمين مثل أخي، قبل الجسر. *ليكن*. ناري لكِ حتى نجد {مفتاح الجمر}. قابليني عند ^الممرّ الغارق^ عند الغسق.',
      ja: '……橋の前の兄のような口ぶりね。*いいでしょう*。{残り火の鍵}が見つかるまで、私の炎はあなたのもの。黄昏に^沈んだ堤道^で会いましょう。',
    },
    trust: 15,
    quest: { en: 'The Drowned Key', ar: 'المفتاح الغارق', ja: '沈んだ鍵' },
    choices: [{ text: { en: 'Dusk, then. Bring the storm.', ar: 'عند الغسق إذن. أحضري العاصفة.', ja: 'では黄昏に。嵐を連れてきてくれ。' }, leave: true, next: 'end' }],
  },
  rebuffed: {
    line: {
      en: 'Do not presume to tell a Stormcaller where her fire is needed. Ask me again when you have *earned* it.',
      ar: 'لا تتجرّئي على إخبار مستدعية العواصف أين تُحتاج نارها. اسأليني مجددًا حين *تستحقّين* ذلك.',
      ja: '嵐呼びに、炎の使い道を指図しないで。それに*値する*者になったら、もう一度頼みなさい。',
    },
    trust: -10,
    choices: [
      { text: { en: 'Fair. Tell me about the gate, then.', ar: 'حسنًا. حدّثيني عن البوابة إذن.', ja: 'わかった。では門のことを話してくれ。' }, next: 'gate' },
      { text: T.bye, leave: true, next: 'farewell' },
    ],
  },
  farewell: { line: { en: 'Go carefully, traveller. *The tide listens.*', ar: 'اذهبي بحذر أيتها المسافرة. *المدّ يُصغي.*', ja: '気をつけて行きなさい、旅の人。*潮は聞いている。*' }, end: true },
  end: { line: { en: 'Dusk. Do not be late, {Warden}. The sea is never late.', ar: 'عند الغسق. لا تتأخّري أيتها {الحارسة}. البحر لا يتأخّر أبدًا.', ja: '黄昏に。遅れないで、{番人}。海は決して遅れない。' }, end: true },
};

const MARK = { '*': 'em', '{': 'key', '}': 'key', '^': 'place' };
/** "a *b* {c}" -> ['a ', ['em', 'b'], ' ', ['key', 'c']] */
function parse(src) {
  const out = [];
  let cur = '';
  let style = null;
  for (const ch of src) {
    const m = MARK[ch];
    if (m && (style === null || style === m)) {
      if (cur) out.push(style ? [style, cur] : cur);
      cur = '';
      style = style ? null : m;
    } else cur += ch;
  }
  if (cur) out.push(style ? [style, cur] : cur);
  return out;
}

const spanStyle = (k) => (k === 'em' ? styles.em : k === 'key' ? styles.keyword : k === 'place' ? styles.place : null);
const lengthOf = (spans) => spans.reduce((n, s) => n + (typeof s === 'string' ? s.length : s[1].length), 0);

/** A span list as nested Text runs, laid out whole and revealed natively:
 * `reveal` (0..1 of the line's graphemes) only changes what paints. */
const RichLine = memo(function RichLine({ spans, reveal, family }) {
  return (
    <Animated.Text style={[styles.line, { fontFamily: family }]} reveal={reveal} revealFraction revealFade={2}>
      {spans.map((s, i) => (typeof s === 'string' ? s : <Text key={i} style={spanStyle(s[0])}>{s[1]}</Text>))}
    </Animated.Text>
  );
});

/** Typewriter: a native timing of the reveal (Animated.Text reveal), so the
 * line types on without a React render or a relayout per character and
 * keeps going while JS is busy; each character fades in over two. Time
 * based: slow frames never slow the text down. Pausing (inactive screen)
 * stops it where it is; a language switch goes on from the same fraction. */
const CPS = 55; // characters per second
function Typewriter({ spans, skip, active, onDone, family }) {
  const total = useMemo(() => lengthOf(spans), [spans]);
  const shown = useMemo(() => new Animated.Value(0, { useNativeDriver: true }), []);
  const done = useRef(false);
  const finish = useRef(null);
  finish.current = () => {
    if (done.current) return;
    done.current = true;
    onDone();
  };
  useEffect(() => {
    if (!active || done.current) return undefined;
    const from = Math.min(1, shown.__getValue());
    const anim = Animated.timing(shown, { toValue: 1, duration: ((1 - from) * total * 1000) / CPS, easing: Easing.linear, useNativeDriver: true });
    anim.start(({ finished }) => finished && finish.current());
    return () => anim.stop();
  }, [active, total]);
  useEffect(() => {
    if (skip && !done.current) {
      shown.setValue(1);
      finish.current();
    }
  }, [skip]);
  return <RichLine spans={spans} reveal={shown} family={family} />;
}

function useLoop(active, make) {
  useEffect(() => {
    if (!active) return undefined;
    const anim = make();
    anim.start();
    return () => anim.stop();
  }, [active]);
}

const Choice = memo(function Choice({ c, i, index, onPick, focusRef, shake, lang, rtl, onFocusIn }) {
  const hot = useRef(new Animated.Value(0)).current;
  const enter = useRef(new Animated.Value(0)).current;
  const jolt = useRef(new Animated.Value(0)).current;
  // Hover / focus / press come from the runtime (state styles and the
  // interaction's pressed value): no re-render per input change.
  const ix = useInteraction();
  const pressScale = useMemo(() => ix.pressed.interpolate({ inputRange: [0, 1], outputRange: [1, 0.98] }), [ix]);
  useEffect(() => {
    Animated.timing(enter, { toValue: 1, duration: 360, delay: 70 * i, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, []);
  useEffect(() => {
    if (!shake) return;
    jolt.setValue(0);
    Animated.sequence([0, 1, 2, 3, 4].map((k) => Animated.timing(jolt, { toValue: k === 4 ? 0 : k % 2 ? -1 : 1, duration: 55, useNativeDriver: true }))).start();
  }, [shake]);
  const to = (v) => Animated.spring(hot, { toValue: v, stiffness: 320, damping: 22, mass: 0.8, useNativeDriver: true }).start();
  const s = rtl ? -1 : 1;
  const tx = useMemo(
    () =>
      Animated.add(
        Animated.add(enter.interpolate({ inputRange: [0, 1], outputRange: [40 * s, 0] }), hot.interpolate({ inputRange: [0, 1], outputRange: [0, -14 * s] })),
        jolt.interpolate({ inputRange: [-1, 1], outputRange: [-8, 8] }),
      ),
    [rtl],
  );
  const u = UI[lang];
  const family = FAMILY[lang];
  return (
    <Pressable
      ref={i === 0 ? focusRef : undefined}
      onPress={() => onPick(c, i)}
      onHoverIn={() => to(1)}
      onHoverOut={() => to(0)}
      onFocus={() => {
        to(1);
        onFocusIn(true);
      }}
      onBlur={() => {
        to(0);
        onFocusIn(false);
      }}
      group
      interaction={ix}
      style={{ cursor: 'pointer' }}
    >
      <Animated.View
        style={[styles.choice, c.locked && styles.choiceLocked, { opacity: enter, transform: [{ translateX: tx }, { scale: pressScale }] }]}
        groupHoveredStyle={rtl ? styles.choiceFocusedRtl : styles.choiceFocused}
        groupFocusedStyle={rtl ? styles.choiceFocusedRtl : styles.choiceFocused}
      >
        <View style={styles.choiceNum} groupHoveredStyle={styles.choiceNumFocused} groupFocusedStyle={styles.choiceNumFocused}>
          <Text style={styles.choiceNumText} groupHoveredStyle={styles.onGold} groupFocusedStyle={styles.onGold}>
            {index}
          </Text>
        </View>
        <Text
          style={[styles.choiceText, { fontFamily: family }, c.leave && { color: colors.textMute }]}
          groupHoveredStyle={styles.white}
          groupFocusedStyle={styles.white}
          numberOfLines={1}
        >
          {c.check ? <Text style={[styles.check, c.locked && { color: colors.red }]}>[{u[c.check.skill]} {c.check.dc}] </Text> : null}
          {c.leave ? <Text style={styles.leave}>[{u.leave}] </Text> : null}
          {c.text[lang]}
        </Text>
        {c.locked ? <Text style={[styles.lock, { fontFamily: family }]}>{u.locked}</Text> : null}
      </Animated.View>
    </Pressable>
  );
});

function LangSwitch({ lang, loading, onPick }) {
  return (
    <View style={styles.langs}>
      <Text style={styles.langHint}>L / Y</Text>
      {LANGS.map((l) => (
        <Pressable key={l.id} focusable={false} onPress={() => onPick(l.id)} group style={{ cursor: 'pointer' }}>
          <View style={[styles.lang, lang === l.id && styles.langOn]} groupHoveredStyle={lang !== l.id ? styles.langHot : undefined}>
            <Text style={[styles.langText, { fontFamily: FAMILY[l.id] }, lang === l.id && { color: '#1a1206' }]}>{loading === l.id ? '…' : l.label}</Text>
          </View>
        </Pressable>
      ))}
    </View>
  );
}

export default function Dialogue({ active = true }) {
  const [lang, setLang] = useState('en');
  const [loadingLang, setLoadingLang] = useState(null);
  const [nodeId, setNodeId] = useState('start');
  // A "beat" is one spoken line: the NPC's node line or the player's choice
  // (kept untranslated so a language switch re-renders it).
  const [beat, setBeat] = useState({ who: 'seren', text: NODES.start.line, key: 0 });
  const [typed, setTyped] = useState(false);
  const [skip, setSkip] = useState(false);
  const [trust, setTrust] = useState(55);
  const [roll, setRoll] = useState(null);
  const [toast, setToast] = useState(null);
  const [lockMsg, setLockMsg] = useState(false);
  const [shakeKey, setShakeKey] = useState({});
  const [delta, setDelta] = useState(0);
  const pending = useRef(null);
  const keyN = useRef(1);
  const focusInside = useRef(false);
  const boxRef = useRef(null);
  const firstChoice = useRef(null);
  const setFocusIn = useCallback((v) => (focusInside.current = v), []);
  const node = NODES[nodeId];
  const showChoices = beat.who === 'seren' && typed && !node.end;
  const rtl = lang === 'ar';
  const u = UI[lang];
  const family = FAMILY[lang];
  const spans = useMemo(() => parse(beat.text[lang]), [beat, lang]);

  const zoom = useRef(new Animated.Value(0)).current;
  const bars = useRef(new Animated.Value(0)).current;
  const bob = useRef(new Animated.Value(0)).current;
  const swap = useRef(new Animated.Value(1)).current;
  const trustV = useRef(new Animated.Value(trust / 100)).current;
  const deltaV = useRef(new Animated.Value(0)).current;
  const toastV = useRef(new Animated.Value(0)).current;
  const fade = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.timing(bars, { toValue: 1, duration: 900, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    Animated.timing(fade, { toValue: 1, duration: 600, useNativeDriver: true }).start();
  }, []);
  useLoop(active, () => Animated.loop(Animated.sequence([
    Animated.timing(zoom, { toValue: 1, duration: 14000, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    Animated.timing(zoom, { toValue: 0, duration: 14000, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
  ])));
  useLoop(active, () => Animated.loop(Animated.sequence([
    Animated.timing(bob, { toValue: 1, duration: 650, easing: Easing.inOut(Easing.quad), useNativeDriver: true }),
    Animated.timing(bob, { toValue: 0, duration: 650, easing: Easing.inOut(Easing.quad), useNativeDriver: true }),
  ])));

  const pickLang = useCallback((id) => {
    const l = LANGS.find((x) => x.id === id);
    setLoadingLang(id);
    loadLang(l).then(
      () => {
        setLoadingLang(null);
        setLang(id);
        // Retype the current line in the new language.
        setBeat((b) => ({ ...b, key: keyN.current++ }));
        setTyped(false);
        setSkip(false);
      },
      () => setLoadingLang(null),
    );
  }, []);
  const cycleLang = useCallback(() => pickLang(LANGS[(LANGS.findIndex((l) => l.id === lang) + 1) % LANGS.length].id), [lang, pickLang]);

  const say = useCallback((who, text) => {
    setBeat((b) => {
      if (b.who !== who) {
        swap.setValue(0);
        Animated.spring(swap, { toValue: 1, stiffness: 180, damping: 18, useNativeDriver: true }).start();
      }
      return { who, text, key: keyN.current++ };
    });
    setTyped(false);
    setSkip(false);
  }, []);

  const bumpTrust = useCallback((d) => {
    if (!d) return;
    setTrust((t) => {
      const n = Math.max(0, Math.min(100, t + d));
      Animated.spring(trustV, { toValue: n / 100, stiffness: 120, damping: 16, useNativeDriver: true }).start();
      return n;
    });
    setDelta(d);
    deltaV.setValue(0);
    Animated.timing(deltaV, { toValue: 1, duration: 1600, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, []);

  const enter = useCallback((id) => {
    const n = NODES[id];
    setNodeId(id);
    setRoll(null);
    setLockMsg(false);
    if (n.trust) bumpTrust(n.trust);
    if (n.quest) {
      setToast(n.quest);
      toastV.setValue(0);
      Animated.sequence([
        Animated.spring(toastV, { toValue: 1, stiffness: 200, damping: 18, useNativeDriver: true }),
        Animated.delay(2600),
        Animated.timing(toastV, { toValue: 0, duration: 400, useNativeDriver: true }),
      ]).start();
    }
    say('seren', n.line);
  }, [say, bumpTrust]);

  const restart = useCallback(() => {
    Animated.timing(fade, { toValue: 0, duration: 450, useNativeDriver: true }).start(() => {
      setTrust(55);
      trustV.setValue(0.55);
      enter('start');
      Animated.timing(fade, { toValue: 1, duration: 600, useNativeDriver: true }).start();
    });
  }, [enter]);

  const pick = useCallback((c, i) => {
    if (c.locked) {
      setShakeKey((s) => ({ ...s, [i]: (s[i] || 0) + 1 }));
      setLockMsg(true);
      return;
    }
    let next = c.next;
    if (c.check) {
      const die = 1 + Math.floor(Math.random() * 20);
      const total = die + (c.check.bonus || 0);
      const ok = total >= c.check.dc;
      setRoll({ ...c.check, die, total, ok });
      next = ok ? c.pass : c.fail;
    } else setRoll(null);
    setLockMsg(false);
    if (c.trust) bumpTrust(c.trust);
    pending.current = next;
    say('you', c.text);
    if (focusInside.current && boxRef.current) boxRef.current.focus();
  }, [say, bumpTrust]);

  const advance = useCallback(() => {
    if (!typed) {
      setSkip(true);
      return;
    }
    if (beat.who === 'you' && pending.current) {
      const n = pending.current;
      pending.current = null;
      enter(n);
    } else if (node.end) restart();
  }, [typed, beat, node, enter, restart]);

  useEffect(() => {
    if (!active || !typed || beat.who !== 'you') return undefined;
    const t = setTimeout(advance, 1100);
    return () => clearTimeout(t);
  }, [typed, beat, active, advance]);

  useEffect(() => {
    if (showChoices && focusInside.current) {
      const t = setTimeout(() => firstChoice.current && firstChoice.current.focus(), 30);
      return () => clearTimeout(t);
    }
    return undefined;
  }, [showChoices, beat.key]);

  const stop = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const onKeyDown = (e) => {
    const k = e.key;
    if (showChoices && k >= '1' && k <= '9') {
      const idx = +k - 1;
      if (node.choices[idx]) pick(node.choices[idx], idx), stop(e);
    } else if (k === 'l' || k === 'L') cycleLang(), stop(e);
  };
  const onPad = (e) => {
    if (e.pressed && e.buttonIndex === 3) cycleLang(), stop(e);
  };

  const sp = SPEAKERS[beat.who];
  const other = SPEAKERS[beat.who === 'seren' ? 'you' : 'seren'];
  const bgScale = zoom.interpolate({ inputRange: [0, 1], outputRange: [1.02, 1.09] });
  const barT = bars.interpolate({ inputRange: [0, 1], outputRange: [-56, 0] });
  const hint = !typed ? u.skip : showChoices ? u.choose + ' · 1–' + node.choices.length : node.end ? u.over : u.cont;
  const ff = { fontFamily: family };

  return (
    <View style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onPad}>
      <Animated.View style={[StyleSheet.absoluteFill, { transform: [{ scale: bgScale }] }]}>
        <ImageBackground source={BG} style={StyleSheet.absoluteFill} resizeMode="cover" />
      </Animated.View>
      <View style={styles.dim} />
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: fade }]}>
        <Animated.View
          style={[
            styles.listener,
            rtl ? { left: 80 } : { right: 80 },
            { opacity: swap.interpolate({ inputRange: [0, 1], outputRange: [0, 0.85] }), transform: [{ translateY: swap.interpolate({ inputRange: [0, 1], outputRange: [-12, 0] }) }] },
          ]}
        >
          <Image source={other.img} style={[styles.listenerImg, { borderColor: other.color + '66' }]} />
          <Text style={[styles.listenerName, ff]}>{beat.who === 'seren' ? u.you : u.seren}</Text>
        </Animated.View>

        {lockMsg ? (
          <View style={[styles.lockMsg, rtl ? { left: 80, alignItems: 'flex-start' } : { right: 80 }]}>
            <Text style={[styles.lockMsgText, ff]}>{u.lockMsg}</Text>
          </View>
        ) : null}

        <View style={[styles.choices, rtl ? { left: 80, direction: 'rtl' } : { right: 80 }]}>
          {showChoices
            ? node.choices.map((c, i) => (
                <Choice key={beat.key + ':' + i + lang} c={c} i={i} index={i + 1} onPick={pick} focusRef={firstChoice} shake={shakeKey[i]} lang={lang} rtl={rtl} onFocusIn={setFocusIn} />
              ))
            : null}
        </View>

        <Pressable
          ref={boxRef}
          autoFocus={active}
          onPress={advance}
          onFocus={() => (focusInside.current = true)}
          onBlur={() => (focusInside.current = false)}
          group
          style={styles.boxHit}
        >
          <View style={[styles.box, rtl && { direction: 'rtl' }]} groupHoveredStyle={styles.boxHot} groupFocusedStyle={styles.boxHot}>
            <View style={styles.boxGlow} />
            <Animated.View
              style={[
                styles.portraitFrame,
                beat.who === 'you' && styles.portraitYou,
                {
                  opacity: swap,
                  transform: [
                    { translateY: Animated.add(swap.interpolate({ inputRange: [0, 1], outputRange: [30, 0] }), bob.interpolate({ inputRange: [0, 1], outputRange: [0, -2] })) },
                    { rotate: (beat.who === 'you') !== rtl ? '2deg' : '-2deg' },
                  ],
                },
              ]}
            >
              <Image source={sp.img} style={styles.portrait} />
            </Animated.View>
            <View style={styles.textCol}>
              <View style={styles.namePlate}>
                <Text style={[styles.name, ff, { color: sp.color, textShadowColor: sp.glow }]}>{sp.name[lang]}</Text>
                <Text style={[styles.title, ff]}>{sp.title[lang]}</Text>
                <View style={{ flex: 1 }} />
                {roll ? (
                  <View style={[styles.roll, { borderColor: roll.ok ? colors.green : colors.red }]}>
                    <Text style={[styles.rollText, ff]}>
                      {u[roll.skill]} · d20 {roll.die}
                      {roll.bonus ? ` + ${roll.bonus}` : ''} = {roll.total} / {roll.dc}
                    </Text>
                    <Text style={[styles.rollVerdict, ff, { color: roll.ok ? colors.green : colors.red }]}>{roll.ok ? u.success : u.failed}</Text>
                  </View>
                ) : null}
              </View>
              <Typewriter key={beat.key} spans={spans} skip={skip} active={active} family={family} onDone={() => setTyped(true)} />
              <View style={styles.footerRow}>
                <View style={styles.affinity}>
                  <Text style={[styles.affinityLabel, ff]}>{u.trust}</Text>
                  <View style={styles.affinityTrack}>
                    <Animated.View style={[styles.affinityFill, { width: trustV.interpolate({ inputRange: [0, 1], outputRange: ['0%', '100%'] }) }]} />
                  </View>
                  <Text style={styles.affinityNum}>{trust}</Text>
                  <Animated.Text
                    style={[
                      styles.delta,
                      ff,
                      { color: delta < 0 ? colors.red : colors.green },
                      { opacity: deltaV.interpolate({ inputRange: [0, 0.15, 0.7, 1], outputRange: [0, 1, 1, 0] }), transform: [{ translateY: deltaV.interpolate({ inputRange: [0, 1], outputRange: [6, -16] }) }] },
                    ]}
                  >
                    {(delta > 0 ? '+' : '') + delta + ' ' + u.trust}
                  </Animated.Text>
                </View>
                <View style={{ flex: 1 }} />
                <Text style={[styles.skip, ff]}>A / Enter · {hint}</Text>
                {typed && !showChoices ? (
                  <Animated.View style={[styles.next, { transform: [{ translateY: bob.interpolate({ inputRange: [0, 1], outputRange: [-2, 3] }) }, { rotate: '45deg' }] }]} />
                ) : null}
              </View>
            </View>
          </View>
        </Pressable>

        {toast ? (
          <Animated.View style={[styles.quest, rtl && { flexDirection: 'row-reverse' }, { opacity: toastV, transform: [{ translateY: toastV.interpolate({ inputRange: [0, 1], outputRange: [-30, 0] }) }] }]}>
            <View style={styles.questDiamond} />
            <View style={rtl && { alignItems: 'flex-end' }}>
              <Text style={[styles.questKicker, ff]}>{u.quest}</Text>
              <Text style={[styles.questTitle, ff]}>{toast[lang]}</Text>
            </View>
          </Animated.View>
        ) : null}
      </Animated.View>

      <Animated.View pointerEvents="none" style={[styles.letterbox, { top: 0, transform: [{ translateY: barT }] }]} />
      <Animated.View pointerEvents="none" style={[styles.letterbox, { bottom: 0, transform: [{ translateY: Animated.multiply(barT, -1) }] }]} />
      <Animated.View style={[styles.langBar, { transform: [{ translateY: barT }] }]}>
        <LangSwitch lang={lang} loading={loadingLang} onPick={pickLang} />
      </Animated.View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: '#05070f' },
  dim: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(4,6,14,0.35)' },
  letterbox: { position: 'absolute', left: 0, right: 0, height: 56, backgroundColor: '#000' },

  listener: { position: 'absolute', top: 90, alignItems: 'center' },
  listenerImg: { width: 96, height: 96, borderRadius: 48, borderWidth: 2 },
  listenerName: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textDim, marginTop: 8 },

  lockMsg: { position: 'absolute', top: 196, width: 560, alignItems: 'flex-end' },
  lockMsgText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: '#ff8fa3', paddingHorizontal: 10, paddingVertical: 5, borderRadius: 6, backgroundColor: 'rgba(40,6,14,0.8)' },

  choices: { position: 'absolute', top: 236, width: 560, gap: 8 },
  choice: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    height: 44,
    paddingHorizontal: 12,
    borderRadius: 8,
    backgroundColor: 'rgba(8,10,22,0.72)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.06)',
  },
  choiceFocused: {
    borderColor: 'rgba(242,195,91,0.8)',
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.28), rgba(8,10,22,0.8) 70%)',
    boxShadow: '0 0 20px rgba(242,195,91,0.25)',
  },
  choiceFocusedRtl: {
    borderColor: 'rgba(242,195,91,0.8)',
    experimental_backgroundImage: 'linear-gradient(270deg, rgba(242,195,91,0.28), rgba(8,10,22,0.8) 70%)',
    boxShadow: '0 0 20px rgba(242,195,91,0.25)',
  },
  choiceLocked: { opacity: 0.55 },
  onGold: { color: '#1a1206' },
  white: { color: '#fff' },
  choiceNum: { width: 24, height: 24, borderRadius: 6, borderWidth: 1, borderColor: colors.lineStrong, alignItems: 'center', justifyContent: 'center' },
  choiceNumFocused: { backgroundColor: colors.gold, borderColor: colors.gold },
  choiceNumText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.textDim },
  choiceText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 15, color: colors.text },
  check: { fontWeight: '700', color: colors.cyan },
  leave: { fontWeight: '700', color: colors.textMute },
  lock: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.4, color: colors.red },

  boxHit: { position: 'absolute', left: 60, right: 60, bottom: 76, height: 190, cursor: 'pointer' },
  box: {
    flex: 1,
    flexDirection: 'row',
    borderRadius: 18,
    backgroundColor: 'rgba(9,12,24,0.94)',
    borderWidth: 1,
    borderColor: 'rgba(148,170,255,0.22)',
    boxShadow: '0 30px 60px rgba(0,0,0,0.7), inset 0 1px 0 rgba(255,255,255,0.08)',
  },
  boxHot: { borderColor: 'rgba(148,170,255,0.4)' },
  boxGlow: {
    position: 'absolute',
    left: 0,
    right: 0,
    top: 0,
    height: 2,
    borderTopLeftRadius: 18,
    borderTopRightRadius: 18,
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(79,209,255,0) 0%, rgba(79,209,255,0.9) 30%, rgba(242,195,91,0.9) 70%, rgba(242,195,91,0) 100%)',
  },
  portraitFrame: {
    width: 190,
    height: 230,
    marginTop: -64,
    marginStart: 26,
    borderRadius: 16,
    padding: 4,
    experimental_backgroundImage: 'linear-gradient(180deg, #7fe3ff 0%, #2a3f80 60%, #151b38 100%)',
    boxShadow: '0 18px 40px rgba(0,0,0,0.7), 0 0 26px rgba(79,209,255,0.35)',
  },
  portraitYou: {
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #8a5a1c 60%, #2a1c0c 100%)',
    boxShadow: '0 18px 40px rgba(0,0,0,0.7), 0 0 26px rgba(242,195,91,0.35)',
  },
  portrait: { flex: 1, borderRadius: 12 },
  textCol: { flex: 1, paddingVertical: 22, paddingStart: 28, paddingEnd: 30 },
  namePlate: { flexDirection: 'row', alignItems: 'center', gap: 12, height: 28 },
  name: { fontFamily: font, fontWeight: '700', fontSize: 22, textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 12 },
  title: { fontFamily: font, fontStyle: 'italic', fontSize: 13, color: colors.textMute, marginTop: 4 },
  roll: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, height: 26, borderRadius: 6, borderWidth: 1, backgroundColor: 'rgba(0,0,0,0.4)' },
  rollText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim },
  rollVerdict: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.4 },
  line: { fontFamily: font, fontSize: 18, lineHeight: 28, color: '#dfe5f7', marginTop: 10 },
  em: { fontStyle: 'italic', color: '#fff' },
  keyword: { fontWeight: '700', color: colors.gold },
  place: { fontWeight: '600', color: colors.orange },
  footerRow: { flexDirection: 'row', alignItems: 'center', marginTop: 'auto', gap: 12 },
  affinity: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  affinityLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute },
  affinityTrack: { width: 120, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.1)', overflow: 'hidden' },
  affinityFill: { height: '100%', borderRadius: 2, backgroundColor: colors.cyan },
  affinityNum: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.text, width: 24 },
  delta: { fontFamily: font, fontWeight: '700', fontSize: 12 },
  skip: { fontFamily: font, fontSize: 12, color: colors.textMute },
  next: { width: 10, height: 10, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.gold, marginTop: -4 },

  langBar: { position: 'absolute', top: 0, right: 40, height: 56, justifyContent: 'center' },
  langs: { flexDirection: 'row', alignItems: 'center', gap: 4 },
  langHint: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.2, color: colors.textMute, marginRight: 6 },
  lang: { height: 28, paddingHorizontal: 12, borderRadius: 14, justifyContent: 'center', borderWidth: 1, borderColor: 'rgba(255,255,255,0.12)' },
  langOn: { backgroundColor: colors.gold, borderColor: colors.gold, boxShadow: '0 0 14px rgba(242,195,91,0.5)' },
  langHot: { borderColor: 'rgba(255,255,255,0.5)', backgroundColor: 'rgba(255,255,255,0.06)' },
  langText: { fontWeight: '600', fontSize: 13, color: colors.textDim },
  quest: {
    position: 'absolute',
    top: 80,
    alignSelf: 'center',
    left: 440,
    width: 400,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 16,
    paddingHorizontal: 22,
    paddingVertical: 14,
    borderRadius: 12,
    backgroundColor: 'rgba(12,10,4,0.9)',
    borderWidth: 1,
    borderColor: 'rgba(242,195,91,0.6)',
    boxShadow: '0 0 30px rgba(242,195,91,0.3), 0 20px 40px rgba(0,0,0,0.6)',
  },
  questDiamond: { width: 14, height: 14, backgroundColor: colors.gold, transform: [{ rotate: '45deg' }], boxShadow: '0 0 12px rgba(242,195,91,0.9)' },
  questKicker: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, textTransform: 'uppercase', color: colors.gold },
  questTitle: { fontFamily: font, fontWeight: '700', fontSize: 20, color: '#fff', marginTop: 2 },
});
