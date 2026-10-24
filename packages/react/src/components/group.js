// Group and Visibility: interaction state and visibility resolved by the
// runtime (core/state_style.h), shared with descendants through context.

import { createContext, createElement as h, useContext, useMemo } from 'react';
import { jsx } from 'react/jsx-runtime';
import { Interaction } from '../animated/nodes.js';
import { createAnimatedComponent } from '../animated/createAnimatedComponent.js';

const AnimatedView = createAnimatedComponent('View');

// {name, ix: Interaction, parent}
export const GroupContext = createContext(null);

// Groups that no element carries: their values stay 0.
const detached = new Map();

/**
 * The interaction state of the nearest enclosing group (`<Group>` or a
 * Pressable with `group`), or of the one named `name`: `{ hovered, pressed,
 * focused, focusVisible, disabled }` as native Animated values (0..1, eased
 * by the group's `transition`), and `.instant.hovered` ... (0 / 1 at once).
 * Use them in styles, interpolate, Animated.derive and <Visibility visible>.
 */
export function useGroupState(name) {
  let g = useContext(GroupContext);
  if (name != null) while (g && g.name !== name) g = g.parent;
  if (g) return g.ix;
  const k = name == null ? '' : String(name);
  let ix = detached.get(k);
  if (!ix) detached.set(k, (ix = new Interaction()));
  return ix;
}

/** Provides `ix` (the element's interaction) to useGroupState below it. */
export function useGroupProvider(group, interaction, children) {
  const parent = useContext(GroupContext);
  const own = useMemo(() => new Interaction(), []);
  const ix = interaction || own;
  const name = typeof group === 'string' ? group : null;
  const value = useMemo(() => ({ name, ix, parent }), [name, ix, parent]);
  return { ix, children: group ? jsx(GroupContext.Provider, { value, children }) : children };
}

/**
 * A View that is a group: descendants' groupHoveredStyle etc. and
 * useGroupState() follow its hover / press / focus. `name` names it for
 * groupName / useGroupState(name); `transition` eases its state.
 */
export function Group(props) {
  const { name, interaction, children } = props;
  const p = useGroupProvider(name == null ? true : String(name), interaction, children);
  const rest = Object.assign({}, props);
  delete rest.name;
  rest.group = name == null ? true : String(name);
  rest.interaction = p.ix;
  rest.children = p.children;
  return jsx('View', rest);
}
Group.displayName = 'Group';

/**
 * Shows or hides its children natively. `visible`: a boolean or an Animated
 * node (nonzero shows: Animated.derive, useGroupState().hovered,
 * useHostStateBind, a host buffer value); the runtime applies a change on
 * the frame it happens, without JS. `mode`: 'remove' (default: no layout,
 * paint or hits once hidden, like display: none) or 'hide' (keeps its
 * layout). `effect`: 'fade' (default with a `transition`), 'scale' (fade +
 * scale from `scale`, default 0.9) or 'none'.
 */
export function Visibility(props) {
  const { visible = true, mode = 'remove', effect, scale, transition } = props;
  const fx = effect || (transition != null ? 'fade' : 'none');
  const visibility = useMemo(() => {
    const v = { mode, effect: fx };
    if (typeof scale === 'number') v.scale = scale;
    return v;
  }, [mode, fx, scale]);
  const rest = Object.assign({}, props);
  delete rest.mode;
  delete rest.effect;
  delete rest.scale;
  rest.visible = visible;
  rest.visibility = visibility;
  return h(AnimatedView, rest);
}
Visibility.displayName = 'Visibility';
