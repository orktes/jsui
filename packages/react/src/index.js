// @orktes/jsui-react: React Native's public API on the JSUI runtime.
// The CLI aliases 'react-native' to this module.

import { installGlobals, passive } from './events.js';
import { View, Text, Image, ImageBackground, ScrollView, TextInput, TextAncestorContext, unsupportedComponent } from './components/basic.js';
import {
  Pressable,
  TouchableOpacity,
  TouchableHighlight,
  TouchableWithoutFeedback,
  TouchableNativeFeedback,
  Button,
} from './components/pressables.js';
import { Group, Visibility, useGroupState } from './components/group.js';
import { Overlay, OverlayArrow } from './components/overlay.js';
import { FlatList, SectionList, VirtualizedList } from './components/lists.js';
import {
  ActivityIndicator,
  Switch,
  Modal,
  StatusBar,
  SafeAreaView,
  KeyboardAvoidingView,
  InputAccessoryView,
  RefreshControl,
  DrawerLayoutAndroid,
  ProgressBarAndroid,
} from './components/misc.js';
import { Animated, makeAnimated, useAnimatedValue, useHostBufferValue, useInteraction, useHostStateBind } from './animated/index.js';
import { Easing } from './animated/Easing.js';
import { Expr, clamp, lerp, step, smoothstep, fract, wrap } from './animated/expression.js';
import { StyleSheet } from './StyleSheet.js';
import { processColor } from './color.js';
import { PanResponder } from './PanResponder.js';
import { render, unmount, createPortal, flushSync, batchedUpdates, rootContainer } from './renderer.js';
import { bridgeStats } from './instances.js';
import { registerAsset } from './components/basic.js';
import { Host } from './apis.js';
import { installHostData } from './hostData.js';
import { installHostBuffer } from './hostBuffer.js';
import { installHostTexture, HostTexture } from './hostTexture.js';
import { Video, VideoState } from './video.js';
import { LottieView } from './lottie.js';
import { ShaderView, sksl, compileShader, precompileShaders } from './shader.js';
import { Sound, SoundInstance, SoundEndReason, useSound, useMusic, UISounds, UISoundsContext, useUISound } from './sound.js';
import { installInspector, inspect } from './inspect.js';

installGlobals();
installHostData();
installHostBuffer();
installHostTexture();
installInspector();
// include/jsui/jsui.h documents a `jsui` global for host messages.
if (!globalThis.jsui) globalThis.jsui = Host;
makeAnimated({ View, Text, Image, ScrollView, FlatList, SectionList });

export * from './apis.js';
export {
  View,
  Text,
  Image,
  ImageBackground,
  HostTexture,
  Video,
  VideoState,
  LottieView,
  ShaderView,
  Sound,
  SoundInstance,
  SoundEndReason,
  useSound,
  useMusic,
  UISounds,
  UISoundsContext,
  useUISound,
  sksl,
  compileShader,
  precompileShaders,
  ScrollView,
  TextInput,
  Pressable,
  TouchableOpacity,
  TouchableHighlight,
  TouchableWithoutFeedback,
  TouchableNativeFeedback,
  Button,
  FlatList,
  SectionList,
  VirtualizedList,
  ActivityIndicator,
  Switch,
  Modal,
  StatusBar,
  SafeAreaView,
  KeyboardAvoidingView,
  InputAccessoryView,
  RefreshControl,
  DrawerLayoutAndroid,
  ProgressBarAndroid,
  Animated,
  Easing,
  StyleSheet,
  PanResponder,
  passive,
  processColor,
  useAnimatedValue,
  useHostBufferValue,
  useInteraction,
  useHostStateBind,
  useGroupState,
  Group,
  Visibility,
  Overlay,
  OverlayArrow,
  // Animated.derive expression helpers (animated/expression.js)
  Expr,
  clamp,
  lerp,
  step,
  smoothstep,
  fract,
  wrap,
  // jsui renderer API
  render,
  unmount,
  createPortal,
  flushSync,
  rootContainer,
  bridgeStats,
  registerAsset,
  inspect,
};

export const unstable_batchedUpdates = batchedUpdates;
export const unstable_TextAncestorContext = TextAncestorContext;
export const VirtualizedSectionList = SectionList;
export const Touchable = { Mixin: {}, TOUCH_TARGET_DEBUG: false, renderDebugView: () => null };
export const experimental_LayoutConformance = (props) => props.children;
export const unstable_VirtualView = unsupportedComponent('VirtualView');
export const VirtualViewMode = { Visible: 0, Prerender: 1, Hidden: 2 };
export const CheckBox = unsupportedComponent('CheckBox');
export const Picker = unsupportedComponent('Picker');
export const ProgressViewIOS = ProgressBarAndroid;
export const ImageEditor = { cropImage: () => Promise.reject(new Error('ImageEditor is not available')) };
export const AssetRegistry = { registerAsset, getAssetByID: () => null };
