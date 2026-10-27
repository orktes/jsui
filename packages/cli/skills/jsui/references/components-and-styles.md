# Components and styles

JSUI implements React Native's public API (React 19). Code written for RN
generally runs unchanged. This is what is supported and how it differs.

## Components

| Component | Notes |
| --- | --- |
| `View` | The host element. All style keys below, `pointerEvents`, `onLayout`, focus props, `testID` / `nativeID`. |
| `Text` | Nested Texts inherit text styles. `numberOfLines`, `ellipsizeMode`, `onPress` (pressable text), `reveal` / `revealMode` / `revealFade` / `revealFraction` (typewriter: paints only the first N graphemes or words, laid out whole; animate it with `Animated.Text`, see native-first.md). Strings outside `<Text>` throw (whitespace is tolerated). |
| `Image` | `source`: `require('./a.png')` (sizes itself from the file; `a@2x.png` is half size), `{ uri }`, an array, a number. `resizeMode` (`cover` / `contain` / `stretch` / `center` / `repeat`), `tintColor` (blur with `filter: 'blur(4px)'`), `onLoad` / `onError` / `onLoadStart` / `onLoadEnd`. `Image.getSize`, `Image.prefetch`. |
| `ImageBackground` | `source`, `resizeMode`, `imageStyle`, children on top. |
| `ScrollView` | `horizontal`, `contentContainerStyle`, `onScroll` (+ `scrollEventThrottle`), `pagingEnabled`, `snapToInterval` / `snapToOffsets` / `snapToAlignment`, `decelerationRate`, `stickyHeaderIndices`, `showsVertical/HorizontalScrollIndicator`, `scrollEnabled`, `contentOffset`; ref `scrollTo({x, y, animated})`, `scrollToEnd`. Mouse wheel, trackpad, touch flings, keyboard and gamepad (right stick) all scroll it; focus moves scroll the focused item into view. |
| `FlatList` / `SectionList` / `VirtualizedList` | Virtualized like RN: `data`, `renderItem`, `keyExtractor`, `numColumns`, `horizontal`, `inverted`, `ItemSeparatorComponent`, `ListHeader/Footer/EmptyComponent`, `onEndReached`, `getItemLayout`, `initialNumToRender`, `windowSize`; ref `scrollToIndex` / `scrollToOffset`. Gamepad focus keeps the next off-screen row mounted. |
| `Pressable` | `onPress`, `onPressIn` / `onPressOut`, `onLongPress` (`delayLongPress`), `onHoverIn` / `onHoverOut`, `onFocus` / `onBlur`, `disabled` (no `hitSlop`: pad the pressable instead). `style` and `children` may be functions of `{ pressed, hovered, focused }`. Focusable by default. Pressed feedback lasts at least 130 ms, so quick taps and gamepad presses are visible. |
| `TouchableOpacity`, `TouchableHighlight`, `TouchableWithoutFeedback`, `TouchableNativeFeedback`, `Button` | As in RN (`activeOpacity`, `underlayColor`, `title` / `color`). Focusable. |
| `TextInput` | Native editing (caret, selection, clipboard, undo). `value` / `defaultValue`, `onChangeText`, `onChange`, `onSubmitEditing`, `onKeyPress`, `onSelectionChange`, `selection`, `placeholder`, `placeholderTextColor`, `maxLength`, `multiline`, `secureTextEntry`, `selectTextOnFocus`, `submitBehavior`, `caretHidden`, `cursorColor`, `selectionColor`, `autoFocus`; ref `focus` / `blur` / `clear` / `isFocused` / `setSelection`. No IME pre-edit. |
| `Modal` | `visible`, `transparent`, `onRequestClose` (B / Escape), `onShow`. Rendered at the root, traps focus, restores focus when closed. `animationType` is ignored: animate the content yourself. |
| `Switch`, `ActivityIndicator`, `SafeAreaView`, `KeyboardAvoidingView`, `StatusBar` | Work. `StatusBar` `hidden` / `barStyle` reach the host as shared state `jsui.statusBar` (the iOS host shows or hides the clock and battery); other hosts ignore it. |
| `HostTexture`, `LottieView`, `ShaderView` | JSUI extras, see media-and-effects.md. |

Not available (render nothing and warn once): `Picker`, `CheckBox`,
`VirtualView`, platform-only components such as `DrawerLayoutAndroid` (stub).
Build those from Views and Pressables.

## APIs

`StyleSheet` (`create`, `flatten`, `compose`, `absoluteFill`,
`absoluteFillObject`, `hairlineWidth`), `Animated`, `Easing`, `PanResponder`,
`LayoutAnimation`, `Platform` (`OS === 'jsui'`; `Platform.select` prefers
`jsui`, then `native`, then `default`), `Dimensions`, `useWindowDimensions`
(the view's size; changes when the engine resizes the view), `PixelRatio`,
`AppRegistry`, `Appearance` / `useColorScheme` (`'dark'`), `I18nManager`,
`BackHandler` (gamepad B), `Keyboard`, `AppState`, `Alert` (logs),
`Linking` (posts a message to the host), `InteractionManager`,
`DeviceEventEmitter`, `findNodeHandle`, `processColor`. Every other
`react-native` export exists as a harmless stub so imports never fail.

Refs on host components have `measure(cb)`, `measureInWindow(cb)`,
`measureLayout`, `focus()`, `blur()`, `setNativeProps`.

## Styles

React Native style keys, including RN 0.76+ additions:

* **Layout (Yoga):** `flex`, `flexDirection`, `flexGrow` / `flexShrink` /
  `flexBasis`, `flexWrap`, `justifyContent`, `alignItems`, `alignSelf`,
  `alignContent`, `gap` / `rowGap` / `columnGap`, `margin*`, `padding*`
  (incl. `marginHorizontal`, `paddingVertical`, logical `*Start` / `*End`),
  `position` (`relative` / `absolute`), `top` / `right` / `bottom` / `left`,
  `inset*`, `width` / `height` / `min*` / `max*` (numbers, `'50%'`, `'auto'`),
  `aspectRatio`, `display` (`flex` / `none`), `overflow` (`hidden` clips,
  incl. rounded corners), `borderWidth*`, `direction`, `boxSizing`, `zIndex`.
* **Paint:** `backgroundColor`, `opacity`, `borderColor*`, `borderRadius*`
  (per corner and logical), `borderStyle` (`solid` / `dashed` / `dotted`),
  `boxShadow` (CSS string like `'0 8px 24px rgba(0,0,0,0.5)'` or RN object
  array; `inset`, spread), legacy `shadowColor` / `shadowOffset` /
  `shadowOpacity` / `shadowRadius`, `experimental_backgroundImage` /
  `backgroundImage` (`'linear-gradient(180deg, #1a2a4a, #0b0f1a)'`,
  `radial-gradient(...)`, several layers), `transform` (RN array or CSS
  string: `translateX/Y`, `scale`, `scaleX/Y`, `rotate`, `rotateX/Y/Z`,
  `skewX/Y`, `perspective`, `matrix`), `transformOrigin`, `filter` (`blur`,
  `brightness`, `contrast`, `saturate`, `grayscale`), `pointerEvents`,
  `cursor`.
* **Text:** `color`, `fontFamily`, `fontSize`, `fontWeight`, `fontStyle`,
  `lineHeight`, `letterSpacing`, `textAlign`, `textTransform`,
  `textDecorationLine` / `textDecorationColor`, `textShadowColor` /
  `textShadowOffset` / `textShadowRadius` (glows), `fontVariant`
  (`tabular-nums` for counters), `fontVariationSettings`,
  `fontFeatureSettings` (see Fonts).
* **Image:** `resizeMode`, `tintColor`.

Colors: any CSS color (`#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`,
`rgba()`, `hsl()`, `hsla()`, names, `transparent`).

Gradient text, masks and blend modes are not styles: use a `ShaderView`
(see media-and-effects.md).

`expo-linear-gradient` / `react-native-linear-gradient`'s `<LinearGradient
colors={[...]} start={{x:0,y:0}} end={{x:0,y:1}}>` works (rendered as a
native gradient background).

## Fonts

The default font is **Inter** (embedded: 400, 500, 600, 700 and italic).
Bundle your own `.ttf` / `.otf` / `.woff` / `.woff2` (also `.ttc` collections
and variable fonts) with `require`: the CLI emits the file next to the bundle
and `require` returns its URL.

```jsx
import { Fonts, useFonts } from '@orktes/jsui-react';

function App() {
  const [loaded, error] = useFonts({
    Cinzel: require('./fonts/Cinzel-Variable.ttf'),          // weight / style from the file
    Game: [                                                  // several faces, one family (like @font-face)
      { source: require('./fonts/Game-Regular.otf'), weight: 400 },
      { source: require('./fonts/Game-Bold.otf'), weight: 'bold' },
      { source: require('./fonts/Game-Italic.otf'), weight: 400, style: 'italic' },
    ],
  });
  if (!loaded && !error) return null;                        // fonts already loaded: true on the first render
  return <Text style={{ fontFamily: 'Game', fontWeight: '700' }}>Victory</Text>;
}

await Fonts.load('Cinzel', require('./fonts/Cinzel-Bold.ttf'), { weight: 700 });   // outside components
await Fonts.loadAsync({ Mono: require('./fonts/Mono.woff2') });                    // expo-font signature
Fonts.load('Runes', new Uint8Array(bytes));                  // ArrayBuffer / typed array (fetched, generated)
```

* Sources: a URL, `require(...)`, `{ uri }` or bytes. Options: `weight`
  (`400`, `'700'`, `'bold'`, ...) and `style` (`'italic'` / `'normal'`);
  omitted, they are read from the file. The same face loads once; text
  re-lays out when it arrives.
* `Fonts.isLoaded(f)`, `isLoading(f)`, `getLoadedFonts()`, `unloadAsync(f)`,
  `unloadAllAsync()` (fonts you loaded; Inter stays),
  `Fonts.setDefaultFamily('Game')` (the family when a style names none).
* **expo-font** imports work (shim): `loadAsync`, `useFonts`, `isLoaded`,
  `unloadAsync`, `FontDisplay`. `@expo-google-fonts/*` packages work
  unchanged: `useFonts({ Lato_400Regular })`, then `fontFamily:
  'Lato_400Regular'`.
* **Fallbacks:** Inter covers Latin, Greek, Cyrillic, common punctuation,
  arrows and math, not CJK, emoji, dingbats or controller symbols. Missing
  glyphs come from `fontFamily` lists (`'Game, Noto Sans JP'`), then
  `Fonts.setFallbacks(['Noto Color Emoji', 'Noto Sans JP'])`, then any
  registered font; with none, a glyph draws as an empty box, no warning.
  Draw button hints as Views, images or an icon font, and screenshot them.
* **Variable fonts / OpenType features** (inherited text styles):
  `fontVariationSettings: "'wght' 650, 'wdth' 80"` or `{ wght: 650 }` (`wght`
  follows `fontWeight` by default), `fontFeatureSettings: "'ss01' 1, 'liga'
  0"` or `{ ss01: 1 }`; `fontVariant: ['tabular-nums']` for counters.
* **Host-provided fonts** (shared by every view of the runtime): C
  `jsui_register_font(rt, "Game", data, size, 0, -1)` (family `NULL` = the
  name in the file), Godot `JsuiView.register_font(...)` or its `fonts`
  property, web `jsui.registerFont(...)` / `jsui.loadFont(...)` on the
  `createJsui` host. The UI then uses `fontFamily: 'Game'` with no loading.

Text is shaped with HarfBuzz and laid out with Skia's paragraph engine
(Arabic, Devanagari, CJK work with a covering font); `writingDirection` /
RTL follow `I18nManager`.

## Images and assets

Prefer bundled assets (`require`): they are emitted next to the bundle and
load from the same place the engine loads the bundle (files, Godot `res://`,
the web server). Remote `{uri: 'http://...'}` works in dev and on the web;
native builds have no TLS, so `https://` URLs need the engine's loader.
`data:` URIs work everywhere.

## Things that behave differently from a phone

* There is no on-screen keyboard; `TextInput` gets keys from the engine.
* `Dimensions` / `useWindowDimensions` is the view the engine gives the UI
  (often 1280x720 or 1920x1080 logical px at a pixel ratio). Design for
  16:9 and test other sizes with `--width` / `--height`.
* `Alert.alert` only logs. Build dialogs with `Modal`.
* `Linking.openURL` posts a `jsui:openURL` message to the host.
* `console.*` goes to the engine's log (and `jsui run` / `screenshot` stderr
  for warnings and errors).
