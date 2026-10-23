// react-reconciler host config (mutation mode, React 19.3 / reconciler 0.34)
// mapping React's host operations onto the __jsui_native bridge.

import { createContext } from 'react';
import Reconciler from 'react-reconciler';
import { native, queueMicrotask } from './native.js';
import { instances, HostInstance, TextInstance } from './instances.js';
import { armPendingLayoutAnimation } from './layoutAnimation.js';

export const DiscreteEventPriority = 2;
export const ContinuousEventPriority = 8;
export const DefaultEventPriority = 32;
const NoEventPriority = 0;
const ConcurrentRoot = 1;

let currentUpdatePriority = NoEventPriority;
/** Sets the React update priority for the duration of an event; returns the previous one. */
export function setUpdatePriority(p) {
  const prev = currentUpdatePriority;
  currentUpdatePriority = p;
  return prev;
}

// Host context: are we inside a <Text>? Constant objects, no allocation.
const ROOT_CONTEXT = { inText: false };
const TEXT_CONTEXT = { inText: true };

function textOutsideText(text) {
  const shown = text.length > 40 ? text.slice(0, 40) + '...' : text;
  return new Error(`Text strings must be rendered within a <Text> component. (text: "${shown}")`);
}

function attach(parent, child) {
  child.parent = parent;
  if (parent.type === 'ScrollView' && !parent._content) parent._content = child;
}

const noop = () => {};

const hostConfig = {
  rendererPackageName: '@orktes/jsui-react',
  rendererVersion: '0.1.0',
  extraDevToolsConfig: null,
  supportsMutation: true,
  supportsPersistence: false,
  supportsHydration: false,
  supportsMicrotasks: true,
  supportsResources: false,
  supportsSingletons: false,
  supportsTestSelectors: false,
  isPrimaryRenderer: true,
  warnsIfNotActing: false,
  noTimeout: -1,
  scheduleTimeout: (fn, ms) => setTimeout(fn, ms),
  cancelTimeout: (id) => clearTimeout(id),
  scheduleMicrotask: queueMicrotask,

  getRootHostContext: () => ROOT_CONTEXT,
  getChildHostContext: (_parent, type) => (type === 'Text' ? TEXT_CONTEXT : ROOT_CONTEXT),
  getPublicInstance: (instance) => instance,
  shouldSetTextContent: () => false,

  // The root container picks the surface (document) the node belongs to.
  createInstance(type, props, root) {
    const id = native.createNode(type, root.id);
    const inst = new HostInstance(id, type, props);
    instances.set(id, inst);
    return inst;
  },
  createTextInstance(text, root, hostContext) {
    // Like RN: a string outside <Text> is an error (whitespace is tolerated
    // because it is invisible and very common in hand-written JSX).
    if (!hostContext.inText && text.trim() !== '') throw textOutsideText(text);
    const id = native.createNode('RawText', root.id);
    native.setText(id, text);
    const inst = new TextInstance(id, text);
    instances.set(id, inst);
    return inst;
  },
  appendInitialChild(parent, child) {
    native.appendChild(parent.id, child.id);
    attach(parent, child);
  },
  finalizeInitialChildren: (_inst, _type, props) => !!(props.autoFocus || props.hasTVPreferredFocus),
  commitMount(inst) {
    inst.focus();
  },

  prepareForCommit() {
    armPendingLayoutAnimation();
    return null;
  },
  resetAfterCommit: noop,
  preparePortalMount: noop,

  appendChild(parent, child) {
    native.appendChild(parent.id, child.id);
    attach(parent, child);
  },
  appendChildToContainer(container, child) {
    native.appendChild(container.id, child.id);
    child.parent = null;
    const i = container.children.indexOf(child);
    if (i >= 0) container.children.splice(i, 1);
    container.children.push(child);
  },
  insertBefore(parent, child, before) {
    native.insertBefore(parent.id, child.id, before.id);
    attach(parent, child);
  },
  insertInContainerBefore(container, child, before) {
    native.insertBefore(container.id, child.id, before.id);
    child.parent = null;
    const i = container.children.indexOf(child);
    if (i >= 0) container.children.splice(i, 1);
    const j = container.children.indexOf(before);
    container.children.splice(j < 0 ? container.children.length : j, 0, child);
  },
  // React only removes host children when deleting them (moves use
  // insertBefore/appendChild), so the native subtree is freed right here.
  removeChild(parent, child) {
    native.removeChild(parent.id, child.id);
    native.destroyNode(child.id);
    child.parent = null;
    if (parent._content === child) parent._content = null;
  },
  removeChildFromContainer(container, child) {
    native.removeChild(container.id, child.id);
    native.destroyNode(child.id);
    const i = container.children.indexOf(child);
    if (i >= 0) container.children.splice(i, 1);
  },
  clearContainer(container) {
    for (const child of container.children) {
      native.removeChild(container.id, child.id);
      native.destroyNode(child.id);
    }
    container.children.length = 0;
  },
  detachDeletedInstance(inst) {
    if (instances.get(inst.id) === inst) instances.delete(inst.id);
  },

  commitUpdate(inst, _type, _oldProps, newProps) {
    inst.update(newProps);
  },
  commitTextUpdate(ti, _old, text) {
    ti.text = text;
    native.setText(ti.id, text);
  },
  resetTextContent: noop,
  hideInstance: (inst) => inst.setHidden(true),
  unhideInstance(inst, props) {
    inst.props = props;
    inst.setHidden(false);
  },
  hideTextInstance: (ti) => native.setText(ti.id, ''),
  unhideTextInstance(ti, text) {
    ti.text = text;
    native.setText(ti.id, text);
  },

  // Update priorities (React 19 API).
  setCurrentUpdatePriority(p) {
    currentUpdatePriority = p;
  },
  getCurrentUpdatePriority: () => currentUpdatePriority,
  resolveUpdatePriority: () => (currentUpdatePriority !== NoEventPriority ? currentUpdatePriority : DefaultEventPriority),
  resolveEventType: () => null,
  resolveEventTimeStamp: () => -1.1,
  trackSchedulerEvent: noop,
  shouldAttemptEagerTransition: () => false,

  // Suspensey commits: nothing in JSUI blocks a commit.
  maySuspendCommit: () => false,
  maySuspendCommitOnUpdate: () => false,
  maySuspendCommitInSyncRender: () => false,
  preloadInstance: () => true,
  startSuspendingCommit: () => null,
  suspendInstance: noop,
  suspendOnActiveViewTransition: noop,
  waitForCommitToBeReady: () => null,
  getSuspendedCommitReason: () => null,

  NotPendingTransition: null,
  HostTransitionContext: createContext(null),
  resetFormInstance: noop,
  requestPostPaintCallback: noop,
  bindToConsole: (method, args) => Function.prototype.bind.apply(console[method] || console.log, [console].concat(args)),

  getInstanceFromNode: () => null,
  getInstanceFromScope: () => null,
  prepareScopeUpdate: noop,
  beforeActiveInstanceBlur: noop,
  afterActiveInstanceBlur: noop,
  setFocusIfFocusable: () => false,

  // View transitions / fragment refs are not supported (not in stable React).
  applyViewTransitionName: noop,
  restoreViewTransitionName: noop,
  cancelViewTransitionName: noop,
  cancelRootViewTransitionName: noop,
  restoreRootViewTransitionName: noop,
  cloneRootViewTransitionContainer: noop,
  removeRootViewTransitionClone: noop,
  measureInstance: () => null,
  measureClonedInstance: () => null,
  wasInstanceInViewport: () => true,
  hasInstanceChanged: () => false,
  hasInstanceAffectedParent: () => false,
  startViewTransition: () => null,
  startGestureTransition: () => null,
  stopViewTransition: noop,
  getCurrentGestureOffset: () => 0,
  createViewTransitionInstance: () => null,
  addViewTransitionFinishedListener: noop,
  createFragmentInstance: () => null,
  updateFragmentInstanceFiber: noop,
  commitNewChildToFragmentInstance: noop,
  deleteChildFromFragmentInstance: noop,
};

export const reconciler = Reconciler(hostConfig);

// --- roots -------------------------------------------------------------------

// QuickJS' error.stack has no "Name: message" header (V8's does).
export function describeError(error) {
  if (!error || typeof error !== 'object') return String(error);
  const head = (error.name || 'Error') + ': ' + error.message;
  const stack = error.stack ? String(error.stack) : '';
  return stack.startsWith(head) ? stack : head + (stack ? '\n' + stack : '');
}

function reportUncaught(error, info) {
  const stack = info && info.componentStack ? '\nComponent stack:' + info.componentStack : '';
  console.error('[jsui] Uncaught error in a React component: ' + describeError(error) + stack);
  // Hand it to the runtime (jsui_last_error, dev overlay): __jsui_reportError
  // or the standard reportError global if the host has one, otherwise
  // rethrow from a timer so the engine reports it like any uncaught error.
  if (typeof globalThis.__jsui_reportError === 'function') {
    globalThis.__jsui_reportError(error);
    return;
  }
  if (typeof globalThis.reportError === 'function') {
    globalThis.reportError(error);
    return;
  }
  setTimeout(() => {
    throw error;
  }, 0);
}

function reportCaught(error, info) {
  const stack = info && info.componentStack ? '\nComponent stack:' + info.componentStack : '';
  console.error('[jsui] Error caught by an error boundary: ' + describeError(error) + stack);
}

function reportRecoverable(error) {
  console.warn('[jsui] Recoverable React error: ' + ((error && error.message) || error));
}

const roots = new Map();

/** Creates (or returns) the React root for a native container node id. */
export function getRoot(containerId = native.rootId) {
  let root = roots.get(containerId);
  if (!root) {
    const container = { id: containerId, type: 'Root', children: [] };
    const fiberRoot = reconciler.createContainer(
      container,
      ConcurrentRoot,
      null,
      false,
      null,
      '',
      reportUncaught,
      reportCaught,
      reportRecoverable,
      noop,
    );
    root = { container, fiberRoot };
    roots.set(containerId, root);
  }
  return root;
}

/**
 * Renders `element` into the runtime's root container. The first render is
 * synchronous so the frame after bundle evaluation already shows the UI.
 */
export function render(element, options) {
  const root = getRoot(options && options.container);
  if (options && options.concurrent) {
    reconciler.updateContainer(element, root.fiberRoot, null, null);
  } else {
    reconciler.updateContainerSync(element, root.fiberRoot, null, null);
    reconciler.flushSyncWork();
  }
  return root;
}

export function unmount(options) {
  const id = (options && options.container) || native.rootId;
  const root = roots.get(id);
  if (!root) return;
  reconciler.updateContainerSync(null, root.fiberRoot, null, null);
  reconciler.flushSyncWork();
  roots.delete(id);
}

/**
 * The root tag (React Native) of the tree a component renders in: the main
 * surface's root container id, or a surface's (AppRegistry provides it).
 */
export const RootTagContext = createContext(native.rootId);

/** The container object of a root tag (default: the main root), e.g. a portal target. */
export function rootContainer(rootTag) {
  return getRoot(rootTag == null ? native.rootId : rootTag).container;
}

export function createPortal(children, container, key) {
  return reconciler.createPortal(children, container || rootContainer(), null, key == null ? null : '' + key);
}

export function flushSync(fn) {
  return reconciler.flushSyncFromReconciler ? reconciler.flushSyncFromReconciler(fn) : fn();
}

export const batchedUpdates = (fn, a) => reconciler.batchedUpdates(fn, a);
