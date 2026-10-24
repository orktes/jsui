(() => {
  var __create = Object.create;
  var __defProp = Object.defineProperty;
  var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
  var __getOwnPropNames = Object.getOwnPropertyNames;
  var __getProtoOf = Object.getPrototypeOf;
  var __hasOwnProp = Object.prototype.hasOwnProperty;
  var __commonJS = (cb, mod) => function __require() {
    try {
      return mod || (0, cb[__getOwnPropNames(cb)[0]])((mod = { exports: {} }).exports, mod), mod.exports;
    } catch (e) {
      throw mod = 0, e;
    }
  };
  var __copyProps = (to, from, except, desc) => {
    if (from && typeof from === "object" || typeof from === "function") {
      for (let key of __getOwnPropNames(from))
        if (!__hasOwnProp.call(to, key) && key !== except)
          __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
    }
    return to;
  };
  var __toESM = (mod, isNodeMode, target) => (target = mod != null ? __create(__getProtoOf(mod)) : {}, __copyProps(
    // If the importer is in node compatibility mode or this is not an ESM
    // file that has been converted to a CommonJS file using a Babel-
    // compatible transform (i.e. "__esModule" has not been set), then set
    // "default" to the CommonJS "module.exports" for node compatibility.
    isNodeMode || !mod || !mod.__esModule ? __defProp(target, "default", { value: mod, enumerable: true }) : target,
    mod
  ));

  // node_modules/react/cjs/react.production.js
  var require_react_production = __commonJS({
    "node_modules/react/cjs/react.production.js"(exports) {
      "use strict";
      var REACT_ELEMENT_TYPE = /* @__PURE__ */ Symbol.for("react.transitional.element");
      var REACT_PORTAL_TYPE = /* @__PURE__ */ Symbol.for("react.portal");
      var REACT_FRAGMENT_TYPE = /* @__PURE__ */ Symbol.for("react.fragment");
      var REACT_STRICT_MODE_TYPE = /* @__PURE__ */ Symbol.for("react.strict_mode");
      var REACT_PROFILER_TYPE = /* @__PURE__ */ Symbol.for("react.profiler");
      var REACT_CONSUMER_TYPE = /* @__PURE__ */ Symbol.for("react.consumer");
      var REACT_CONTEXT_TYPE = /* @__PURE__ */ Symbol.for("react.context");
      var REACT_FORWARD_REF_TYPE = /* @__PURE__ */ Symbol.for("react.forward_ref");
      var REACT_SUSPENSE_TYPE = /* @__PURE__ */ Symbol.for("react.suspense");
      var REACT_MEMO_TYPE = /* @__PURE__ */ Symbol.for("react.memo");
      var REACT_LAZY_TYPE = /* @__PURE__ */ Symbol.for("react.lazy");
      var REACT_ACTIVITY_TYPE = /* @__PURE__ */ Symbol.for("react.activity");
      var REACT_VIEW_TRANSITION_TYPE = /* @__PURE__ */ Symbol.for("react.view_transition");
      var MAYBE_ITERATOR_SYMBOL = Symbol.iterator;
      function getIteratorFn(maybeIterable) {
        if (null === maybeIterable || "object" !== typeof maybeIterable) return null;
        maybeIterable = MAYBE_ITERATOR_SYMBOL && maybeIterable[MAYBE_ITERATOR_SYMBOL] || maybeIterable["@@iterator"];
        return "function" === typeof maybeIterable ? maybeIterable : null;
      }
      var ReactNoopUpdateQueue = {
        isMounted: function() {
          return false;
        },
        enqueueForceUpdate: function() {
        },
        enqueueReplaceState: function() {
        },
        enqueueSetState: function() {
        }
      };
      var assign = Object.assign;
      var emptyObject = {};
      function Component(props, context, updater) {
        this.props = props;
        this.context = context;
        this.refs = emptyObject;
        this.updater = updater || ReactNoopUpdateQueue;
      }
      Component.prototype.isReactComponent = {};
      Component.prototype.setState = function(partialState, callback) {
        if ("object" !== typeof partialState && "function" !== typeof partialState && null != partialState)
          throw Error(
            "takes an object of state variables to update or a function which returns an object of state variables."
          );
        this.updater.enqueueSetState(this, partialState, callback, "setState");
      };
      Component.prototype.forceUpdate = function(callback) {
        this.updater.enqueueForceUpdate(this, callback, "forceUpdate");
      };
      function ComponentDummy() {
      }
      ComponentDummy.prototype = Component.prototype;
      function PureComponent(props, context, updater) {
        this.props = props;
        this.context = context;
        this.refs = emptyObject;
        this.updater = updater || ReactNoopUpdateQueue;
      }
      var pureComponentPrototype = PureComponent.prototype = new ComponentDummy();
      pureComponentPrototype.constructor = PureComponent;
      assign(pureComponentPrototype, Component.prototype);
      pureComponentPrototype.isPureReactComponent = true;
      var isArrayImpl = Array.isArray;
      function noop() {
      }
      var ReactSharedInternals = { H: null, A: null, T: null, S: null };
      var hasOwnProperty = Object.prototype.hasOwnProperty;
      function ReactElement(type, key, props) {
        var refProp = props.ref;
        return {
          $$typeof: REACT_ELEMENT_TYPE,
          type,
          key,
          ref: void 0 !== refProp ? refProp : null,
          props
        };
      }
      function cloneAndReplaceKey(oldElement, newKey) {
        return ReactElement(oldElement.type, newKey, oldElement.props);
      }
      function isValidElement(object) {
        return "object" === typeof object && null !== object && object.$$typeof === REACT_ELEMENT_TYPE;
      }
      function escape(key) {
        var escaperLookup = { "=": "=0", ":": "=2" };
        return "$" + key.replace(/[=:]/g, function(match) {
          return escaperLookup[match];
        });
      }
      var userProvidedKeyEscapeRegex = /\/+/g;
      function getElementKey(element, index) {
        return "object" === typeof element && null !== element && null != element.key ? escape("" + element.key) : index.toString(36);
      }
      function resolveThenable(thenable) {
        switch (thenable.status) {
          case "fulfilled":
            return thenable.value;
          case "rejected":
            throw thenable.reason;
          default:
            switch ("string" === typeof thenable.status ? thenable.then(noop, noop) : (thenable.status = "pending", thenable.then(
              function(fulfilledValue) {
                "pending" === thenable.status && (thenable.status = "fulfilled", thenable.value = fulfilledValue);
              },
              function(error) {
                "pending" === thenable.status && (thenable.status = "rejected", thenable.reason = error);
              }
            )), thenable.status) {
              case "fulfilled":
                return thenable.value;
              case "rejected":
                throw thenable.reason;
            }
        }
        throw thenable;
      }
      function mapIntoArray(children, array, escapedPrefix, nameSoFar, callback) {
        var type = typeof children;
        if ("undefined" === type || "boolean" === type) children = null;
        var invokeCallback = false;
        if (null === children) invokeCallback = true;
        else
          switch (type) {
            case "bigint":
            case "string":
            case "number":
              invokeCallback = true;
              break;
            case "object":
              switch (children.$$typeof) {
                case REACT_ELEMENT_TYPE:
                case REACT_PORTAL_TYPE:
                  invokeCallback = true;
                  break;
                case REACT_LAZY_TYPE:
                  return invokeCallback = children._init, mapIntoArray(
                    invokeCallback(children._payload),
                    array,
                    escapedPrefix,
                    nameSoFar,
                    callback
                  );
              }
          }
        if (invokeCallback)
          return callback = callback(children), invokeCallback = "" === nameSoFar ? "." + getElementKey(children, 0) : nameSoFar, isArrayImpl(callback) ? (escapedPrefix = "", null != invokeCallback && (escapedPrefix = invokeCallback.replace(userProvidedKeyEscapeRegex, "$&/") + "/"), mapIntoArray(callback, array, escapedPrefix, "", function(c) {
            return c;
          })) : null != callback && (isValidElement(callback) && (callback = cloneAndReplaceKey(
            callback,
            escapedPrefix + (null == callback.key || children && children.key === callback.key ? "" : ("" + callback.key).replace(
              userProvidedKeyEscapeRegex,
              "$&/"
            ) + "/") + invokeCallback
          )), array.push(callback)), 1;
        invokeCallback = 0;
        var nextNamePrefix = "" === nameSoFar ? "." : nameSoFar + ":";
        if (isArrayImpl(children))
          for (var i = 0; i < children.length; i++)
            nameSoFar = children[i], type = nextNamePrefix + getElementKey(nameSoFar, i), invokeCallback += mapIntoArray(
              nameSoFar,
              array,
              escapedPrefix,
              type,
              callback
            );
        else if (i = getIteratorFn(children), "function" === typeof i)
          for (children = i.call(children), i = 0; !(nameSoFar = children.next()).done; )
            nameSoFar = nameSoFar.value, type = nextNamePrefix + getElementKey(nameSoFar, i++), invokeCallback += mapIntoArray(
              nameSoFar,
              array,
              escapedPrefix,
              type,
              callback
            );
        else if ("object" === type) {
          if ("function" === typeof children.then)
            return mapIntoArray(
              resolveThenable(children),
              array,
              escapedPrefix,
              nameSoFar,
              callback
            );
          array = String(children);
          throw Error(
            "Objects are not valid as a React child (found: " + ("[object Object]" === array ? "object with keys {" + Object.keys(children).join(", ") + "}" : array) + "). If you meant to render a collection of children, use an array instead."
          );
        }
        return invokeCallback;
      }
      function mapChildren(children, func, context) {
        if (null == children) return children;
        var result = [], count = 0;
        mapIntoArray(children, result, "", "", function(child) {
          return func.call(context, child, count++);
        });
        return result;
      }
      function lazyInitializer(payload) {
        if (-1 === payload._status) {
          var ctor = payload._result, thenable = ctor();
          thenable.then(
            function(moduleObject) {
              if (0 === payload._status || -1 === payload._status)
                payload._status = 1, payload._result = moduleObject, void 0 === thenable.status && (thenable.status = "fulfilled", thenable.value = moduleObject);
            },
            function(error) {
              if (0 === payload._status || -1 === payload._status)
                payload._status = 2, payload._result = error, void 0 === thenable.status && (thenable.status = "rejected", thenable.reason = error);
            }
          );
          -1 === payload._status && (payload._status = 0, payload._result = thenable);
        }
        if (1 === payload._status) return payload._result.default;
        throw payload._result;
      }
      var reportGlobalError = "function" === typeof reportError ? reportError : function(error) {
        if ("object" === typeof window && "function" === typeof window.ErrorEvent) {
          var event = new window.ErrorEvent("error", {
            bubbles: true,
            cancelable: true,
            message: "object" === typeof error && null !== error && "string" === typeof error.message ? String(error.message) : String(error),
            error
          });
          if (!window.dispatchEvent(event)) return;
        } else if ("object" === typeof process && "function" === typeof process.emit) {
          process.emit("uncaughtException", error);
          return;
        }
        console.error(error);
      };
      function startTransition(scope) {
        var prevTransition = ReactSharedInternals.T, currentTransition = {};
        currentTransition.types = null !== prevTransition ? prevTransition.types : null;
        ReactSharedInternals.T = currentTransition;
        try {
          var returnValue = scope(), onStartTransitionFinish = ReactSharedInternals.S;
          null !== onStartTransitionFinish && onStartTransitionFinish(currentTransition, returnValue);
          "object" === typeof returnValue && null !== returnValue && "function" === typeof returnValue.then && returnValue.then(noop, reportGlobalError);
        } catch (error) {
          reportGlobalError(error);
        } finally {
          null !== prevTransition && null !== currentTransition.types && (prevTransition.types = currentTransition.types), ReactSharedInternals.T = prevTransition;
        }
      }
      function addTransitionType(type) {
        var transition = ReactSharedInternals.T;
        if (null !== transition) {
          var transitionTypes = transition.types;
          null === transitionTypes ? transition.types = [type] : -1 === transitionTypes.indexOf(type) && transitionTypes.push(type);
        } else startTransition(addTransitionType.bind(null, type));
      }
      var Children = {
        map: mapChildren,
        forEach: function(children, forEachFunc, forEachContext) {
          mapChildren(
            children,
            function() {
              forEachFunc.apply(this, arguments);
            },
            forEachContext
          );
        },
        count: function(children) {
          var n = 0;
          mapChildren(children, function() {
            n++;
          });
          return n;
        },
        toArray: function(children) {
          return mapChildren(children, function(child) {
            return child;
          }) || [];
        },
        only: function(children) {
          if (!isValidElement(children))
            throw Error(
              "React.Children.only expected to receive a single React element child."
            );
          return children;
        }
      };
      exports.Activity = REACT_ACTIVITY_TYPE;
      exports.Children = Children;
      exports.Component = Component;
      exports.Fragment = REACT_FRAGMENT_TYPE;
      exports.Profiler = REACT_PROFILER_TYPE;
      exports.PureComponent = PureComponent;
      exports.StrictMode = REACT_STRICT_MODE_TYPE;
      exports.Suspense = REACT_SUSPENSE_TYPE;
      exports.ViewTransition = REACT_VIEW_TRANSITION_TYPE;
      exports.__CLIENT_INTERNALS_DO_NOT_USE_OR_WARN_USERS_THEY_CANNOT_UPGRADE = ReactSharedInternals;
      exports.__COMPILER_RUNTIME = {
        __proto__: null,
        c: function(size) {
          return ReactSharedInternals.H.useMemoCache(size);
        }
      };
      exports.addTransitionType = addTransitionType;
      exports.cache = function(fn) {
        return function() {
          return fn.apply(null, arguments);
        };
      };
      exports.cacheSignal = function() {
        return null;
      };
      exports.cloneElement = function(element, config, children) {
        if (null === element || void 0 === element)
          throw Error(
            "The argument must be a React element, but you passed " + element + "."
          );
        var props = assign({}, element.props), key = element.key;
        if (null != config)
          for (propName in void 0 !== config.key && (key = "" + config.key), config)
            !hasOwnProperty.call(config, propName) || "key" === propName || "__self" === propName || "__source" === propName || "ref" === propName && void 0 === config.ref || (props[propName] = config[propName]);
        var propName = arguments.length - 2;
        if (1 === propName) props.children = children;
        else if (1 < propName) {
          for (var childArray = Array(propName), i = 0; i < propName; i++)
            childArray[i] = arguments[i + 2];
          props.children = childArray;
        }
        return ReactElement(element.type, key, props);
      };
      exports.createContext = function(defaultValue) {
        defaultValue = {
          $$typeof: REACT_CONTEXT_TYPE,
          _currentValue: defaultValue,
          _currentValue2: defaultValue,
          _threadCount: 0,
          Provider: null,
          Consumer: null
        };
        defaultValue.Provider = defaultValue;
        defaultValue.Consumer = {
          $$typeof: REACT_CONSUMER_TYPE,
          _context: defaultValue
        };
        return defaultValue;
      };
      exports.createElement = function(type, config, children) {
        var propName, props = {}, key = null;
        if (null != config)
          for (propName in void 0 !== config.key && (key = "" + config.key), config)
            hasOwnProperty.call(config, propName) && "key" !== propName && "__self" !== propName && "__source" !== propName && (props[propName] = config[propName]);
        var childrenLength = arguments.length - 2;
        if (1 === childrenLength) props.children = children;
        else if (1 < childrenLength) {
          for (var childArray = Array(childrenLength), i = 0; i < childrenLength; i++)
            childArray[i] = arguments[i + 2];
          props.children = childArray;
        }
        if (type && type.defaultProps)
          for (propName in childrenLength = type.defaultProps, childrenLength)
            void 0 === props[propName] && (props[propName] = childrenLength[propName]);
        return ReactElement(type, key, props);
      };
      exports.createRef = function() {
        return { current: null };
      };
      exports.forwardRef = function(render2) {
        return { $$typeof: REACT_FORWARD_REF_TYPE, render: render2 };
      };
      exports.isValidElement = isValidElement;
      exports.lazy = function(ctor) {
        return {
          $$typeof: REACT_LAZY_TYPE,
          _payload: { _status: -1, _result: ctor },
          _init: lazyInitializer
        };
      };
      exports.memo = function(type, compare) {
        return {
          $$typeof: REACT_MEMO_TYPE,
          type,
          compare: void 0 === compare ? null : compare
        };
      };
      exports.startTransition = startTransition;
      exports.unstable_useCacheRefresh = function() {
        return ReactSharedInternals.H.useCacheRefresh();
      };
      exports.use = function(usable) {
        return ReactSharedInternals.H.use(usable);
      };
      exports.useActionState = function(action, initialState, permalink) {
        return ReactSharedInternals.H.useActionState(action, initialState, permalink);
      };
      exports.useCallback = function(callback, deps) {
        return ReactSharedInternals.H.useCallback(callback, deps);
      };
      exports.useContext = function(Context) {
        return ReactSharedInternals.H.useContext(Context);
      };
      exports.useDebugValue = function() {
      };
      exports.useDeferredValue = function(value, initialValue) {
        return ReactSharedInternals.H.useDeferredValue(value, initialValue);
      };
      exports.useEffect = function(create, deps) {
        return ReactSharedInternals.H.useEffect(create, deps);
      };
      exports.useEffectEvent = function(callback) {
        return ReactSharedInternals.H.useEffectEvent(callback);
      };
      exports.useId = function() {
        return ReactSharedInternals.H.useId();
      };
      exports.useImperativeHandle = function(ref, create, deps) {
        return ReactSharedInternals.H.useImperativeHandle(ref, create, deps);
      };
      exports.useInsertionEffect = function(create, deps) {
        return ReactSharedInternals.H.useInsertionEffect(create, deps);
      };
      exports.useLayoutEffect = function(create, deps) {
        return ReactSharedInternals.H.useLayoutEffect(create, deps);
      };
      exports.useMemo = function(create, deps) {
        return ReactSharedInternals.H.useMemo(create, deps);
      };
      exports.useOptimistic = function(passthrough, reducer) {
        return ReactSharedInternals.H.useOptimistic(passthrough, reducer);
      };
      exports.useReducer = function(reducer, initialArg, init) {
        return ReactSharedInternals.H.useReducer(reducer, initialArg, init);
      };
      exports.useRef = function(initialValue) {
        return ReactSharedInternals.H.useRef(initialValue);
      };
      exports.useState = function(initialState) {
        return ReactSharedInternals.H.useState(initialState);
      };
      exports.useSyncExternalStore = function(subscribe, getSnapshot, getServerSnapshot) {
        return ReactSharedInternals.H.useSyncExternalStore(
          subscribe,
          getSnapshot,
          getServerSnapshot
        );
      };
      exports.useTransition = function() {
        return ReactSharedInternals.H.useTransition();
      };
      exports.version = "19.3.1";
    }
  });

  // node_modules/react/index.js
  var require_react = __commonJS({
    "node_modules/react/index.js"(exports, module) {
      "use strict";
      if (true) {
        module.exports = require_react_production();
      } else {
        module.exports = null;
      }
    }
  });

  // node_modules/scheduler/cjs/scheduler.production.js
  var require_scheduler_production = __commonJS({
    "node_modules/scheduler/cjs/scheduler.production.js"(exports) {
      "use strict";
      function push(heap, node) {
        var index = heap.length;
        heap.push(node);
        a: for (; 0 < index; ) {
          var parentIndex = index - 1 >>> 1, parent = heap[parentIndex];
          if (0 < compare(parent, node))
            heap[parentIndex] = node, heap[index] = parent, index = parentIndex;
          else break a;
        }
      }
      function peek(heap) {
        return 0 === heap.length ? null : heap[0];
      }
      function pop(heap) {
        if (0 === heap.length) return null;
        var first = heap[0], last = heap.pop();
        if (last !== first) {
          heap[0] = last;
          a: for (var index = 0, length = heap.length, halfLength = length >>> 1; index < halfLength; ) {
            var leftIndex = 2 * (index + 1) - 1, left = heap[leftIndex], rightIndex = leftIndex + 1, right = heap[rightIndex];
            if (0 > compare(left, last))
              rightIndex < length && 0 > compare(right, left) ? (heap[index] = right, heap[rightIndex] = last, index = rightIndex) : (heap[index] = left, heap[leftIndex] = last, index = leftIndex);
            else if (rightIndex < length && 0 > compare(right, last))
              heap[index] = right, heap[rightIndex] = last, index = rightIndex;
            else break a;
          }
        }
        return first;
      }
      function compare(a, b) {
        var diff = a.sortIndex - b.sortIndex;
        return 0 !== diff ? diff : a.id - b.id;
      }
      exports.unstable_now = void 0;
      if ("object" === typeof performance && "function" === typeof performance.now) {
        localPerformance = performance;
        exports.unstable_now = function() {
          return localPerformance.now();
        };
      } else {
        localDate = Date, initialTime = localDate.now();
        exports.unstable_now = function() {
          return localDate.now() - initialTime;
        };
      }
      var localPerformance;
      var localDate;
      var initialTime;
      var taskQueue = [];
      var timerQueue = [];
      var taskIdCounter = 1;
      var currentTask = null;
      var currentPriorityLevel = 3;
      var isPerformingWork = false;
      var isHostCallbackScheduled = false;
      var isHostTimeoutScheduled = false;
      var needsPaint = false;
      var localSetTimeout = "function" === typeof setTimeout ? setTimeout : null;
      var localClearTimeout = "function" === typeof clearTimeout ? clearTimeout : null;
      var localSetImmediate = "undefined" !== typeof setImmediate ? setImmediate : null;
      function advanceTimers(currentTime) {
        for (var timer = peek(timerQueue); null !== timer; ) {
          if (null === timer.callback) pop(timerQueue);
          else if (timer.startTime <= currentTime)
            pop(timerQueue), timer.sortIndex = timer.expirationTime, push(taskQueue, timer);
          else break;
          timer = peek(timerQueue);
        }
      }
      function handleTimeout(currentTime) {
        isHostTimeoutScheduled = false;
        advanceTimers(currentTime);
        if (!isHostCallbackScheduled)
          if (null !== peek(taskQueue))
            isHostCallbackScheduled = true, isMessageLoopRunning || (isMessageLoopRunning = true, schedulePerformWorkUntilDeadline());
          else {
            var firstTimer = peek(timerQueue);
            null !== firstTimer && requestHostTimeout(handleTimeout, firstTimer.startTime - currentTime);
          }
      }
      var isMessageLoopRunning = false;
      var taskTimeoutID = -1;
      var frameInterval = 5;
      var startTime = -1;
      function shouldYieldToHost() {
        return needsPaint ? true : exports.unstable_now() - startTime < frameInterval ? false : true;
      }
      function performWorkUntilDeadline() {
        needsPaint = false;
        if (isMessageLoopRunning) {
          var currentTime = exports.unstable_now();
          startTime = currentTime;
          var hasMoreWork = true;
          try {
            a: {
              isHostCallbackScheduled = false;
              isHostTimeoutScheduled && (isHostTimeoutScheduled = false, localClearTimeout(taskTimeoutID), taskTimeoutID = -1);
              isPerformingWork = true;
              var previousPriorityLevel = currentPriorityLevel;
              try {
                b: {
                  advanceTimers(currentTime);
                  for (currentTask = peek(taskQueue); null !== currentTask && !(currentTask.expirationTime > currentTime && shouldYieldToHost()); ) {
                    var callback = currentTask.callback;
                    if ("function" === typeof callback) {
                      currentTask.callback = null;
                      currentPriorityLevel = currentTask.priorityLevel;
                      var continuationCallback = callback(
                        currentTask.expirationTime <= currentTime
                      );
                      currentTime = exports.unstable_now();
                      if ("function" === typeof continuationCallback) {
                        currentTask.callback = continuationCallback;
                        advanceTimers(currentTime);
                        hasMoreWork = true;
                        break b;
                      }
                      currentTask === peek(taskQueue) && pop(taskQueue);
                      advanceTimers(currentTime);
                    } else pop(taskQueue);
                    currentTask = peek(taskQueue);
                  }
                  if (null !== currentTask) hasMoreWork = true;
                  else {
                    var firstTimer = peek(timerQueue);
                    null !== firstTimer && requestHostTimeout(
                      handleTimeout,
                      firstTimer.startTime - currentTime
                    );
                    hasMoreWork = false;
                  }
                }
                break a;
              } finally {
                currentTask = null, currentPriorityLevel = previousPriorityLevel, isPerformingWork = false;
              }
              hasMoreWork = void 0;
            }
          } finally {
            hasMoreWork ? schedulePerformWorkUntilDeadline() : isMessageLoopRunning = false;
          }
        }
      }
      var schedulePerformWorkUntilDeadline;
      if ("function" === typeof localSetImmediate)
        schedulePerformWorkUntilDeadline = function() {
          localSetImmediate(performWorkUntilDeadline);
        };
      else if ("undefined" !== typeof MessageChannel) {
        channel = new MessageChannel(), port = channel.port2;
        channel.port1.onmessage = performWorkUntilDeadline;
        schedulePerformWorkUntilDeadline = function() {
          port.postMessage(null);
        };
      } else
        schedulePerformWorkUntilDeadline = function() {
          localSetTimeout(performWorkUntilDeadline, 0);
        };
      var channel;
      var port;
      function requestHostTimeout(callback, ms) {
        taskTimeoutID = localSetTimeout(function() {
          callback(exports.unstable_now());
        }, ms);
      }
      exports.unstable_IdlePriority = 5;
      exports.unstable_ImmediatePriority = 1;
      exports.unstable_LowPriority = 4;
      exports.unstable_NormalPriority = 3;
      exports.unstable_Profiling = null;
      exports.unstable_UserBlockingPriority = 2;
      exports.unstable_cancelCallback = function(task) {
        task.callback = null;
      };
      exports.unstable_forceFrameRate = function(fps) {
        0 > fps || 125 < fps ? console.error(
          "forceFrameRate takes a positive int between 0 and 125, forcing frame rates higher than 125 fps is not supported"
        ) : frameInterval = 0 < fps ? Math.floor(1e3 / fps) : 5;
      };
      exports.unstable_getCurrentPriorityLevel = function() {
        return currentPriorityLevel;
      };
      exports.unstable_next = function(eventHandler) {
        switch (currentPriorityLevel) {
          case 1:
          case 2:
          case 3:
            var priorityLevel = 3;
            break;
          default:
            priorityLevel = currentPriorityLevel;
        }
        var previousPriorityLevel = currentPriorityLevel;
        currentPriorityLevel = priorityLevel;
        try {
          return eventHandler();
        } finally {
          currentPriorityLevel = previousPriorityLevel;
        }
      };
      exports.unstable_requestPaint = function() {
        needsPaint = true;
      };
      exports.unstable_runWithPriority = function(priorityLevel, eventHandler) {
        switch (priorityLevel) {
          case 1:
          case 2:
          case 3:
          case 4:
          case 5:
            break;
          default:
            priorityLevel = 3;
        }
        var previousPriorityLevel = currentPriorityLevel;
        currentPriorityLevel = priorityLevel;
        try {
          return eventHandler();
        } finally {
          currentPriorityLevel = previousPriorityLevel;
        }
      };
      exports.unstable_scheduleCallback = function(priorityLevel, callback, options) {
        var currentTime = exports.unstable_now();
        "object" === typeof options && null !== options ? (options = options.delay, options = "number" === typeof options && 0 < options ? currentTime + options : currentTime) : options = currentTime;
        switch (priorityLevel) {
          case 1:
            var timeout = -1;
            break;
          case 2:
            timeout = 250;
            break;
          case 5:
            timeout = 1073741823;
            break;
          case 4:
            timeout = 1e4;
            break;
          default:
            timeout = 5e3;
        }
        timeout = options + timeout;
        priorityLevel = {
          id: taskIdCounter++,
          callback,
          priorityLevel,
          startTime: options,
          expirationTime: timeout,
          sortIndex: -1
        };
        options > currentTime ? (priorityLevel.sortIndex = options, push(timerQueue, priorityLevel), null === peek(taskQueue) && priorityLevel === peek(timerQueue) && (isHostTimeoutScheduled ? (localClearTimeout(taskTimeoutID), taskTimeoutID = -1) : isHostTimeoutScheduled = true, requestHostTimeout(handleTimeout, options - currentTime))) : (priorityLevel.sortIndex = timeout, push(taskQueue, priorityLevel), isHostCallbackScheduled || isPerformingWork || (isHostCallbackScheduled = true, isMessageLoopRunning || (isMessageLoopRunning = true, schedulePerformWorkUntilDeadline())));
        return priorityLevel;
      };
      exports.unstable_shouldYield = shouldYieldToHost;
      exports.unstable_wrapCallback = function(callback) {
        var parentPriorityLevel = currentPriorityLevel;
        return function() {
          var previousPriorityLevel = currentPriorityLevel;
          currentPriorityLevel = parentPriorityLevel;
          try {
            return callback.apply(this, arguments);
          } finally {
            currentPriorityLevel = previousPriorityLevel;
          }
        };
      };
    }
  });

  // node_modules/scheduler/index.js
  var require_scheduler = __commonJS({
    "node_modules/scheduler/index.js"(exports, module) {
      "use strict";
      if (true) {
        module.exports = require_scheduler_production();
      } else {
        module.exports = null;
      }
    }
  });

  // node_modules/react-reconciler/cjs/react-reconciler.production.js
  var require_react_reconciler_production = __commonJS({
    "node_modules/react-reconciler/cjs/react-reconciler.production.js"(exports, module) {
      "use strict";
      module.exports = function($$$config) {
        function createFiber(tag, pendingProps, key, mode) {
          return new FiberNode(tag, pendingProps, key, mode);
        }
        function noop() {
        }
        function formatProdErrorMessage(code) {
          var url = "https://react.dev/errors/" + code;
          if (1 < arguments.length) {
            url += "?args[]=" + encodeURIComponent(arguments[1]);
            for (var i = 2; i < arguments.length; i++)
              url += "&args[]=" + encodeURIComponent(arguments[i]);
          }
          return "Minified React error #" + code + "; visit " + url + " for the full message or use the non-minified dev environment for full errors and additional helpful warnings.";
        }
        function getNearestMountedFiber(fiber) {
          for (var node = fiber, nextNode = node; nextNode && !nextNode.alternate; )
            node = nextNode, 0 !== (node.flags & 4098) && (fiber = node.return), nextNode = node.return;
          for (; node.return; ) node = node.return;
          return 3 === node.tag ? fiber : null;
        }
        function assertIsMounted(fiber) {
          if (getNearestMountedFiber(fiber) !== fiber)
            throw Error(formatProdErrorMessage(188));
        }
        function findCurrentFiberUsingSlowPath(fiber) {
          var alternate = fiber.alternate;
          if (!alternate) {
            alternate = getNearestMountedFiber(fiber);
            if (null === alternate) throw Error(formatProdErrorMessage(188));
            return alternate !== fiber ? null : fiber;
          }
          for (var a = fiber, b = alternate; ; ) {
            var parentA = a.return;
            if (null === parentA) break;
            var parentB = parentA.alternate;
            if (null === parentB) {
              b = parentA.return;
              if (null !== b) {
                a = b;
                continue;
              }
              break;
            }
            if (parentA.child === parentB.child) {
              for (parentB = parentA.child; parentB; ) {
                if (parentB === a) return assertIsMounted(parentA), fiber;
                if (parentB === b) return assertIsMounted(parentA), alternate;
                parentB = parentB.sibling;
              }
              throw Error(formatProdErrorMessage(188));
            }
            if (a.return !== b.return) a = parentA, b = parentB;
            else {
              for (var didFindChild = false, child$0 = parentA.child; child$0; ) {
                if (child$0 === a) {
                  didFindChild = true;
                  a = parentA;
                  b = parentB;
                  break;
                }
                if (child$0 === b) {
                  didFindChild = true;
                  b = parentA;
                  a = parentB;
                  break;
                }
                child$0 = child$0.sibling;
              }
              if (!didFindChild) {
                for (child$0 = parentB.child; child$0; ) {
                  if (child$0 === a) {
                    didFindChild = true;
                    a = parentB;
                    b = parentA;
                    break;
                  }
                  if (child$0 === b) {
                    didFindChild = true;
                    b = parentB;
                    a = parentA;
                    break;
                  }
                  child$0 = child$0.sibling;
                }
                if (!didFindChild) throw Error(formatProdErrorMessage(189));
              }
            }
            if (a.alternate !== b) throw Error(formatProdErrorMessage(190));
          }
          if (3 !== a.tag) throw Error(formatProdErrorMessage(188));
          return a.stateNode.current === a ? fiber : alternate;
        }
        function findCurrentHostFiberImpl(node) {
          var tag = node.tag;
          if (5 === tag || 26 === tag || 27 === tag || 6 === tag) return node;
          for (node = node.child; null !== node; ) {
            tag = findCurrentHostFiberImpl(node);
            if (null !== tag) return tag;
            node = node.sibling;
          }
          return null;
        }
        function findCurrentHostFiberWithNoPortalsImpl(node) {
          var tag = node.tag;
          if (5 === tag || 26 === tag || 27 === tag || 6 === tag) return node;
          for (node = node.child; null !== node; ) {
            if (4 !== node.tag && (tag = findCurrentHostFiberWithNoPortalsImpl(node), null !== tag))
              return tag;
            node = node.sibling;
          }
          return null;
        }
        function getIteratorFn(maybeIterable) {
          if (null === maybeIterable || "object" !== typeof maybeIterable)
            return null;
          maybeIterable = MAYBE_ITERATOR_SYMBOL && maybeIterable[MAYBE_ITERATOR_SYMBOL] || maybeIterable["@@iterator"];
          return "function" === typeof maybeIterable ? maybeIterable : null;
        }
        function getComponentNameFromType(type) {
          if (null == type) return null;
          if ("function" === typeof type)
            return type.$$typeof === REACT_CLIENT_REFERENCE ? null : type.displayName || type.name || null;
          if ("string" === typeof type) return type;
          switch (type) {
            case REACT_FRAGMENT_TYPE:
              return "Fragment";
            case REACT_PROFILER_TYPE:
              return "Profiler";
            case REACT_STRICT_MODE_TYPE:
              return "StrictMode";
            case REACT_SUSPENSE_TYPE:
              return "Suspense";
            case REACT_SUSPENSE_LIST_TYPE:
              return "SuspenseList";
            case REACT_ACTIVITY_TYPE:
              return "Activity";
            case REACT_VIEW_TRANSITION_TYPE:
              return "ViewTransition";
          }
          if ("object" === typeof type)
            switch (type.$$typeof) {
              case REACT_PORTAL_TYPE:
                return "Portal";
              case REACT_CONTEXT_TYPE:
                return type.displayName || "Context";
              case REACT_CONSUMER_TYPE:
                return (type._context.displayName || "Context") + ".Consumer";
              case REACT_FORWARD_REF_TYPE:
                var innerType = type.render;
                type = type.displayName;
                type || (type = innerType.displayName || innerType.name || "", type = "" !== type ? "ForwardRef(" + type + ")" : "ForwardRef");
                return type;
              case REACT_MEMO_TYPE:
                return innerType = type.displayName || null, null !== innerType ? innerType : getComponentNameFromType(type.type) || "Memo";
              case REACT_LAZY_TYPE:
                innerType = type._payload;
                type = type._init;
                try {
                  return getComponentNameFromType(type(innerType));
                } catch (x) {
                }
            }
          return null;
        }
        function createCursor(defaultValue) {
          return { current: defaultValue };
        }
        function pop(cursor) {
          0 > index$jscomp$0 || (cursor.current = valueStack[index$jscomp$0], valueStack[index$jscomp$0] = null, index$jscomp$0--);
        }
        function push(cursor, value) {
          index$jscomp$0++;
          valueStack[index$jscomp$0] = cursor.current;
          cursor.current = value;
        }
        function clz32Fallback(x) {
          x >>>= 0;
          return 0 === x ? 32 : 31 - (log$1(x) / LN2 | 0) | 0;
        }
        function getHighestPriorityLanes(lanes) {
          var pendingSyncLanes = lanes & 42;
          if (0 !== pendingSyncLanes) return pendingSyncLanes;
          switch (lanes & -lanes) {
            case 1:
              return 1;
            case 2:
              return 2;
            case 4:
              return 4;
            case 8:
              return 8;
            case 16:
              return 16;
            case 32:
              return 32;
            case 64:
              return 64;
            case 128:
              return 128;
            case 256:
            case 512:
            case 1024:
            case 2048:
            case 4096:
            case 8192:
            case 16384:
            case 32768:
            case 65536:
            case 131072:
              return lanes & -lanes;
            case 262144:
            case 524288:
            case 1048576:
            case 2097152:
              return lanes & 3932160;
            case 4194304:
            case 8388608:
            case 16777216:
            case 33554432:
              return lanes & 62914560;
            case 67108864:
              return 67108864;
            case 134217728:
              return 134217728;
            case 268435456:
              return 268435456;
            case 536870912:
              return 536870912;
            case 1073741824:
              return 0;
            default:
              return lanes;
          }
        }
        function getNextLanes(root2, wipLanes, rootHasPendingCommit) {
          var pendingLanes = root2.pendingLanes;
          if (0 === pendingLanes) return 0;
          var nextLanes = 0, suspendedLanes = root2.suspendedLanes, pingedLanes = root2.pingedLanes;
          root2 = root2.warmLanes;
          var nonIdlePendingLanes = pendingLanes & 134217727;
          0 !== nonIdlePendingLanes ? (pendingLanes = nonIdlePendingLanes & ~suspendedLanes, 0 !== pendingLanes ? nextLanes = getHighestPriorityLanes(pendingLanes) : (pingedLanes &= nonIdlePendingLanes, 0 !== pingedLanes ? nextLanes = getHighestPriorityLanes(pingedLanes) : rootHasPendingCommit || (rootHasPendingCommit = nonIdlePendingLanes & ~root2, 0 !== rootHasPendingCommit && (nextLanes = getHighestPriorityLanes(rootHasPendingCommit))))) : (nonIdlePendingLanes = pendingLanes & ~suspendedLanes, 0 !== nonIdlePendingLanes ? nextLanes = getHighestPriorityLanes(nonIdlePendingLanes) : 0 !== pingedLanes ? nextLanes = getHighestPriorityLanes(pingedLanes) : rootHasPendingCommit || (rootHasPendingCommit = pendingLanes & ~root2, 0 !== rootHasPendingCommit && (nextLanes = getHighestPriorityLanes(rootHasPendingCommit))));
          return 0 === nextLanes ? 0 : 0 !== wipLanes && wipLanes !== nextLanes && 0 === (wipLanes & suspendedLanes) && (suspendedLanes = nextLanes & -nextLanes, rootHasPendingCommit = wipLanes & -wipLanes, suspendedLanes >= rootHasPendingCommit || 32 === suspendedLanes && 0 !== (rootHasPendingCommit & 4194048)) ? wipLanes : nextLanes;
        }
        function checkIfRootIsPrerendering(root2, renderLanes2) {
          return 0 === (root2.pendingLanes & ~(root2.suspendedLanes & ~root2.pingedLanes) & renderLanes2);
        }
        function getEntangledLanes(root2, renderLanes2) {
          0 !== (renderLanes2 & 8) && (renderLanes2 |= renderLanes2 & 32);
          var allEntangledLanes = root2.entangledLanes;
          if (0 !== allEntangledLanes)
            for (root2 = root2.entanglements, allEntangledLanes &= renderLanes2; 0 < allEntangledLanes; ) {
              var index$2 = 31 - clz32(allEntangledLanes), lane = 1 << index$2;
              renderLanes2 |= root2[index$2];
              allEntangledLanes &= ~lane;
            }
          return renderLanes2;
        }
        function computeExpirationTime(lane, currentTime) {
          switch (lane) {
            case 1:
            case 2:
            case 4:
            case 8:
            case 64:
              return currentTime + 250;
            case 16:
            case 32:
            case 128:
            case 256:
            case 512:
            case 1024:
            case 2048:
            case 4096:
            case 8192:
            case 16384:
            case 32768:
            case 65536:
            case 131072:
            case 262144:
            case 524288:
            case 1048576:
            case 2097152:
              return currentTime + 5e3;
            case 4194304:
            case 8388608:
            case 16777216:
            case 33554432:
              return -1;
            case 67108864:
            case 134217728:
            case 268435456:
            case 536870912:
            case 1073741824:
              return -1;
            default:
              return -1;
          }
        }
        function claimNextRetryLane() {
          var lane = nextRetryLane;
          nextRetryLane <<= 1;
          0 === (nextRetryLane & 62914560) && (nextRetryLane = 4194304);
          return lane;
        }
        function createLaneMap(initial) {
          for (var laneMap = [], i = 0; 31 > i; i++) laneMap.push(initial);
          return laneMap;
        }
        function markRootUpdated$1(root2, updateLane) {
          root2.pendingLanes |= updateLane;
          268435456 !== updateLane && (root2.suspendedLanes = 0, root2.pingedLanes = 0, root2.warmLanes = 0);
        }
        function markRootFinished(root2, finishedLanes, remainingLanes, spawnedLane, updatedLanes, suspendedRetryLanes) {
          var previouslyPendingLanes = root2.pendingLanes;
          root2.pendingLanes = remainingLanes;
          root2.suspendedLanes = 0;
          root2.pingedLanes = 0;
          root2.warmLanes = 0;
          root2.expiredLanes &= remainingLanes;
          root2.entangledLanes &= remainingLanes;
          root2.errorRecoveryDisabledLanes &= remainingLanes;
          root2.shellSuspendCounter = 0;
          var entanglements = root2.entanglements, expirationTimes = root2.expirationTimes, hiddenUpdates = root2.hiddenUpdates;
          for (remainingLanes = previouslyPendingLanes & ~remainingLanes; 0 < remainingLanes; ) {
            var index$5 = 31 - clz32(remainingLanes), lane = 1 << index$5;
            entanglements[index$5] = 0;
            expirationTimes[index$5] = -1;
            var hiddenUpdatesForLane = hiddenUpdates[index$5];
            if (null !== hiddenUpdatesForLane)
              for (hiddenUpdates[index$5] = null, index$5 = 0; index$5 < hiddenUpdatesForLane.length; index$5++) {
                var update = hiddenUpdatesForLane[index$5];
                null !== update && (update.lane &= -536870913);
              }
            remainingLanes &= ~lane;
          }
          0 !== spawnedLane && markSpawnedDeferredLane(root2, spawnedLane, 0);
          0 !== suspendedRetryLanes && 0 === updatedLanes && 0 !== root2.tag && (root2.suspendedLanes |= suspendedRetryLanes & ~(previouslyPendingLanes & ~finishedLanes));
        }
        function markSpawnedDeferredLane(root2, spawnedLane, entangledLanes) {
          root2.pendingLanes |= spawnedLane;
          root2.suspendedLanes &= ~spawnedLane;
          var spawnedLaneIndex = 31 - clz32(spawnedLane);
          root2.entangledLanes |= spawnedLane;
          root2.entanglements[spawnedLaneIndex] = root2.entanglements[spawnedLaneIndex] | 1073741824 | entangledLanes & 261930;
        }
        function markRootEntangled(root2, entangledLanes) {
          var rootEntangledLanes = root2.entangledLanes |= entangledLanes;
          for (root2 = root2.entanglements; rootEntangledLanes; ) {
            var index$6 = 31 - clz32(rootEntangledLanes), lane = 1 << index$6;
            lane & entangledLanes | root2[index$6] & entangledLanes && (root2[index$6] |= entangledLanes);
            rootEntangledLanes &= ~lane;
          }
        }
        function getBumpedLaneForHydration(root2, renderLanes2) {
          var renderLane = renderLanes2 & -renderLanes2;
          renderLane = 0 !== (renderLane & 42) ? 1 : getBumpedLaneForHydrationByLane(renderLane);
          return 0 !== (renderLane & (root2.suspendedLanes | renderLanes2)) ? 0 : renderLane;
        }
        function getBumpedLaneForHydrationByLane(lane) {
          switch (lane) {
            case 2:
              lane = 1;
              break;
            case 8:
              lane = 4;
              break;
            case 32:
              lane = 16;
              break;
            case 256:
            case 512:
            case 1024:
            case 2048:
            case 4096:
            case 8192:
            case 16384:
            case 32768:
            case 65536:
            case 131072:
            case 262144:
            case 524288:
            case 1048576:
            case 2097152:
            case 4194304:
            case 8388608:
            case 16777216:
            case 33554432:
              lane = 128;
              break;
            case 268435456:
              lane = 134217728;
              break;
            default:
              lane = 0;
          }
          return lane;
        }
        function lanesToEventPriority(lanes) {
          lanes &= -lanes;
          return 2 < lanes ? 8 < lanes ? 0 !== (lanes & 134217727) ? 32 : 268435456 : 8 : 2;
        }
        function setIsStrictModeForDevtools(newIsStrictMode) {
          "function" === typeof log2 && unstable_setDisableYieldValue(newIsStrictMode);
          if (injectedHook && "function" === typeof injectedHook.setStrictMode)
            try {
              injectedHook.setStrictMode(rendererID, newIsStrictMode);
            } catch (err) {
            }
        }
        function getViewTransitionName(props, instance) {
          if (null != props.name && "auto" !== props.name) return props.name;
          if (null !== instance.autoName) return instance.autoName;
          props = pendingEffectsRoot.identifierPrefix;
          var globalClientId = globalClientIdCounter$1++;
          props = "_" + props + "t_" + globalClientId.toString(32) + "_";
          return instance.autoName = props;
        }
        function getClassNameByType(classByType) {
          if (null == classByType || "string" === typeof classByType)
            return classByType;
          var className = null, activeTypes = pendingTransitionTypes;
          if (null !== activeTypes)
            for (var i = 0; i < activeTypes.length; i++) {
              var match = classByType[activeTypes[i]];
              if (null != match) {
                if ("none" === match) return "none";
                className = null == className ? match : className + (" " + match);
              }
            }
          return null == className ? classByType.default : className;
        }
        function getViewTransitionClassName(defaultClass, eventClass) {
          defaultClass = getClassNameByType(defaultClass);
          eventClass = getClassNameByType(eventClass);
          return null == eventClass ? "auto" === defaultClass ? null : defaultClass : "auto" === eventClass ? null : eventClass;
        }
        function is(x, y) {
          return x === y && (0 !== x || 1 / x === 1 / y) || x !== x && y !== y;
        }
        function describeBuiltInComponentFrame(name) {
          if (void 0 === prefix)
            try {
              throw Error();
            } catch (x) {
              var match = x.stack.trim().match(/\n( *(at )?)/);
              prefix = match && match[1] || "";
              suffix = -1 < x.stack.indexOf("\n    at") ? " (<anonymous>)" : -1 < x.stack.indexOf("@") ? "@unknown:0:0" : "";
            }
          return "\n" + prefix + name + suffix;
        }
        function describeNativeComponentFrame(fn, construct) {
          if (!fn || reentry) return "";
          reentry = true;
          var previousPrepareStackTrace = Error.prepareStackTrace;
          Error.prepareStackTrace = void 0;
          try {
            var RunInRootFrame = {
              DetermineComponentFrameRoot: function() {
                try {
                  if (construct) {
                    var Fake = function() {
                      throw Error();
                    };
                    Object.defineProperty(Fake.prototype, "props", {
                      set: function() {
                        throw Error();
                      }
                    });
                    if ("object" === typeof Reflect && Reflect.construct) {
                      try {
                        Reflect.construct(Fake, []);
                      } catch (x) {
                        var control = x;
                      }
                      Reflect.construct(fn, [], Fake);
                    } else {
                      try {
                        Fake.call();
                      } catch (x$8) {
                        control = x$8;
                      }
                      Fake = false;
                      try {
                        var prevProps = Object.getOwnPropertyDescriptor(
                          fn.prototype,
                          "props"
                        );
                        Object.defineProperty(fn.prototype, "props", {
                          configurable: true,
                          set: function() {
                            throw Error();
                          }
                        });
                        Fake = true;
                        new fn();
                      } finally {
                        Fake && (void 0 !== prevProps ? Object.defineProperty(fn.prototype, "props", prevProps) : delete fn.prototype.props);
                      }
                    }
                  } else {
                    try {
                      throw Error();
                    } catch (x$9) {
                      control = x$9;
                    }
                    (Fake = fn()) && "function" === typeof Fake.catch && Fake.catch(function() {
                    });
                  }
                } catch (sample) {
                  if (sample && control && "string" === typeof sample.stack)
                    return [sample.stack, control.stack];
                }
                return [null, null];
              }
            };
            RunInRootFrame.DetermineComponentFrameRoot.displayName = "DetermineComponentFrameRoot";
            var namePropDescriptor = Object.getOwnPropertyDescriptor(
              RunInRootFrame.DetermineComponentFrameRoot,
              "name"
            );
            namePropDescriptor && namePropDescriptor.configurable && Object.defineProperty(
              RunInRootFrame.DetermineComponentFrameRoot,
              "name",
              { value: "DetermineComponentFrameRoot" }
            );
            var _RunInRootFrame$Deter = RunInRootFrame.DetermineComponentFrameRoot(), sampleStack = _RunInRootFrame$Deter[0], controlStack = _RunInRootFrame$Deter[1];
            if (sampleStack && controlStack) {
              var sampleLines = sampleStack.split("\n"), controlLines = controlStack.split("\n");
              for (namePropDescriptor = RunInRootFrame = 0; RunInRootFrame < sampleLines.length && !sampleLines[RunInRootFrame].includes("DetermineComponentFrameRoot"); )
                RunInRootFrame++;
              for (; namePropDescriptor < controlLines.length && !controlLines[namePropDescriptor].includes(
                "DetermineComponentFrameRoot"
              ); )
                namePropDescriptor++;
              if (RunInRootFrame === sampleLines.length || namePropDescriptor === controlLines.length)
                for (RunInRootFrame = sampleLines.length - 1, namePropDescriptor = controlLines.length - 1; 1 <= RunInRootFrame && 0 <= namePropDescriptor && sampleLines[RunInRootFrame] !== controlLines[namePropDescriptor]; )
                  namePropDescriptor--;
              for (; 1 <= RunInRootFrame && 0 <= namePropDescriptor; RunInRootFrame--, namePropDescriptor--)
                if (sampleLines[RunInRootFrame] !== controlLines[namePropDescriptor]) {
                  if (1 !== RunInRootFrame || 1 !== namePropDescriptor) {
                    do
                      if (RunInRootFrame--, namePropDescriptor--, 0 > namePropDescriptor || sampleLines[RunInRootFrame] !== controlLines[namePropDescriptor]) {
                        var frame = "\n" + sampleLines[RunInRootFrame].replace(" at new ", " at ");
                        fn.displayName && frame.includes("<anonymous>") && (frame = frame.replace("<anonymous>", fn.displayName));
                        return frame;
                      }
                    while (1 <= RunInRootFrame && 0 <= namePropDescriptor);
                  }
                  break;
                }
            }
          } finally {
            reentry = false, Error.prepareStackTrace = previousPrepareStackTrace;
          }
          return (previousPrepareStackTrace = fn ? fn.displayName || fn.name : "") ? describeBuiltInComponentFrame(previousPrepareStackTrace) : "";
        }
        function describeFiber(fiber, childFiber) {
          switch (fiber.tag) {
            case 26:
            case 27:
            case 5:
              return describeBuiltInComponentFrame(fiber.type);
            case 16:
              return describeBuiltInComponentFrame("Lazy");
            case 13:
              return fiber.child !== childFiber && null !== childFiber ? describeBuiltInComponentFrame("Suspense Fallback") : describeBuiltInComponentFrame("Suspense");
            case 19:
              return describeBuiltInComponentFrame("SuspenseList");
            case 0:
            case 15:
              return describeNativeComponentFrame(fiber.type, false);
            case 11:
              return describeNativeComponentFrame(fiber.type.render, false);
            case 1:
              return describeNativeComponentFrame(fiber.type, true);
            case 31:
              return describeBuiltInComponentFrame("Activity");
            case 30:
              return describeBuiltInComponentFrame("ViewTransition");
            default:
              return "";
          }
        }
        function getStackByFiberInDevAndProd(workInProgress2) {
          try {
            var info = "", previous = null;
            do
              info += describeFiber(workInProgress2, previous), previous = workInProgress2, workInProgress2 = workInProgress2.return;
            while (workInProgress2);
            return info;
          } catch (x) {
            return "\nError generating stack: " + x.message + "\n" + x.stack;
          }
        }
        function createCapturedValueAtFiber(value, source) {
          if ("object" === typeof value && null !== value) {
            var existing = CapturedStacks.get(value);
            if (void 0 !== existing) return existing;
            source = {
              value,
              source,
              stack: getStackByFiberInDevAndProd(source)
            };
            CapturedStacks.set(value, source);
            return source;
          }
          return {
            value,
            source,
            stack: getStackByFiberInDevAndProd(source)
          };
        }
        function pushTreeFork(workInProgress2, totalChildren) {
          forkStack[forkStackIndex++] = treeForkCount;
          forkStack[forkStackIndex++] = treeForkProvider;
          treeForkProvider = workInProgress2;
          treeForkCount = totalChildren;
        }
        function pushTreeId(workInProgress2, totalChildren, index) {
          idStack[idStackIndex++] = treeContextId;
          idStack[idStackIndex++] = treeContextOverflow;
          idStack[idStackIndex++] = treeContextProvider;
          treeContextProvider = workInProgress2;
          var baseIdWithLeadingBit = treeContextId;
          workInProgress2 = treeContextOverflow;
          var baseLength = 32 - clz32(baseIdWithLeadingBit) - 1;
          baseIdWithLeadingBit &= ~(1 << baseLength);
          index += 1;
          var length = 32 - clz32(totalChildren) + baseLength;
          if (30 < length) {
            var numberOfOverflowBits = baseLength - baseLength % 5;
            length = (baseIdWithLeadingBit & (1 << numberOfOverflowBits) - 1).toString(32);
            baseIdWithLeadingBit >>= numberOfOverflowBits;
            baseLength -= numberOfOverflowBits;
            treeContextId = 1 << 32 - clz32(totalChildren) + baseLength | index << baseLength | baseIdWithLeadingBit;
            treeContextOverflow = length + workInProgress2;
          } else
            treeContextId = 1 << length | index << baseLength | baseIdWithLeadingBit, treeContextOverflow = workInProgress2;
        }
        function pushMaterializedTreeId(workInProgress2) {
          null !== workInProgress2.return && (pushTreeFork(workInProgress2, 1), pushTreeId(workInProgress2, 1, 0));
        }
        function popTreeContext(workInProgress2) {
          for (; workInProgress2 === treeForkProvider; )
            treeForkProvider = forkStack[--forkStackIndex], forkStack[forkStackIndex] = null, treeForkCount = forkStack[--forkStackIndex], forkStack[forkStackIndex] = null;
          for (; workInProgress2 === treeContextProvider; )
            treeContextProvider = idStack[--idStackIndex], idStack[idStackIndex] = null, treeContextOverflow = idStack[--idStackIndex], idStack[idStackIndex] = null, treeContextId = idStack[--idStackIndex], idStack[idStackIndex] = null;
        }
        function restoreSuspendedTreeContext(workInProgress2, suspendedContext) {
          idStack[idStackIndex++] = treeContextId;
          idStack[idStackIndex++] = treeContextOverflow;
          idStack[idStackIndex++] = treeContextProvider;
          treeContextId = suspendedContext.id;
          treeContextOverflow = suspendedContext.overflow;
          treeContextProvider = workInProgress2;
        }
        function pushHostContainer(fiber, nextRootInstance) {
          push(rootInstanceStackCursor, nextRootInstance);
          push(contextFiberStackCursor, fiber);
          push(contextStackCursor, null);
          fiber = getRootHostContext(nextRootInstance);
          pop(contextStackCursor);
          push(contextStackCursor, fiber);
        }
        function popHostContainer() {
          pop(contextStackCursor);
          pop(contextFiberStackCursor);
          pop(rootInstanceStackCursor);
        }
        function pushHostContext(fiber) {
          var stateHook = fiber.memoizedState;
          null !== stateHook && (stateHook = stateHook.memoizedState, isPrimaryRenderer ? HostTransitionContext._currentValue = stateHook : HostTransitionContext._currentValue2 = stateHook, push(hostTransitionProviderCursor, fiber));
          stateHook = contextStackCursor.current;
          var nextContext = getChildHostContext(stateHook, fiber.type);
          stateHook !== nextContext && (push(contextFiberStackCursor, fiber), push(contextStackCursor, nextContext));
        }
        function popHostContext(fiber) {
          contextFiberStackCursor.current === fiber && (pop(contextStackCursor), pop(contextFiberStackCursor));
          hostTransitionProviderCursor.current === fiber && (pop(hostTransitionProviderCursor), isPrimaryRenderer ? HostTransitionContext._currentValue = NotPendingTransition : HostTransitionContext._currentValue2 = NotPendingTransition);
        }
        function throwOnHydrationMismatch(fiber) {
          var error = Error(
            formatProdErrorMessage(
              418,
              1 < arguments.length && void 0 !== arguments[1] && arguments[1] ? "text" : "HTML",
              ""
            )
          );
          queueHydrationError(createCapturedValueAtFiber(error, fiber));
          throw HydrationMismatchException;
        }
        function prepareToHydrateHostInstance(fiber, hostContext) {
          if (!supportsHydration) throw Error(formatProdErrorMessage(175));
          hydrateInstance(
            fiber.stateNode,
            fiber.type,
            fiber.memoizedProps,
            hostContext,
            fiber
          ) || throwOnHydrationMismatch(fiber, true);
        }
        function popToNextHostParent(fiber) {
          for (hydrationParentFiber = fiber.return; hydrationParentFiber; )
            switch (hydrationParentFiber.tag) {
              case 5:
              case 31:
              case 13:
                rootOrSingletonContext = false;
                return;
              case 27:
              case 3:
                rootOrSingletonContext = true;
                return;
              default:
                hydrationParentFiber = hydrationParentFiber.return;
            }
        }
        function popHydrationState(fiber) {
          if (!supportsHydration || fiber !== hydrationParentFiber) return false;
          if (!isHydrating) return popToNextHostParent(fiber), isHydrating = true, false;
          var tag = fiber.tag;
          supportsSingletons ? 3 !== tag && 27 !== tag && (5 !== tag || shouldDeleteUnhydratedTailInstances(fiber.type) && !shouldSetTextContent(fiber.type, fiber.memoizedProps)) && nextHydratableInstance && throwOnHydrationMismatch(fiber) : 3 !== tag && (5 !== tag || shouldDeleteUnhydratedTailInstances(fiber.type) && !shouldSetTextContent(fiber.type, fiber.memoizedProps)) && nextHydratableInstance && throwOnHydrationMismatch(fiber);
          popToNextHostParent(fiber);
          if (13 === tag) {
            if (!supportsHydration) throw Error(formatProdErrorMessage(316));
            fiber = fiber.memoizedState;
            fiber = null !== fiber ? fiber.dehydrated : null;
            if (!fiber) throw Error(formatProdErrorMessage(317));
            nextHydratableInstance = getNextHydratableInstanceAfterSuspenseInstance(fiber);
          } else if (31 === tag) {
            fiber = fiber.memoizedState;
            fiber = null !== fiber ? fiber.dehydrated : null;
            if (!fiber) throw Error(formatProdErrorMessage(317));
            nextHydratableInstance = getNextHydratableInstanceAfterActivityInstance(fiber);
          } else
            nextHydratableInstance = supportsSingletons && 27 === tag ? getNextHydratableSiblingAfterSingleton(
              fiber.type,
              nextHydratableInstance
            ) : hydrationParentFiber ? getNextHydratableSibling(fiber.stateNode) : null;
          return true;
        }
        function resetHydrationState() {
          supportsHydration && (nextHydratableInstance = hydrationParentFiber = null, isHydrating = false);
        }
        function upgradeHydrationErrorsToRecoverable() {
          var queuedErrors = hydrationErrors;
          null !== queuedErrors && (null === workInProgressRootRecoverableErrors ? workInProgressRootRecoverableErrors = queuedErrors : workInProgressRootRecoverableErrors.push.apply(
            workInProgressRootRecoverableErrors,
            queuedErrors
          ), hydrationErrors = null);
          return queuedErrors;
        }
        function queueHydrationError(error) {
          null === hydrationErrors ? hydrationErrors = [error] : hydrationErrors.push(error);
        }
        function pushProvider(providerFiber, context, nextValue) {
          isPrimaryRenderer ? (push(valueCursor, context._currentValue), context._currentValue = nextValue) : (push(valueCursor, context._currentValue2), context._currentValue2 = nextValue);
        }
        function popProvider(context) {
          var currentValue = valueCursor.current;
          isPrimaryRenderer ? context._currentValue = currentValue : context._currentValue2 = currentValue;
          pop(valueCursor);
        }
        function scheduleContextWorkOnParentPath(parent, renderLanes2, propagationRoot) {
          for (; null !== parent; ) {
            var alternate = parent.alternate;
            (parent.childLanes & renderLanes2) !== renderLanes2 ? (parent.childLanes |= renderLanes2, null !== alternate && (alternate.childLanes |= renderLanes2)) : null !== alternate && (alternate.childLanes & renderLanes2) !== renderLanes2 && (alternate.childLanes |= renderLanes2);
            if (parent === propagationRoot) break;
            parent = parent.return;
          }
        }
        function propagateContextChanges(workInProgress2, contexts, renderLanes2, forcePropagateEntireTree) {
          var fiber = workInProgress2.child;
          null !== fiber && (fiber.return = workInProgress2);
          for (; null !== fiber; ) {
            var list = fiber.dependencies;
            if (null !== list) {
              var nextFiber = fiber.child;
              list = list.firstContext;
              a: for (; null !== list; ) {
                var dependency = list;
                list = fiber;
                for (var i = 0; i < contexts.length; i++)
                  if (dependency.context === contexts[i]) {
                    list.lanes |= renderLanes2;
                    dependency = list.alternate;
                    null !== dependency && (dependency.lanes |= renderLanes2);
                    scheduleContextWorkOnParentPath(
                      list.return,
                      renderLanes2,
                      workInProgress2
                    );
                    forcePropagateEntireTree || (nextFiber = null);
                    break a;
                  }
                list = dependency.next;
              }
            } else if (18 === fiber.tag) {
              nextFiber = fiber.return;
              if (null === nextFiber) throw Error(formatProdErrorMessage(341));
              nextFiber.lanes |= renderLanes2;
              list = nextFiber.alternate;
              null !== list && (list.lanes |= renderLanes2);
              scheduleContextWorkOnParentPath(nextFiber, renderLanes2, workInProgress2);
              nextFiber = null;
            } else
              13 === fiber.tag && null !== fiber.memoizedState && null === fiber.memoizedState.dehydrated ? (fiber.lanes |= renderLanes2, nextFiber = fiber.alternate, null !== nextFiber && (nextFiber.lanes |= renderLanes2), scheduleContextWorkOnParentPath(
                fiber.return,
                renderLanes2,
                workInProgress2
              ), nextFiber = fiber.child, nextFiber = null !== nextFiber ? nextFiber.sibling : null) : nextFiber = fiber.child;
            if (null !== nextFiber) nextFiber.return = fiber;
            else
              for (nextFiber = fiber; null !== nextFiber; ) {
                if (nextFiber === workInProgress2) {
                  nextFiber = null;
                  break;
                }
                fiber = nextFiber.sibling;
                if (null !== fiber) {
                  fiber.return = nextFiber.return;
                  nextFiber = fiber;
                  break;
                }
                nextFiber = nextFiber.return;
              }
            fiber = nextFiber;
          }
        }
        function propagateParentContextChanges(current, workInProgress2, renderLanes2, forcePropagateEntireTree) {
          current = null;
          for (var parent = workInProgress2, isInsidePropagationBailout = false; null !== parent; ) {
            if (!isInsidePropagationBailout) {
              if (0 !== (parent.flags & 524288)) isInsidePropagationBailout = true;
              else if (0 !== (parent.flags & 262144)) break;
            }
            if (10 === parent.tag) {
              var currentParent = parent.alternate;
              if (null === currentParent) throw Error(formatProdErrorMessage(387));
              currentParent = currentParent.memoizedProps;
              if (null !== currentParent) {
                var context = parent.type;
                objectIs(parent.pendingProps.value, currentParent.value) || (null !== current ? current.push(context) : current = [context]);
              }
            } else if (parent === hostTransitionProviderCursor.current) {
              currentParent = parent.alternate;
              if (null === currentParent) throw Error(formatProdErrorMessage(387));
              currentParent.memoizedState.memoizedState !== parent.memoizedState.memoizedState && (null !== current ? current.push(HostTransitionContext) : current = [HostTransitionContext]);
            }
            parent = parent.return;
          }
          null !== current && propagateContextChanges(
            workInProgress2,
            current,
            renderLanes2,
            forcePropagateEntireTree
          );
          workInProgress2.flags |= 262144;
          return null !== current;
        }
        function checkIfContextChanged(currentDependencies) {
          for (currentDependencies = currentDependencies.firstContext; null !== currentDependencies; ) {
            var context = currentDependencies.context;
            if (!objectIs(
              isPrimaryRenderer ? context._currentValue : context._currentValue2,
              currentDependencies.memoizedValue
            ))
              return true;
            currentDependencies = currentDependencies.next;
          }
          return false;
        }
        function prepareToReadContext(workInProgress2) {
          currentlyRenderingFiber$1 = workInProgress2;
          lastContextDependency = null;
          workInProgress2 = workInProgress2.dependencies;
          null !== workInProgress2 && (workInProgress2.firstContext = null);
        }
        function readContext(context) {
          return readContextForConsumer(currentlyRenderingFiber$1, context);
        }
        function readContextDuringReconciliation(consumer, context) {
          null === currentlyRenderingFiber$1 && prepareToReadContext(consumer);
          return readContextForConsumer(consumer, context);
        }
        function readContextForConsumer(consumer, context) {
          var value = isPrimaryRenderer ? context._currentValue : context._currentValue2;
          context = { context, memoizedValue: value, next: null };
          if (null === lastContextDependency) {
            if (null === consumer) throw Error(formatProdErrorMessage(308));
            lastContextDependency = context;
            consumer.dependencies = { lanes: 0, firstContext: context };
            consumer.flags |= 524288;
          } else lastContextDependency = lastContextDependency.next = context;
          return value;
        }
        function createCache() {
          return {
            controller: new AbortControllerLocal(),
            data: /* @__PURE__ */ new Map(),
            refCount: 0
          };
        }
        function releaseCache(cache) {
          cache.refCount--;
          0 === cache.refCount && scheduleCallback$2(NormalPriority, function() {
            cache.controller.abort();
          });
        }
        function queueTransitionTypes(root2, transitionTypes) {
          if (0 !== (root2.pendingLanes & 4194048)) {
            var queued = root2.transitionTypes;
            null === queued && (queued = root2.transitionTypes = []);
            for (root2 = 0; root2 < transitionTypes.length; root2++) {
              var transitionType = transitionTypes[root2];
              -1 === queued.indexOf(transitionType) && queued.push(transitionType);
            }
          }
        }
        function claimQueuedTransitionTypes(root2) {
          var claimed = root2.transitionTypes;
          root2.transitionTypes = null;
          return claimed;
        }
        function noop$1() {
        }
        function ensureRootIsScheduled(root2) {
          root2 !== lastScheduledRoot && null === root2.next && (null === lastScheduledRoot ? firstScheduledRoot = lastScheduledRoot = root2 : lastScheduledRoot = lastScheduledRoot.next = root2);
          mightHavePendingSyncWork = true;
          didScheduleMicrotask || (didScheduleMicrotask = true, scheduleImmediateRootScheduleTask());
        }
        function flushSyncWorkAcrossRoots_impl(syncTransitionLanes, onlyLegacy) {
          if (!isFlushingWork && mightHavePendingSyncWork) {
            isFlushingWork = true;
            do {
              var didPerformSomeWork = false;
              for (var root2 = firstScheduledRoot; null !== root2; ) {
                if (!onlyLegacy)
                  if (0 !== syncTransitionLanes) {
                    var pendingLanes = root2.pendingLanes;
                    if (0 === pendingLanes) var JSCompiler_inline_result = 0;
                    else {
                      var suspendedLanes = root2.suspendedLanes, pingedLanes = root2.pingedLanes;
                      JSCompiler_inline_result = (1 << 31 - clz32(42 | syncTransitionLanes) + 1) - 1;
                      JSCompiler_inline_result &= pendingLanes & ~(suspendedLanes & ~pingedLanes);
                      JSCompiler_inline_result = JSCompiler_inline_result & 201326741 ? JSCompiler_inline_result & 201326741 | 1 : JSCompiler_inline_result ? JSCompiler_inline_result | 2 : 0;
                    }
                    0 !== JSCompiler_inline_result && (didPerformSomeWork = true, performSyncWorkOnRoot(root2, JSCompiler_inline_result));
                  } else
                    JSCompiler_inline_result = workInProgressRootRenderLanes, JSCompiler_inline_result = getNextLanes(
                      root2,
                      root2 === workInProgressRoot ? JSCompiler_inline_result : 0,
                      null !== root2.cancelPendingCommit || root2.timeoutHandle !== noTimeout
                    ), 0 === (JSCompiler_inline_result & 3) || checkIfRootIsPrerendering(root2, JSCompiler_inline_result) || (didPerformSomeWork = true, performSyncWorkOnRoot(root2, JSCompiler_inline_result));
                root2 = root2.next;
              }
            } while (didPerformSomeWork);
            isFlushingWork = false;
          }
        }
        function processRootScheduleInImmediateTask() {
          processRootScheduleInMicrotask();
        }
        function processRootScheduleInMicrotask() {
          mightHavePendingSyncWork = didScheduleMicrotask = false;
          var syncTransitionLanes = 0;
          0 !== currentEventTransitionLane && shouldAttemptEagerTransition() && (syncTransitionLanes = currentEventTransitionLane);
          for (var currentTime = now2(), prev = null, root2 = firstScheduledRoot; null !== root2; ) {
            var next = root2.next, nextLanes = scheduleTaskForRootDuringMicrotask(root2, currentTime);
            if (0 === nextLanes)
              root2.next = null, null === prev ? firstScheduledRoot = next : prev.next = next, null === next && (lastScheduledRoot = prev);
            else if (prev = root2, 0 !== syncTransitionLanes || 0 !== (nextLanes & 3))
              mightHavePendingSyncWork = true;
            root2 = next;
          }
          0 !== pendingEffectsStatus && 5 !== pendingEffectsStatus || flushSyncWorkAcrossRoots_impl(syncTransitionLanes, false);
          0 !== currentEventTransitionLane && (currentEventTransitionLane = 0);
        }
        function scheduleTaskForRootDuringMicrotask(root2, currentTime) {
          for (var suspendedLanes = root2.suspendedLanes, pingedLanes = root2.pingedLanes, expirationTimes = root2.expirationTimes, lanes = root2.pendingLanes & -62914561; 0 < lanes; ) {
            var index$3 = 31 - clz32(lanes), lane = 1 << index$3, expirationTime = expirationTimes[index$3];
            if (-1 === expirationTime) {
              if (0 === (lane & suspendedLanes) || 0 !== (lane & pingedLanes))
                expirationTimes[index$3] = computeExpirationTime(lane, currentTime);
            } else expirationTime <= currentTime && (root2.expiredLanes |= lane);
            lanes &= ~lane;
          }
          currentTime = workInProgressRoot;
          suspendedLanes = workInProgressRootRenderLanes;
          suspendedLanes = getNextLanes(
            root2,
            root2 === currentTime ? suspendedLanes : 0,
            null !== root2.cancelPendingCommit || root2.timeoutHandle !== noTimeout
          );
          pingedLanes = root2.callbackNode;
          if (0 === suspendedLanes || root2 === currentTime && (2 === workInProgressSuspendedReason || 9 === workInProgressSuspendedReason) || null !== root2.cancelPendingCommit)
            return null !== pingedLanes && null !== pingedLanes && cancelCallback$1(pingedLanes), root2.callbackNode = null, root2.callbackPriority = 0;
          if (0 === (suspendedLanes & 3) || checkIfRootIsPrerendering(root2, suspendedLanes)) {
            currentTime = suspendedLanes & -suspendedLanes;
            if (currentTime === root2.callbackPriority) return currentTime;
            null !== pingedLanes && cancelCallback$1(pingedLanes);
            switch (lanesToEventPriority(suspendedLanes)) {
              case 2:
              case 8:
                suspendedLanes = UserBlockingPriority;
                break;
              case 32:
                suspendedLanes = NormalPriority$1;
                break;
              case 268435456:
                suspendedLanes = IdlePriority;
                break;
              default:
                suspendedLanes = NormalPriority$1;
            }
            pingedLanes = performWorkOnRootViaSchedulerTask.bind(null, root2);
            suspendedLanes = scheduleCallback$3(suspendedLanes, pingedLanes);
            root2.callbackPriority = currentTime;
            root2.callbackNode = suspendedLanes;
            return currentTime;
          }
          null !== pingedLanes && null !== pingedLanes && cancelCallback$1(pingedLanes);
          root2.callbackPriority = 2;
          root2.callbackNode = null;
          return 2;
        }
        function performWorkOnRootViaSchedulerTask(root2, didTimeout) {
          if (0 !== pendingEffectsStatus && 5 !== pendingEffectsStatus)
            return root2.callbackNode = null, root2.callbackPriority = 0, null;
          var originalCallbackNode = root2.callbackNode;
          if (flushPendingEffects() && root2.callbackNode !== originalCallbackNode)
            return null;
          var workInProgressRootRenderLanes$jscomp$0 = workInProgressRootRenderLanes;
          workInProgressRootRenderLanes$jscomp$0 = getNextLanes(
            root2,
            root2 === workInProgressRoot ? workInProgressRootRenderLanes$jscomp$0 : 0,
            null !== root2.cancelPendingCommit || root2.timeoutHandle !== noTimeout
          );
          if (0 === workInProgressRootRenderLanes$jscomp$0) return null;
          performWorkOnRoot(root2, workInProgressRootRenderLanes$jscomp$0, didTimeout);
          scheduleTaskForRootDuringMicrotask(root2, now2());
          return null != root2.callbackNode && root2.callbackNode === originalCallbackNode ? performWorkOnRootViaSchedulerTask.bind(null, root2) : null;
        }
        function performSyncWorkOnRoot(root2, lanes) {
          if (flushPendingEffects()) return null;
          performWorkOnRoot(root2, lanes, true);
        }
        function scheduleImmediateRootScheduleTask() {
          supportsMicrotasks ? scheduleMicrotask(function() {
            0 !== (executionContext & 6) ? scheduleCallback$3(
              ImmediatePriority,
              processRootScheduleInImmediateTask
            ) : processRootScheduleInMicrotask();
          }) : scheduleCallback$3(
            ImmediatePriority,
            processRootScheduleInImmediateTask
          );
        }
        function requestTransitionLane() {
          if (0 === currentEventTransitionLane) {
            var actionScopeLane = currentEntangledLane;
            0 === actionScopeLane && (actionScopeLane = nextTransitionUpdateLane, nextTransitionUpdateLane <<= 1, 0 === (nextTransitionUpdateLane & 261888) && (nextTransitionUpdateLane = 256));
            currentEventTransitionLane = actionScopeLane;
          }
          return currentEventTransitionLane;
        }
        function entangleAsyncAction(transition, thenable) {
          if (null === currentEntangledListeners) {
            var entangledListeners = currentEntangledListeners = [];
            currentEntangledPendingCount = 0;
            currentEntangledLane = requestTransitionLane();
            currentEntangledActionThenable = {
              status: "pending",
              value: void 0,
              then: function(resolve) {
                entangledListeners.push(resolve);
              }
            };
          }
          currentEntangledPendingCount++;
          thenable.then(pingEngtangledActionScope, pingEngtangledActionScope);
          return thenable;
        }
        function pingEngtangledActionScope() {
          if (0 === --currentEntangledPendingCount && (entangledTransitionTypes = null, null !== currentEntangledListeners)) {
            null !== currentEntangledActionThenable && (currentEntangledActionThenable.status = "fulfilled");
            var listeners = currentEntangledListeners;
            currentEntangledListeners = null;
            currentEntangledLane = 0;
            currentEntangledActionThenable = null;
            for (var i = 0; i < listeners.length; i++) (0, listeners[i])();
          }
        }
        function chainThenableValue(thenable, result) {
          var listeners = [], thenableWithOverride = {
            status: "pending",
            value: null,
            reason: null,
            then: function(resolve) {
              listeners.push(resolve);
            }
          };
          thenable.then(
            function() {
              thenableWithOverride.status = "fulfilled";
              thenableWithOverride.value = result;
              for (var i = 0; i < listeners.length; i++) (0, listeners[i])(result);
            },
            function(error) {
              thenableWithOverride.status = "rejected";
              thenableWithOverride.reason = error;
              for (error = 0; error < listeners.length; error++)
                (0, listeners[error])(void 0);
            }
          );
          return thenableWithOverride;
        }
        function peekCacheFromPool() {
          var cacheResumedFromPreviousRender = resumedCache.current;
          return null !== cacheResumedFromPreviousRender ? cacheResumedFromPreviousRender : workInProgressRoot.pooledCache;
        }
        function pushTransition(offscreenWorkInProgress, prevCachePool) {
          null === prevCachePool ? push(resumedCache, resumedCache.current) : push(resumedCache, prevCachePool.pool);
        }
        function getSuspendedCache() {
          var cacheFromPool = peekCacheFromPool();
          return null === cacheFromPool ? null : {
            parent: isPrimaryRenderer ? CacheContext._currentValue : CacheContext._currentValue2,
            pool: cacheFromPool
          };
        }
        function shallowEqual(objA, objB) {
          if (objectIs(objA, objB)) return true;
          if ("object" !== typeof objA || null === objA || "object" !== typeof objB || null === objB)
            return false;
          var keysA = Object.keys(objA), keysB = Object.keys(objB);
          if (keysA.length !== keysB.length) return false;
          for (keysB = 0; keysB < keysA.length; keysB++) {
            var currentKey = keysA[keysB];
            if (!hasOwnProperty.call(objB, currentKey) || !objectIs(objA[currentKey], objB[currentKey]))
              return false;
          }
          return true;
        }
        function isThenableResolved(thenable) {
          thenable = thenable.status;
          return "fulfilled" === thenable || "rejected" === thenable;
        }
        function trackUsedThenable(thenableState2, thenable, index) {
          index = thenableState2[index];
          void 0 === index ? thenableState2.push(thenable) : index !== thenable && (thenable.then(noop$1, noop$1), thenable = index);
          switch (thenable.status) {
            case "fulfilled":
              return thenable.value;
            case "rejected":
              thenableState2 = thenable.reason;
              checkIfUseWrappedInAsyncCatch(thenableState2);
              if (void 0 === thenableState2 && !("reason" in thenable))
                throw Error(formatProdErrorMessage(600));
              throw thenableState2;
            default:
              if ("string" === typeof thenable.status) thenable.then(noop$1, noop$1);
              else {
                thenableState2 = workInProgressRoot;
                if (null !== thenableState2 && 100 < thenableState2.shellSuspendCounter)
                  throw Error(formatProdErrorMessage(482));
                thenableState2 = thenable;
                thenableState2.status = "pending";
                thenableState2.then(
                  function(fulfilledValue) {
                    if ("pending" === thenable.status) {
                      var fulfilledThenable = thenable;
                      fulfilledThenable.status = "fulfilled";
                      fulfilledThenable.value = fulfilledValue;
                    }
                  },
                  function(error) {
                    if ("pending" === thenable.status) {
                      var rejectedThenable = thenable;
                      rejectedThenable.status = "rejected";
                      rejectedThenable.reason = error;
                    }
                  }
                );
              }
              switch (thenable.status) {
                case "fulfilled":
                  return thenable.value;
                case "rejected":
                  throw thenableState2 = thenable.reason, checkIfUseWrappedInAsyncCatch(thenableState2), thenableState2;
              }
              suspendedThenable = thenable;
              throw SuspenseException;
          }
        }
        function resolveLazy(lazyType) {
          try {
            var init = lazyType._init;
            return init(lazyType._payload);
          } catch (x) {
            if (null !== x && "object" === typeof x && "function" === typeof x.then)
              throw suspendedThenable = x, SuspenseException;
            throw x;
          }
        }
        function getSuspendedThenable() {
          if (null === suspendedThenable) throw Error(formatProdErrorMessage(459));
          var thenable = suspendedThenable;
          suspendedThenable = null;
          return thenable;
        }
        function checkIfUseWrappedInAsyncCatch(rejectedReason) {
          if (rejectedReason === SuspenseException || rejectedReason === SuspenseActionException)
            throw Error(formatProdErrorMessage(483));
        }
        function unwrapThenable(thenable) {
          var index = thenableIndexCounter$1;
          thenableIndexCounter$1 += 1;
          null === thenableState$1 && (thenableState$1 = []);
          return trackUsedThenable(thenableState$1, thenable, index);
        }
        function coerceRef(workInProgress2, element) {
          element = element.props.ref;
          workInProgress2.ref = void 0 !== element ? element : null;
        }
        function throwOnInvalidObjectTypeImpl(returnFiber, newChild) {
          if (newChild.$$typeof === REACT_LEGACY_ELEMENT_TYPE)
            throw Error(formatProdErrorMessage(525));
          returnFiber = Object.prototype.toString.call(newChild);
          throw Error(
            formatProdErrorMessage(
              31,
              "[object Object]" === returnFiber ? "object with keys {" + Object.keys(newChild).join(", ") + "}" : returnFiber
            )
          );
        }
        function createChildReconciler(shouldTrackSideEffects) {
          function deleteChild(returnFiber, childToDelete) {
            if (shouldTrackSideEffects) {
              var deletions = returnFiber.deletions;
              null === deletions ? (returnFiber.deletions = [childToDelete], returnFiber.flags |= 16) : deletions.push(childToDelete);
            }
          }
          function deleteRemainingChildren(returnFiber, currentFirstChild) {
            if (!shouldTrackSideEffects) return null;
            for (; null !== currentFirstChild; )
              deleteChild(returnFiber, currentFirstChild), currentFirstChild = currentFirstChild.sibling;
            return null;
          }
          function mapRemainingChildren(currentFirstChild) {
            for (var existingChildren = /* @__PURE__ */ new Map(); null !== currentFirstChild; )
              null === currentFirstChild.key ? existingChildren.set(currentFirstChild.index, currentFirstChild) : existingChildren.set(currentFirstChild.key, currentFirstChild), currentFirstChild = currentFirstChild.sibling;
            return existingChildren;
          }
          function useFiber(fiber, pendingProps) {
            fiber = createWorkInProgress(fiber, pendingProps);
            fiber.index = 0;
            fiber.sibling = null;
            return fiber;
          }
          function placeChild(newFiber, lastPlacedIndex, newIndex) {
            newFiber.index = newIndex;
            if (!shouldTrackSideEffects)
              return newFiber.flags |= 1048576, lastPlacedIndex;
            newIndex = newFiber.alternate;
            if (null !== newIndex)
              return newIndex = newIndex.index, newIndex < lastPlacedIndex ? (newFiber.flags |= 2, lastPlacedIndex) : newIndex;
            newFiber.flags |= 134217730;
            return lastPlacedIndex;
          }
          function placeSingleChild(newFiber) {
            shouldTrackSideEffects && null === newFiber.alternate && (newFiber.flags |= 134217730);
            return newFiber;
          }
          function updateTextNode(returnFiber, current, textContent, lanes) {
            if (null === current || 6 !== current.tag)
              return current = createFiberFromText(textContent, returnFiber.mode, lanes), current.return = returnFiber, current;
            current = useFiber(current, textContent);
            current.return = returnFiber;
            return current;
          }
          function updateElement(returnFiber, current, element, lanes) {
            var elementType = element.type;
            if (elementType === REACT_FRAGMENT_TYPE)
              return returnFiber = updateFragment(
                returnFiber,
                current,
                element.props.children,
                lanes,
                element.key
              ), coerceRef(returnFiber, element), returnFiber;
            if (null !== current && (current.elementType === elementType || "object" === typeof elementType && null !== elementType && elementType.$$typeof === REACT_LAZY_TYPE && resolveLazy(elementType) === current.type))
              return current = useFiber(current, element.props), coerceRef(current, element), current.return = returnFiber, current;
            current = createFiberFromTypeAndProps(
              element.type,
              element.key,
              element.props,
              null,
              returnFiber.mode,
              lanes
            );
            coerceRef(current, element);
            current.return = returnFiber;
            return current;
          }
          function updatePortal(returnFiber, current, portal, lanes) {
            if (null === current || 4 !== current.tag || current.stateNode.containerInfo !== portal.containerInfo || current.stateNode.implementation !== portal.implementation)
              return current = createFiberFromPortal(portal, returnFiber.mode, lanes), current.return = returnFiber, current;
            current = useFiber(current, portal.children || []);
            current.return = returnFiber;
            return current;
          }
          function updateFragment(returnFiber, current, fragment, lanes, key) {
            if (null === current || 7 !== current.tag)
              return current = createFiberFromFragment(
                fragment,
                returnFiber.mode,
                lanes,
                key
              ), current.return = returnFiber, current;
            current = useFiber(current, fragment);
            current.return = returnFiber;
            return current;
          }
          function createChild(returnFiber, newChild, lanes) {
            if ("string" === typeof newChild && "" !== newChild || "number" === typeof newChild || "bigint" === typeof newChild)
              return newChild = createFiberFromText(
                "" + newChild,
                returnFiber.mode,
                lanes
              ), newChild.return = returnFiber, newChild;
            if ("object" === typeof newChild && null !== newChild) {
              switch (newChild.$$typeof) {
                case REACT_ELEMENT_TYPE:
                  return lanes = createFiberFromTypeAndProps(
                    newChild.type,
                    newChild.key,
                    newChild.props,
                    null,
                    returnFiber.mode,
                    lanes
                  ), coerceRef(lanes, newChild), lanes.return = returnFiber, lanes;
                case REACT_PORTAL_TYPE:
                  return newChild = createFiberFromPortal(
                    newChild,
                    returnFiber.mode,
                    lanes
                  ), newChild.return = returnFiber, newChild;
                case REACT_LAZY_TYPE:
                  return newChild = resolveLazy(newChild), createChild(returnFiber, newChild, lanes);
              }
              if (isArrayImpl(newChild) || getIteratorFn(newChild))
                return newChild = createFiberFromFragment(
                  newChild,
                  returnFiber.mode,
                  lanes,
                  null
                ), newChild.return = returnFiber, newChild;
              if ("function" === typeof newChild.then)
                return createChild(returnFiber, unwrapThenable(newChild), lanes);
              if (newChild.$$typeof === REACT_CONTEXT_TYPE)
                return createChild(
                  returnFiber,
                  readContextDuringReconciliation(returnFiber, newChild),
                  lanes
                );
              throwOnInvalidObjectTypeImpl(returnFiber, newChild);
            }
            return null;
          }
          function updateSlot(returnFiber, oldFiber, newChild, lanes) {
            var key = null !== oldFiber ? oldFiber.key : null;
            if ("string" === typeof newChild && "" !== newChild || "number" === typeof newChild || "bigint" === typeof newChild)
              return null !== key ? null : updateTextNode(returnFiber, oldFiber, "" + newChild, lanes);
            if ("object" === typeof newChild && null !== newChild) {
              switch (newChild.$$typeof) {
                case REACT_ELEMENT_TYPE:
                  return newChild.key === key ? updateElement(returnFiber, oldFiber, newChild, lanes) : null;
                case REACT_PORTAL_TYPE:
                  return newChild.key === key ? updatePortal(returnFiber, oldFiber, newChild, lanes) : null;
                case REACT_LAZY_TYPE:
                  return newChild = resolveLazy(newChild), updateSlot(returnFiber, oldFiber, newChild, lanes);
              }
              if (isArrayImpl(newChild) || getIteratorFn(newChild))
                return null !== key ? null : updateFragment(returnFiber, oldFiber, newChild, lanes, null);
              if ("function" === typeof newChild.then)
                return updateSlot(
                  returnFiber,
                  oldFiber,
                  unwrapThenable(newChild),
                  lanes
                );
              if (newChild.$$typeof === REACT_CONTEXT_TYPE)
                return updateSlot(
                  returnFiber,
                  oldFiber,
                  readContextDuringReconciliation(returnFiber, newChild),
                  lanes
                );
              throwOnInvalidObjectTypeImpl(returnFiber, newChild);
            }
            return null;
          }
          function updateFromMap(existingChildren, returnFiber, newIdx, newChild, lanes) {
            if ("string" === typeof newChild && "" !== newChild || "number" === typeof newChild || "bigint" === typeof newChild)
              return existingChildren = existingChildren.get(newIdx) || null, updateTextNode(returnFiber, existingChildren, "" + newChild, lanes);
            if ("object" === typeof newChild && null !== newChild) {
              switch (newChild.$$typeof) {
                case REACT_ELEMENT_TYPE:
                  return existingChildren = existingChildren.get(
                    null === newChild.key ? newIdx : newChild.key
                  ) || null, updateElement(returnFiber, existingChildren, newChild, lanes);
                case REACT_PORTAL_TYPE:
                  return existingChildren = existingChildren.get(
                    null === newChild.key ? newIdx : newChild.key
                  ) || null, updatePortal(returnFiber, existingChildren, newChild, lanes);
                case REACT_LAZY_TYPE:
                  return newChild = resolveLazy(newChild), updateFromMap(
                    existingChildren,
                    returnFiber,
                    newIdx,
                    newChild,
                    lanes
                  );
              }
              if (isArrayImpl(newChild) || getIteratorFn(newChild))
                return existingChildren = existingChildren.get(newIdx) || null, updateFragment(returnFiber, existingChildren, newChild, lanes, null);
              if ("function" === typeof newChild.then)
                return updateFromMap(
                  existingChildren,
                  returnFiber,
                  newIdx,
                  unwrapThenable(newChild),
                  lanes
                );
              if (newChild.$$typeof === REACT_CONTEXT_TYPE)
                return updateFromMap(
                  existingChildren,
                  returnFiber,
                  newIdx,
                  readContextDuringReconciliation(returnFiber, newChild),
                  lanes
                );
              throwOnInvalidObjectTypeImpl(returnFiber, newChild);
            }
            return null;
          }
          function reconcileChildrenArray(returnFiber, currentFirstChild, newChildren, lanes) {
            for (var resultingFirstChild = null, previousNewFiber = null, oldFiber = currentFirstChild, newIdx = currentFirstChild = 0, nextOldFiber = null; null !== oldFiber && newIdx < newChildren.length; newIdx++) {
              oldFiber.index > newIdx ? (nextOldFiber = oldFiber, oldFiber = null) : nextOldFiber = oldFiber.sibling;
              var newFiber = updateSlot(
                returnFiber,
                oldFiber,
                newChildren[newIdx],
                lanes
              );
              if (null === newFiber) {
                null === oldFiber && (oldFiber = nextOldFiber);
                break;
              }
              shouldTrackSideEffects && oldFiber && null === newFiber.alternate && deleteChild(returnFiber, oldFiber);
              currentFirstChild = placeChild(newFiber, currentFirstChild, newIdx);
              null === previousNewFiber ? resultingFirstChild = newFiber : previousNewFiber.sibling = newFiber;
              previousNewFiber = newFiber;
              oldFiber = nextOldFiber;
            }
            if (newIdx === newChildren.length)
              return deleteRemainingChildren(returnFiber, oldFiber), isHydrating && pushTreeFork(returnFiber, newIdx), resultingFirstChild;
            if (null === oldFiber) {
              for (; newIdx < newChildren.length; newIdx++)
                oldFiber = createChild(returnFiber, newChildren[newIdx], lanes), null !== oldFiber && (currentFirstChild = placeChild(
                  oldFiber,
                  currentFirstChild,
                  newIdx
                ), null === previousNewFiber ? resultingFirstChild = oldFiber : previousNewFiber.sibling = oldFiber, previousNewFiber = oldFiber);
              isHydrating && pushTreeFork(returnFiber, newIdx);
              return resultingFirstChild;
            }
            for (oldFiber = mapRemainingChildren(oldFiber); newIdx < newChildren.length; newIdx++)
              nextOldFiber = updateFromMap(
                oldFiber,
                returnFiber,
                newIdx,
                newChildren[newIdx],
                lanes
              ), null !== nextOldFiber && (shouldTrackSideEffects && (newFiber = nextOldFiber.alternate, null !== newFiber && oldFiber.delete(null === newFiber.key ? newIdx : newFiber.key)), currentFirstChild = placeChild(
                nextOldFiber,
                currentFirstChild,
                newIdx
              ), null === previousNewFiber ? resultingFirstChild = nextOldFiber : previousNewFiber.sibling = nextOldFiber, previousNewFiber = nextOldFiber);
            shouldTrackSideEffects && oldFiber.forEach(function(child) {
              return deleteChild(returnFiber, child);
            });
            isHydrating && pushTreeFork(returnFiber, newIdx);
            return resultingFirstChild;
          }
          function reconcileChildrenIterator(returnFiber, currentFirstChild, newChildren, lanes) {
            if (null == newChildren) throw Error(formatProdErrorMessage(151));
            for (var resultingFirstChild = null, previousNewFiber = null, oldFiber = currentFirstChild, newIdx = currentFirstChild = 0, nextOldFiber = null, step = newChildren.next(); null !== oldFiber && !step.done; newIdx++, step = newChildren.next()) {
              oldFiber.index > newIdx ? (nextOldFiber = oldFiber, oldFiber = null) : nextOldFiber = oldFiber.sibling;
              var newFiber = updateSlot(returnFiber, oldFiber, step.value, lanes);
              if (null === newFiber) {
                null === oldFiber && (oldFiber = nextOldFiber);
                break;
              }
              shouldTrackSideEffects && oldFiber && null === newFiber.alternate && deleteChild(returnFiber, oldFiber);
              currentFirstChild = placeChild(newFiber, currentFirstChild, newIdx);
              null === previousNewFiber ? resultingFirstChild = newFiber : previousNewFiber.sibling = newFiber;
              previousNewFiber = newFiber;
              oldFiber = nextOldFiber;
            }
            if (step.done)
              return deleteRemainingChildren(returnFiber, oldFiber), isHydrating && pushTreeFork(returnFiber, newIdx), resultingFirstChild;
            if (null === oldFiber) {
              for (; !step.done; newIdx++, step = newChildren.next())
                step = createChild(returnFiber, step.value, lanes), null !== step && (currentFirstChild = placeChild(
                  step,
                  currentFirstChild,
                  newIdx
                ), null === previousNewFiber ? resultingFirstChild = step : previousNewFiber.sibling = step, previousNewFiber = step);
              isHydrating && pushTreeFork(returnFiber, newIdx);
              return resultingFirstChild;
            }
            for (oldFiber = mapRemainingChildren(oldFiber); !step.done; newIdx++, step = newChildren.next())
              step = updateFromMap(
                oldFiber,
                returnFiber,
                newIdx,
                step.value,
                lanes
              ), null !== step && (shouldTrackSideEffects && (nextOldFiber = step.alternate, null !== nextOldFiber && oldFiber.delete(
                null === nextOldFiber.key ? newIdx : nextOldFiber.key
              )), currentFirstChild = placeChild(step, currentFirstChild, newIdx), null === previousNewFiber ? resultingFirstChild = step : previousNewFiber.sibling = step, previousNewFiber = step);
            shouldTrackSideEffects && oldFiber.forEach(function(child) {
              return deleteChild(returnFiber, child);
            });
            isHydrating && pushTreeFork(returnFiber, newIdx);
            return resultingFirstChild;
          }
          function reconcileChildFibersImpl(returnFiber, currentFirstChild, newChild, lanes) {
            "object" === typeof newChild && null !== newChild && newChild.type === REACT_FRAGMENT_TYPE && null === newChild.key && void 0 === newChild.props.ref && (newChild = newChild.props.children);
            if ("object" === typeof newChild && null !== newChild) {
              switch (newChild.$$typeof) {
                case REACT_ELEMENT_TYPE:
                  a: {
                    for (var key = newChild.key; null !== currentFirstChild; ) {
                      if (currentFirstChild.key === key) {
                        key = newChild.type;
                        if (key === REACT_FRAGMENT_TYPE) {
                          if (7 === currentFirstChild.tag) {
                            deleteRemainingChildren(
                              returnFiber,
                              currentFirstChild.sibling
                            );
                            lanes = useFiber(
                              currentFirstChild,
                              newChild.props.children
                            );
                            coerceRef(lanes, newChild);
                            lanes.return = returnFiber;
                            returnFiber = lanes;
                            break a;
                          }
                        } else if (currentFirstChild.elementType === key || "object" === typeof key && null !== key && key.$$typeof === REACT_LAZY_TYPE && resolveLazy(key) === currentFirstChild.type) {
                          deleteRemainingChildren(
                            returnFiber,
                            currentFirstChild.sibling
                          );
                          lanes = useFiber(currentFirstChild, newChild.props);
                          coerceRef(lanes, newChild);
                          lanes.return = returnFiber;
                          returnFiber = lanes;
                          break a;
                        }
                        deleteRemainingChildren(returnFiber, currentFirstChild);
                        break;
                      } else deleteChild(returnFiber, currentFirstChild);
                      currentFirstChild = currentFirstChild.sibling;
                    }
                    newChild.type === REACT_FRAGMENT_TYPE ? (lanes = createFiberFromFragment(
                      newChild.props.children,
                      returnFiber.mode,
                      lanes,
                      newChild.key
                    ), coerceRef(lanes, newChild), lanes.return = returnFiber, returnFiber = lanes) : (lanes = createFiberFromTypeAndProps(
                      newChild.type,
                      newChild.key,
                      newChild.props,
                      null,
                      returnFiber.mode,
                      lanes
                    ), coerceRef(lanes, newChild), lanes.return = returnFiber, returnFiber = lanes);
                  }
                  return placeSingleChild(returnFiber);
                case REACT_PORTAL_TYPE:
                  a: {
                    for (key = newChild.key; null !== currentFirstChild; ) {
                      if (currentFirstChild.key === key)
                        if (4 === currentFirstChild.tag && currentFirstChild.stateNode.containerInfo === newChild.containerInfo && currentFirstChild.stateNode.implementation === newChild.implementation) {
                          deleteRemainingChildren(
                            returnFiber,
                            currentFirstChild.sibling
                          );
                          lanes = useFiber(
                            currentFirstChild,
                            newChild.children || []
                          );
                          lanes.return = returnFiber;
                          returnFiber = lanes;
                          break a;
                        } else {
                          deleteRemainingChildren(returnFiber, currentFirstChild);
                          break;
                        }
                      else deleteChild(returnFiber, currentFirstChild);
                      currentFirstChild = currentFirstChild.sibling;
                    }
                    lanes = createFiberFromPortal(newChild, returnFiber.mode, lanes);
                    lanes.return = returnFiber;
                    returnFiber = lanes;
                  }
                  return placeSingleChild(returnFiber);
                case REACT_LAZY_TYPE:
                  return newChild = resolveLazy(newChild), reconcileChildFibersImpl(
                    returnFiber,
                    currentFirstChild,
                    newChild,
                    lanes
                  );
              }
              if (isArrayImpl(newChild))
                return reconcileChildrenArray(
                  returnFiber,
                  currentFirstChild,
                  newChild,
                  lanes
                );
              if (getIteratorFn(newChild)) {
                key = getIteratorFn(newChild);
                if ("function" !== typeof key)
                  throw Error(formatProdErrorMessage(150));
                newChild = key.call(newChild);
                return reconcileChildrenIterator(
                  returnFiber,
                  currentFirstChild,
                  newChild,
                  lanes
                );
              }
              if ("function" === typeof newChild.then)
                return reconcileChildFibersImpl(
                  returnFiber,
                  currentFirstChild,
                  unwrapThenable(newChild),
                  lanes
                );
              if (newChild.$$typeof === REACT_CONTEXT_TYPE)
                return reconcileChildFibersImpl(
                  returnFiber,
                  currentFirstChild,
                  readContextDuringReconciliation(returnFiber, newChild),
                  lanes
                );
              throwOnInvalidObjectTypeImpl(returnFiber, newChild);
            }
            return "string" === typeof newChild && "" !== newChild || "number" === typeof newChild || "bigint" === typeof newChild ? (newChild = "" + newChild, null !== currentFirstChild && 6 === currentFirstChild.tag ? (deleteRemainingChildren(returnFiber, currentFirstChild.sibling), lanes = useFiber(currentFirstChild, newChild), lanes.return = returnFiber, returnFiber = lanes) : (deleteRemainingChildren(returnFiber, currentFirstChild), lanes = createFiberFromText(newChild, returnFiber.mode, lanes), lanes.return = returnFiber, returnFiber = lanes), placeSingleChild(returnFiber)) : deleteRemainingChildren(returnFiber, currentFirstChild);
          }
          return function(returnFiber, currentFirstChild, newChild, lanes) {
            try {
              thenableIndexCounter$1 = 0;
              var firstChildFiber = reconcileChildFibersImpl(
                returnFiber,
                currentFirstChild,
                newChild,
                lanes
              );
              thenableState$1 = null;
              return firstChildFiber;
            } catch (x) {
              if (x === SuspenseException || x === SuspenseActionException) throw x;
              var fiber = createFiber(29, x, null, returnFiber.mode);
              fiber.lanes = lanes;
              fiber.return = returnFiber;
              return fiber;
            } finally {
            }
          };
        }
        function finishQueueingConcurrentUpdates() {
          for (var endIndex = concurrentQueuesIndex, i = concurrentlyUpdatedLanes = concurrentQueuesIndex = 0; i < endIndex; ) {
            var fiber = concurrentQueues[i];
            concurrentQueues[i++] = null;
            var queue = concurrentQueues[i];
            concurrentQueues[i++] = null;
            var update = concurrentQueues[i];
            concurrentQueues[i++] = null;
            var lane = concurrentQueues[i];
            concurrentQueues[i++] = null;
            if (null !== queue && null !== update) {
              var pending = queue.pending;
              null === pending ? update.next = update : (update.next = pending.next, pending.next = update);
              queue.pending = update;
            }
            0 !== lane && markUpdateLaneFromFiberToRoot(fiber, update, lane);
          }
        }
        function enqueueUpdate$1(fiber, queue, update, lane) {
          concurrentQueues[concurrentQueuesIndex++] = fiber;
          concurrentQueues[concurrentQueuesIndex++] = queue;
          concurrentQueues[concurrentQueuesIndex++] = update;
          concurrentQueues[concurrentQueuesIndex++] = lane;
          concurrentlyUpdatedLanes |= lane;
          fiber.lanes |= lane;
          fiber = fiber.alternate;
          null !== fiber && (fiber.lanes |= lane);
        }
        function enqueueConcurrentHookUpdate(fiber, queue, update, lane) {
          enqueueUpdate$1(fiber, queue, update, lane);
          return getRootForUpdatedFiber(fiber);
        }
        function enqueueConcurrentRenderForLane(fiber, lane) {
          enqueueUpdate$1(fiber, null, null, lane);
          return getRootForUpdatedFiber(fiber);
        }
        function markUpdateLaneFromFiberToRoot(sourceFiber, update, lane) {
          sourceFiber.lanes |= lane;
          var alternate = sourceFiber.alternate;
          null !== alternate && (alternate.lanes |= lane);
          for (var isHidden = false, parent = sourceFiber.return; null !== parent; )
            parent.childLanes |= lane, alternate = parent.alternate, null !== alternate && (alternate.childLanes |= lane), 22 === parent.tag && (sourceFiber = parent.stateNode, null === sourceFiber || sourceFiber._visibility & 1 || (isHidden = true)), sourceFiber = parent, parent = parent.return;
          return 3 === sourceFiber.tag ? (parent = sourceFiber.stateNode, isHidden && null !== update && (isHidden = 31 - clz32(lane), sourceFiber = parent.hiddenUpdates, alternate = sourceFiber[isHidden], null === alternate ? sourceFiber[isHidden] = [update] : alternate.push(update), update.lane = lane | 536870912), parent) : null;
        }
        function getRootForUpdatedFiber(sourceFiber) {
          if (50 < nestedUpdateCount)
            throw nestedUpdateCount = 0, rootWithNestedUpdates = null, Error(formatProdErrorMessage(185));
          for (var parent = sourceFiber.return; null !== parent; )
            sourceFiber = parent, parent = sourceFiber.return;
          return 3 === sourceFiber.tag ? sourceFiber.stateNode : null;
        }
        function initializeUpdateQueue(fiber) {
          fiber.updateQueue = {
            baseState: fiber.memoizedState,
            firstBaseUpdate: null,
            lastBaseUpdate: null,
            shared: { pending: null, lanes: 0, hiddenCallbacks: null },
            callbacks: null
          };
        }
        function cloneUpdateQueue(current, workInProgress2) {
          current = current.updateQueue;
          workInProgress2.updateQueue === current && (workInProgress2.updateQueue = {
            baseState: current.baseState,
            firstBaseUpdate: current.firstBaseUpdate,
            lastBaseUpdate: current.lastBaseUpdate,
            shared: current.shared,
            callbacks: null
          });
        }
        function createUpdate(lane) {
          return { lane, tag: 0, payload: null, callback: null, next: null };
        }
        function enqueueUpdate(fiber, update, lane) {
          var updateQueue = fiber.updateQueue;
          if (null === updateQueue) return null;
          updateQueue = updateQueue.shared;
          if (0 !== (executionContext & 2)) {
            var pending = updateQueue.pending;
            null === pending ? update.next = update : (update.next = pending.next, pending.next = update);
            updateQueue.pending = update;
            update = getRootForUpdatedFiber(fiber);
            markUpdateLaneFromFiberToRoot(fiber, null, lane);
            return update;
          }
          enqueueUpdate$1(fiber, updateQueue, update, lane);
          return getRootForUpdatedFiber(fiber);
        }
        function entangleTransitions(root2, fiber, lane) {
          fiber = fiber.updateQueue;
          if (null !== fiber && (fiber = fiber.shared, 0 !== (lane & 4194048))) {
            var queueLanes = fiber.lanes;
            queueLanes &= root2.pendingLanes;
            lane |= queueLanes;
            fiber.lanes = lane;
            markRootEntangled(root2, lane);
          }
        }
        function enqueueCapturedUpdate(workInProgress2, capturedUpdate) {
          var queue = workInProgress2.updateQueue, current = workInProgress2.alternate;
          if (null !== current && (current = current.updateQueue, queue === current)) {
            var newFirst = null, newLast = null;
            queue = queue.firstBaseUpdate;
            if (null !== queue) {
              do {
                var clone = {
                  lane: queue.lane,
                  tag: queue.tag,
                  payload: queue.payload,
                  callback: null,
                  next: null
                };
                null === newLast ? newFirst = newLast = clone : newLast = newLast.next = clone;
                queue = queue.next;
              } while (null !== queue);
              null === newLast ? newFirst = newLast = capturedUpdate : newLast = newLast.next = capturedUpdate;
            } else newFirst = newLast = capturedUpdate;
            queue = {
              baseState: current.baseState,
              firstBaseUpdate: newFirst,
              lastBaseUpdate: newLast,
              shared: current.shared,
              callbacks: current.callbacks
            };
            workInProgress2.updateQueue = queue;
            return;
          }
          workInProgress2 = queue.lastBaseUpdate;
          null === workInProgress2 ? queue.firstBaseUpdate = capturedUpdate : workInProgress2.next = capturedUpdate;
          queue.lastBaseUpdate = capturedUpdate;
        }
        function suspendIfUpdateReadFromEntangledAsyncAction() {
          if (didReadFromEntangledAsyncAction) {
            var entangledActionThenable = currentEntangledActionThenable;
            if (null !== entangledActionThenable) throw entangledActionThenable;
          }
        }
        function processUpdateQueue(workInProgress$jscomp$0, props, instance$jscomp$0, renderLanes2) {
          didReadFromEntangledAsyncAction = false;
          var queue = workInProgress$jscomp$0.updateQueue;
          hasForceUpdate = false;
          var firstBaseUpdate = queue.firstBaseUpdate, lastBaseUpdate = queue.lastBaseUpdate, pendingQueue = queue.shared.pending;
          if (null !== pendingQueue) {
            queue.shared.pending = null;
            var lastPendingUpdate = pendingQueue, firstPendingUpdate = lastPendingUpdate.next;
            lastPendingUpdate.next = null;
            null === lastBaseUpdate ? firstBaseUpdate = firstPendingUpdate : lastBaseUpdate.next = firstPendingUpdate;
            lastBaseUpdate = lastPendingUpdate;
            var current = workInProgress$jscomp$0.alternate;
            null !== current && (current = current.updateQueue, pendingQueue = current.lastBaseUpdate, pendingQueue !== lastBaseUpdate && (null === pendingQueue ? current.firstBaseUpdate = firstPendingUpdate : pendingQueue.next = firstPendingUpdate, current.lastBaseUpdate = lastPendingUpdate));
          }
          if (null !== firstBaseUpdate) {
            var newState = queue.baseState;
            lastBaseUpdate = 0;
            current = firstPendingUpdate = lastPendingUpdate = null;
            pendingQueue = firstBaseUpdate;
            do {
              var updateLane = pendingQueue.lane & -536870913, isHiddenUpdate = updateLane !== pendingQueue.lane;
              if (isHiddenUpdate ? (workInProgressRootRenderLanes & updateLane) === updateLane : (renderLanes2 & updateLane) === updateLane) {
                0 !== updateLane && updateLane === currentEntangledLane && (didReadFromEntangledAsyncAction = true);
                null !== current && (current = current.next = {
                  lane: 0,
                  tag: pendingQueue.tag,
                  payload: pendingQueue.payload,
                  callback: null,
                  next: null
                });
                a: {
                  var workInProgress2 = workInProgress$jscomp$0, update = pendingQueue;
                  updateLane = props;
                  var instance = instance$jscomp$0;
                  switch (update.tag) {
                    case 1:
                      workInProgress2 = update.payload;
                      if ("function" === typeof workInProgress2) {
                        newState = workInProgress2.call(
                          instance,
                          newState,
                          updateLane
                        );
                        break a;
                      }
                      newState = workInProgress2;
                      break a;
                    case 3:
                      workInProgress2.flags = workInProgress2.flags & -65537 | 128;
                    case 0:
                      workInProgress2 = update.payload;
                      updateLane = "function" === typeof workInProgress2 ? workInProgress2.call(instance, newState, updateLane) : workInProgress2;
                      if (null === updateLane || void 0 === updateLane) break a;
                      newState = assign({}, newState, updateLane);
                      break a;
                    case 2:
                      hasForceUpdate = true;
                  }
                }
                updateLane = pendingQueue.callback;
                null !== updateLane && (workInProgress$jscomp$0.flags |= 64, isHiddenUpdate && (workInProgress$jscomp$0.flags |= 8192), isHiddenUpdate = queue.callbacks, null === isHiddenUpdate ? queue.callbacks = [updateLane] : isHiddenUpdate.push(updateLane));
              } else
                isHiddenUpdate = {
                  lane: updateLane,
                  tag: pendingQueue.tag,
                  payload: pendingQueue.payload,
                  callback: pendingQueue.callback,
                  next: null
                }, null === current ? (firstPendingUpdate = current = isHiddenUpdate, lastPendingUpdate = newState) : current = current.next = isHiddenUpdate, lastBaseUpdate |= updateLane;
              pendingQueue = pendingQueue.next;
              if (null === pendingQueue)
                if (pendingQueue = queue.shared.pending, null === pendingQueue)
                  break;
                else
                  isHiddenUpdate = pendingQueue, pendingQueue = isHiddenUpdate.next, isHiddenUpdate.next = null, queue.lastBaseUpdate = isHiddenUpdate, queue.shared.pending = null;
            } while (1);
            null === current && (lastPendingUpdate = newState);
            queue.baseState = lastPendingUpdate;
            queue.firstBaseUpdate = firstPendingUpdate;
            queue.lastBaseUpdate = current;
            null === firstBaseUpdate && (queue.shared.lanes = 0);
            workInProgressRootSkippedLanes |= lastBaseUpdate;
            workInProgress$jscomp$0.lanes = lastBaseUpdate;
            workInProgress$jscomp$0.memoizedState = newState;
          }
        }
        function callCallback(callback, context) {
          if ("function" !== typeof callback)
            throw Error(formatProdErrorMessage(191, callback));
          callback.call(context);
        }
        function commitCallbacks(updateQueue, context) {
          var callbacks = updateQueue.callbacks;
          if (null !== callbacks)
            for (updateQueue.callbacks = null, updateQueue = 0; updateQueue < callbacks.length; updateQueue++)
              callCallback(callbacks[updateQueue], context);
        }
        function pushHiddenContext(fiber, context) {
          fiber = entangledRenderLanes;
          push(prevEntangledRenderLanesCursor, fiber);
          push(currentTreeHiddenStackCursor, context);
          entangledRenderLanes = fiber | context.baseLanes;
        }
        function reuseHiddenContextOnStack() {
          push(prevEntangledRenderLanesCursor, entangledRenderLanes);
          push(currentTreeHiddenStackCursor, currentTreeHiddenStackCursor.current);
        }
        function popHiddenContext() {
          entangledRenderLanes = prevEntangledRenderLanesCursor.current;
          pop(currentTreeHiddenStackCursor);
          pop(prevEntangledRenderLanesCursor);
        }
        function pushPrimaryTreeSuspenseHandler(handler) {
          var current = handler.alternate;
          push(suspenseStackCursor, suspenseStackCursor.current & 1);
          push(suspenseHandlerStackCursor, handler);
          null === shellBoundary && (null === current || null !== currentTreeHiddenStackCursor.current ? shellBoundary = handler : null !== current.memoizedState && (shellBoundary = handler));
        }
        function pushDehydratedActivitySuspenseHandler(fiber) {
          push(suspenseStackCursor, suspenseStackCursor.current);
          push(suspenseHandlerStackCursor, fiber);
          null === shellBoundary && (shellBoundary = fiber);
        }
        function pushOffscreenSuspenseHandler(fiber) {
          22 === fiber.tag ? (push(suspenseStackCursor, suspenseStackCursor.current), push(suspenseHandlerStackCursor, fiber), null === shellBoundary && (shellBoundary = fiber)) : reuseSuspenseHandlerOnStack();
        }
        function reuseSuspenseHandlerOnStack() {
          push(suspenseStackCursor, suspenseStackCursor.current);
          push(suspenseHandlerStackCursor, suspenseHandlerStackCursor.current);
        }
        function popSuspenseHandler(fiber) {
          pop(suspenseHandlerStackCursor);
          shellBoundary === fiber && (shellBoundary = null);
          pop(suspenseStackCursor);
        }
        function pushSuspenseListContext(fiber, newContext) {
          push(suspenseHandlerStackCursor, suspenseHandlerStackCursor.current);
          push(suspenseStackCursor, newContext);
        }
        function popSuspenseListContext(fiber) {
          pop(suspenseStackCursor);
          pop(suspenseHandlerStackCursor);
          shellBoundary === fiber && (shellBoundary = null);
        }
        function findFirstSuspended(row) {
          for (var node = row; null !== node; ) {
            if (13 === node.tag) {
              var state = node.memoizedState;
              if (null !== state && (state = state.dehydrated, null === state || isSuspenseInstancePending(state) || isSuspenseInstanceFallback(state)))
                return node;
            } else if (19 === node.tag && "independent" !== node.memoizedProps.revealOrder) {
              if (0 !== (node.flags & 128)) return node;
            } else if (null !== node.child) {
              node.child.return = node;
              node = node.child;
              continue;
            }
            if (node === row) break;
            for (; null === node.sibling; ) {
              if (null === node.return || node.return === row) return null;
              node = node.return;
            }
            node.sibling.return = node.return;
            node = node.sibling;
          }
          return null;
        }
        function throwInvalidHookError() {
          throw Error(formatProdErrorMessage(321));
        }
        function areHookInputsEqual(nextDeps, prevDeps) {
          if (null === prevDeps) return false;
          for (var i = 0; i < prevDeps.length && i < nextDeps.length; i++)
            if (!objectIs(nextDeps[i], prevDeps[i])) return false;
          return true;
        }
        function renderWithHooks(current, workInProgress2, Component, props, secondArg, nextRenderLanes) {
          renderLanes = nextRenderLanes;
          currentlyRenderingFiber = workInProgress2;
          workInProgress2.memoizedState = null;
          workInProgress2.updateQueue = null;
          workInProgress2.lanes = 0;
          ReactSharedInternals.H = null === current || null === current.memoizedState ? HooksDispatcherOnMount : HooksDispatcherOnUpdate;
          shouldDoubleInvokeUserFnsInHooksDEV = false;
          nextRenderLanes = Component(props, secondArg);
          shouldDoubleInvokeUserFnsInHooksDEV = false;
          didScheduleRenderPhaseUpdateDuringThisPass && (nextRenderLanes = renderWithHooksAgain(
            workInProgress2,
            Component,
            props,
            secondArg
          ));
          finishRenderingHooks(current);
          return nextRenderLanes;
        }
        function finishRenderingHooks(current) {
          ReactSharedInternals.H = ContextOnlyDispatcher;
          var didRenderTooFewHooks = null !== currentHook && null !== currentHook.next;
          renderLanes = 0;
          workInProgressHook = currentHook = currentlyRenderingFiber = null;
          didScheduleRenderPhaseUpdate = false;
          thenableIndexCounter = 0;
          thenableState = null;
          if (didRenderTooFewHooks) throw Error(formatProdErrorMessage(300));
          null === current || didReceiveUpdate || (current = current.dependencies, null !== current && checkIfContextChanged(current) && (didReceiveUpdate = true));
        }
        function renderWithHooksAgain(workInProgress2, Component, props, secondArg) {
          currentlyRenderingFiber = workInProgress2;
          var numberOfReRenders = 0;
          do {
            didScheduleRenderPhaseUpdateDuringThisPass && (thenableState = null);
            thenableIndexCounter = 0;
            didScheduleRenderPhaseUpdateDuringThisPass = false;
            if (25 <= numberOfReRenders) throw Error(formatProdErrorMessage(301));
            numberOfReRenders += 1;
            workInProgressHook = currentHook = null;
            if (null != workInProgress2.updateQueue) {
              var children = workInProgress2.updateQueue;
              children.lastEffect = null;
              children.events = null;
              children.stores = null;
              null != children.memoCache && (children.memoCache.index = 0);
            }
            ReactSharedInternals.H = HooksDispatcherOnRerender;
            children = Component(props, secondArg);
          } while (didScheduleRenderPhaseUpdateDuringThisPass);
          return children;
        }
        function TransitionAwareHostComponent() {
          var dispatcher = ReactSharedInternals.H, maybeThenable = dispatcher.useState()[0];
          maybeThenable = "function" === typeof maybeThenable.then ? useThenable(maybeThenable) : maybeThenable;
          dispatcher = dispatcher.useState()[0];
          (null !== currentHook ? currentHook.memoizedState : null) !== dispatcher && (currentlyRenderingFiber.flags |= 1024);
          return maybeThenable;
        }
        function checkDidRenderIdHook() {
          var didRenderIdHook = 0 !== localIdCounter;
          localIdCounter = 0;
          return didRenderIdHook;
        }
        function bailoutHooks(current, workInProgress2, lanes) {
          workInProgress2.updateQueue = current.updateQueue;
          workInProgress2.flags &= -2053;
          current.lanes &= ~lanes;
        }
        function resetHooksOnUnwind(workInProgress2) {
          if (didScheduleRenderPhaseUpdate) {
            for (workInProgress2 = workInProgress2.memoizedState; null !== workInProgress2; ) {
              var queue = workInProgress2.queue;
              null !== queue && (queue.pending = null);
              workInProgress2 = workInProgress2.next;
            }
            didScheduleRenderPhaseUpdate = false;
          }
          renderLanes = 0;
          workInProgressHook = currentHook = currentlyRenderingFiber = null;
          didScheduleRenderPhaseUpdateDuringThisPass = false;
          thenableIndexCounter = localIdCounter = 0;
          thenableState = null;
        }
        function mountWorkInProgressHook() {
          var hook = {
            memoizedState: null,
            baseState: null,
            baseQueue: null,
            queue: null,
            next: null
          };
          null === workInProgressHook ? currentlyRenderingFiber.memoizedState = workInProgressHook = hook : workInProgressHook = workInProgressHook.next = hook;
          return workInProgressHook;
        }
        function updateWorkInProgressHook() {
          if (null === currentHook) {
            var nextCurrentHook = currentlyRenderingFiber.alternate;
            nextCurrentHook = null !== nextCurrentHook ? nextCurrentHook.memoizedState : null;
          } else nextCurrentHook = currentHook.next;
          var nextWorkInProgressHook = null === workInProgressHook ? currentlyRenderingFiber.memoizedState : workInProgressHook.next;
          if (null !== nextWorkInProgressHook)
            workInProgressHook = nextWorkInProgressHook, currentHook = nextCurrentHook;
          else {
            if (null === nextCurrentHook) {
              if (null === currentlyRenderingFiber.alternate)
                throw Error(formatProdErrorMessage(467));
              throw Error(formatProdErrorMessage(310));
            }
            currentHook = nextCurrentHook;
            nextCurrentHook = {
              memoizedState: currentHook.memoizedState,
              baseState: currentHook.baseState,
              baseQueue: currentHook.baseQueue,
              queue: currentHook.queue,
              next: null
            };
            null === workInProgressHook ? currentlyRenderingFiber.memoizedState = workInProgressHook = nextCurrentHook : workInProgressHook = workInProgressHook.next = nextCurrentHook;
          }
          return workInProgressHook;
        }
        function createFunctionComponentUpdateQueue() {
          return { lastEffect: null, events: null, stores: null, memoCache: null };
        }
        function useThenable(thenable) {
          var index = thenableIndexCounter;
          thenableIndexCounter += 1;
          null === thenableState && (thenableState = []);
          thenable = trackUsedThenable(thenableState, thenable, index);
          index = currentlyRenderingFiber;
          null === (null === workInProgressHook ? index.memoizedState : workInProgressHook.next) && (index = index.alternate, ReactSharedInternals.H = null === index || null === index.memoizedState ? HooksDispatcherOnMount : HooksDispatcherOnUpdate);
          return thenable;
        }
        function use(usable) {
          if (null !== usable && "object" === typeof usable) {
            if ("function" === typeof usable.then) return useThenable(usable);
            if (usable.$$typeof === REACT_RECOVERABLE_TYPE) return;
            if (usable.$$typeof === REACT_CONTEXT_TYPE) return readContext(usable);
          }
          throw Error(formatProdErrorMessage(438, String(usable)));
        }
        function useMemoCache(size) {
          var memoCache = null, updateQueue = currentlyRenderingFiber.updateQueue;
          null !== updateQueue && (memoCache = updateQueue.memoCache);
          if (null == memoCache) {
            var current = currentlyRenderingFiber.alternate;
            null !== current && (current = current.updateQueue, null !== current && (current = current.memoCache, null != current && (memoCache = {
              data: current.data.map(function(array) {
                return array.slice();
              }),
              index: 0
            })));
          }
          null == memoCache && (memoCache = { data: [], index: 0 });
          null === updateQueue && (updateQueue = createFunctionComponentUpdateQueue(), currentlyRenderingFiber.updateQueue = updateQueue);
          updateQueue.memoCache = memoCache;
          updateQueue = memoCache.data[memoCache.index];
          if (void 0 === updateQueue)
            for (updateQueue = memoCache.data[memoCache.index] = Array(size), current = 0; current < size; current++)
              updateQueue[current] = REACT_MEMO_CACHE_SENTINEL;
          memoCache.index++;
          return updateQueue;
        }
        function basicStateReducer(state, action) {
          return "function" === typeof action ? action(state) : action;
        }
        function updateReducer(reducer) {
          var hook = updateWorkInProgressHook();
          return updateReducerImpl(hook, currentHook, reducer);
        }
        function updateReducerImpl(hook, current, reducer) {
          var queue = hook.queue;
          if (null === queue) throw Error(formatProdErrorMessage(311));
          queue.lastRenderedReducer = reducer;
          var baseQueue = hook.baseQueue, pendingQueue = queue.pending;
          if (null !== pendingQueue) {
            if (null !== baseQueue) {
              var baseFirst = baseQueue.next;
              baseQueue.next = pendingQueue.next;
              pendingQueue.next = baseFirst;
            }
            current.baseQueue = baseQueue = pendingQueue;
            queue.pending = null;
          }
          pendingQueue = hook.baseState;
          if (null === baseQueue) hook.memoizedState = pendingQueue;
          else {
            current = baseQueue.next;
            var newBaseQueueFirst = baseFirst = null, newBaseQueueLast = null, update = current, didReadFromEntangledAsyncAction$53 = false;
            do {
              var updateLane = update.lane & -536870913;
              if (updateLane !== update.lane ? (workInProgressRootRenderLanes & updateLane) === updateLane : (renderLanes & updateLane) === updateLane) {
                var revertLane = update.revertLane;
                if (0 === revertLane)
                  null !== newBaseQueueLast && (newBaseQueueLast = newBaseQueueLast.next = {
                    lane: 0,
                    revertLane: 0,
                    gesture: null,
                    action: update.action,
                    hasEagerState: update.hasEagerState,
                    eagerState: update.eagerState,
                    next: null
                  }), updateLane === currentEntangledLane && (didReadFromEntangledAsyncAction$53 = true);
                else if ((renderLanes & revertLane) === revertLane) {
                  update = update.next;
                  revertLane === currentEntangledLane && (didReadFromEntangledAsyncAction$53 = true);
                  continue;
                } else
                  updateLane = {
                    lane: 0,
                    revertLane: update.revertLane,
                    gesture: null,
                    action: update.action,
                    hasEagerState: update.hasEagerState,
                    eagerState: update.eagerState,
                    next: null
                  }, null === newBaseQueueLast ? (newBaseQueueFirst = newBaseQueueLast = updateLane, baseFirst = pendingQueue) : newBaseQueueLast = newBaseQueueLast.next = updateLane, currentlyRenderingFiber.lanes |= revertLane, workInProgressRootSkippedLanes |= revertLane;
                updateLane = update.action;
                shouldDoubleInvokeUserFnsInHooksDEV && reducer(pendingQueue, updateLane);
                pendingQueue = update.hasEagerState ? update.eagerState : reducer(pendingQueue, updateLane);
              } else
                revertLane = {
                  lane: updateLane,
                  revertLane: update.revertLane,
                  gesture: update.gesture,
                  action: update.action,
                  hasEagerState: update.hasEagerState,
                  eagerState: update.eagerState,
                  next: null
                }, null === newBaseQueueLast ? (newBaseQueueFirst = newBaseQueueLast = revertLane, baseFirst = pendingQueue) : newBaseQueueLast = newBaseQueueLast.next = revertLane, currentlyRenderingFiber.lanes |= updateLane, workInProgressRootSkippedLanes |= updateLane;
              update = update.next;
            } while (null !== update && update !== current);
            null === newBaseQueueLast ? baseFirst = pendingQueue : newBaseQueueLast.next = newBaseQueueFirst;
            if (!objectIs(pendingQueue, hook.memoizedState) && (didReceiveUpdate = true, didReadFromEntangledAsyncAction$53 && (reducer = currentEntangledActionThenable, null !== reducer)))
              throw reducer;
            hook.memoizedState = pendingQueue;
            hook.baseState = baseFirst;
            hook.baseQueue = newBaseQueueLast;
            queue.lastRenderedState = pendingQueue;
          }
          null === baseQueue && (queue.lanes = 0);
          return [hook.memoizedState, queue.dispatch];
        }
        function rerenderReducer(reducer) {
          var hook = updateWorkInProgressHook(), queue = hook.queue;
          if (null === queue) throw Error(formatProdErrorMessage(311));
          queue.lastRenderedReducer = reducer;
          var dispatch = queue.dispatch, lastRenderPhaseUpdate = queue.pending, newState = hook.memoizedState;
          if (null !== lastRenderPhaseUpdate) {
            queue.pending = null;
            var update = lastRenderPhaseUpdate = lastRenderPhaseUpdate.next;
            do
              newState = reducer(newState, update.action), update = update.next;
            while (update !== lastRenderPhaseUpdate);
            objectIs(newState, hook.memoizedState) || (didReceiveUpdate = true);
            hook.memoizedState = newState;
            null === hook.baseQueue && (hook.baseState = newState);
            queue.lastRenderedState = newState;
          }
          return [newState, dispatch];
        }
        function updateSyncExternalStore(subscribe, getSnapshot, getServerSnapshot) {
          var fiber = currentlyRenderingFiber, hook = updateWorkInProgressHook(), isHydrating$jscomp$0 = isHydrating;
          if (isHydrating$jscomp$0) {
            if (void 0 === getServerSnapshot)
              throw Error(formatProdErrorMessage(407));
            getServerSnapshot = getServerSnapshot();
          } else getServerSnapshot = getSnapshot();
          var snapshotChanged = !objectIs(
            (currentHook || hook).memoizedState,
            getServerSnapshot
          );
          snapshotChanged && (hook.memoizedState = getServerSnapshot, didReceiveUpdate = true);
          hook = hook.queue;
          updateEffect(subscribeToStore.bind(null, fiber, hook, subscribe), [
            subscribe
          ]);
          subscribe = hook.getSnapshot !== getSnapshot || snapshotChanged || null !== workInProgressHook && 0 !== (workInProgressHook.memoizedState.tag & 1);
          pushSimpleEffect(
            subscribe ? 9 : 8,
            { destroy: void 0 },
            updateStoreInstance.bind(
              null,
              fiber,
              hook,
              getServerSnapshot,
              getSnapshot
            ),
            null
          );
          if (subscribe) {
            fiber.flags |= 2048;
            if (null === workInProgressRoot) throw Error(formatProdErrorMessage(349));
            isHydrating$jscomp$0 || 0 !== (renderLanes & 127) || pushStoreConsistencyCheck(fiber, getSnapshot, getServerSnapshot);
          }
          return getServerSnapshot;
        }
        function pushStoreConsistencyCheck(fiber, getSnapshot, renderedSnapshot) {
          fiber.flags |= 16384;
          fiber = { getSnapshot, value: renderedSnapshot };
          getSnapshot = currentlyRenderingFiber.updateQueue;
          null === getSnapshot ? (getSnapshot = createFunctionComponentUpdateQueue(), currentlyRenderingFiber.updateQueue = getSnapshot, getSnapshot.stores = [fiber]) : (renderedSnapshot = getSnapshot.stores, null === renderedSnapshot ? getSnapshot.stores = [fiber] : renderedSnapshot.push(fiber));
        }
        function updateStoreInstance(fiber, inst, nextSnapshot, getSnapshot) {
          inst.value = nextSnapshot;
          inst.getSnapshot = getSnapshot;
          checkIfSnapshotChanged(inst) && forceStoreRerender(fiber);
        }
        function subscribeToStore(fiber, inst, subscribe) {
          return subscribe(function() {
            checkIfSnapshotChanged(inst) && forceStoreRerender(fiber);
          });
        }
        function checkIfSnapshotChanged(inst) {
          var latestGetSnapshot = inst.getSnapshot;
          inst = inst.value;
          try {
            var nextValue = latestGetSnapshot();
            return !objectIs(inst, nextValue);
          } catch (error) {
            return true;
          }
        }
        function forceStoreRerender(fiber) {
          var root2 = enqueueConcurrentRenderForLane(fiber, 2);
          null !== root2 && scheduleUpdateOnFiber(root2, fiber, 2);
        }
        function mountStateImpl(initialState) {
          var hook = mountWorkInProgressHook();
          if ("function" === typeof initialState) {
            var initialStateInitializer = initialState;
            initialState = initialStateInitializer();
            if (shouldDoubleInvokeUserFnsInHooksDEV) {
              setIsStrictModeForDevtools(true);
              try {
                initialStateInitializer();
              } finally {
                setIsStrictModeForDevtools(false);
              }
            }
          }
          hook.memoizedState = hook.baseState = initialState;
          hook.queue = {
            pending: null,
            lanes: 0,
            dispatch: null,
            lastRenderedReducer: basicStateReducer,
            lastRenderedState: initialState
          };
          return hook;
        }
        function updateOptimisticImpl(hook, current, passthrough, reducer) {
          hook.baseState = passthrough;
          return updateReducerImpl(
            hook,
            currentHook,
            "function" === typeof reducer ? reducer : basicStateReducer
          );
        }
        function dispatchActionState(fiber, actionQueue, setPendingState, setState, payload) {
          if (isRenderPhaseUpdate(fiber)) throw Error(formatProdErrorMessage(485));
          fiber = actionQueue.action;
          if (null !== fiber) {
            var actionNode = {
              payload,
              action: fiber,
              next: null,
              isTransition: true,
              status: "pending",
              value: null,
              reason: null,
              listeners: [],
              then: function(listener) {
                actionNode.listeners.push(listener);
              }
            };
            null !== ReactSharedInternals.T ? setPendingState(true) : actionNode.isTransition = false;
            setState(actionNode);
            setPendingState = actionQueue.pending;
            null === setPendingState ? (actionNode.next = actionQueue.pending = actionNode, runActionStateAction(actionQueue, actionNode)) : (actionNode.next = setPendingState.next, actionQueue.pending = setPendingState.next = actionNode);
          }
        }
        function runActionStateAction(actionQueue, node) {
          var action = node.action, payload = node.payload, prevState = actionQueue.state;
          if (node.isTransition) {
            var prevTransition = ReactSharedInternals.T, currentTransition = {};
            currentTransition.types = null !== prevTransition ? prevTransition.types : null;
            ReactSharedInternals.T = currentTransition;
            try {
              var returnValue = action(prevState, payload), onStartTransitionFinish = ReactSharedInternals.S;
              null !== onStartTransitionFinish && onStartTransitionFinish(currentTransition, returnValue);
              handleActionReturnValue(actionQueue, node, returnValue);
            } catch (error) {
              onActionError(actionQueue, node, error);
            } finally {
              null !== prevTransition && null !== currentTransition.types && (prevTransition.types = currentTransition.types), ReactSharedInternals.T = prevTransition;
            }
          } else
            try {
              prevTransition = action(prevState, payload), handleActionReturnValue(actionQueue, node, prevTransition);
            } catch (error$57) {
              onActionError(actionQueue, node, error$57);
            }
        }
        function handleActionReturnValue(actionQueue, node, returnValue) {
          null !== returnValue && "object" === typeof returnValue && "function" === typeof returnValue.then ? returnValue.then(
            function(nextState) {
              onActionSuccess(actionQueue, node, nextState);
            },
            function(error) {
              return onActionError(actionQueue, node, error);
            }
          ) : onActionSuccess(actionQueue, node, returnValue);
        }
        function onActionSuccess(actionQueue, actionNode, nextState) {
          actionNode.status = "fulfilled";
          actionNode.value = nextState;
          notifyActionListeners(actionNode);
          actionQueue.state = nextState;
          actionNode = actionQueue.pending;
          null !== actionNode && (nextState = actionNode.next, nextState === actionNode ? actionQueue.pending = null : (nextState = nextState.next, actionNode.next = nextState, runActionStateAction(actionQueue, nextState)));
        }
        function onActionError(actionQueue, actionNode, error) {
          var last = actionQueue.pending;
          actionQueue.pending = null;
          if (null !== last) {
            last = last.next;
            do
              actionNode.status = "rejected", actionNode.reason = error, notifyActionListeners(actionNode), actionNode = actionNode.next;
            while (actionNode !== last);
          }
          actionQueue.action = null;
        }
        function notifyActionListeners(actionNode) {
          actionNode = actionNode.listeners;
          for (var i = 0; i < actionNode.length; i++) (0, actionNode[i])();
        }
        function actionStateReducer(oldState, newState) {
          return newState;
        }
        function mountActionState(action, initialStateProp) {
          if (isHydrating) {
            var ssrFormState = workInProgressRoot.formState;
            if (null !== ssrFormState) {
              a: {
                var JSCompiler_inline_result = currentlyRenderingFiber;
                if (isHydrating) {
                  if (nextHydratableInstance) {
                    var markerInstance = canHydrateFormStateMarker(
                      nextHydratableInstance,
                      rootOrSingletonContext
                    );
                    if (markerInstance) {
                      nextHydratableInstance = getNextHydratableSibling(markerInstance);
                      JSCompiler_inline_result = isFormStateMarkerMatching(markerInstance);
                      break a;
                    }
                  }
                  throwOnHydrationMismatch(JSCompiler_inline_result);
                }
                JSCompiler_inline_result = false;
              }
              JSCompiler_inline_result && (initialStateProp = ssrFormState[0]);
            }
          }
          ssrFormState = mountWorkInProgressHook();
          ssrFormState.memoizedState = ssrFormState.baseState = initialStateProp;
          JSCompiler_inline_result = {
            pending: null,
            lanes: 0,
            dispatch: null,
            lastRenderedReducer: actionStateReducer,
            lastRenderedState: initialStateProp
          };
          ssrFormState.queue = JSCompiler_inline_result;
          ssrFormState = dispatchSetState.bind(
            null,
            currentlyRenderingFiber,
            JSCompiler_inline_result
          );
          JSCompiler_inline_result.dispatch = ssrFormState;
          JSCompiler_inline_result = mountStateImpl(false);
          var setPendingState = dispatchOptimisticSetState.bind(
            null,
            currentlyRenderingFiber,
            false,
            JSCompiler_inline_result.queue
          );
          JSCompiler_inline_result = mountWorkInProgressHook();
          markerInstance = {
            state: initialStateProp,
            dispatch: null,
            action,
            pending: null
          };
          JSCompiler_inline_result.queue = markerInstance;
          ssrFormState = dispatchActionState.bind(
            null,
            currentlyRenderingFiber,
            markerInstance,
            setPendingState,
            ssrFormState
          );
          markerInstance.dispatch = ssrFormState;
          JSCompiler_inline_result.memoizedState = action;
          return [initialStateProp, ssrFormState, false];
        }
        function updateActionState(action) {
          var stateHook = updateWorkInProgressHook();
          return updateActionStateImpl(stateHook, currentHook, action);
        }
        function updateActionStateImpl(stateHook, currentStateHook, action) {
          currentStateHook = updateReducerImpl(
            stateHook,
            currentStateHook,
            actionStateReducer
          )[0];
          stateHook = updateReducer(basicStateReducer)[0];
          if ("object" === typeof currentStateHook && null !== currentStateHook && "function" === typeof currentStateHook.then)
            try {
              var state = useThenable(currentStateHook);
            } catch (x) {
              if (x === SuspenseException) throw SuspenseActionException;
              throw x;
            }
          else state = currentStateHook;
          currentStateHook = updateWorkInProgressHook();
          var actionQueue = currentStateHook.queue, dispatch = actionQueue.dispatch;
          action !== currentStateHook.memoizedState && (currentlyRenderingFiber.flags |= 2048, pushSimpleEffect(
            9,
            { destroy: void 0 },
            actionStateActionEffect.bind(null, actionQueue, action),
            null
          ));
          return [state, dispatch, stateHook];
        }
        function actionStateActionEffect(actionQueue, action) {
          actionQueue.action = action;
        }
        function rerenderActionState(action) {
          var stateHook = updateWorkInProgressHook(), currentStateHook = currentHook;
          if (null !== currentStateHook)
            return updateActionStateImpl(stateHook, currentStateHook, action);
          updateWorkInProgressHook();
          stateHook = stateHook.memoizedState;
          currentStateHook = updateWorkInProgressHook();
          var dispatch = currentStateHook.queue.dispatch;
          currentStateHook.memoizedState = action;
          return [stateHook, dispatch, false];
        }
        function pushSimpleEffect(tag, inst, create, deps) {
          tag = { tag, create, deps, inst, next: null };
          inst = currentlyRenderingFiber.updateQueue;
          null === inst && (inst = createFunctionComponentUpdateQueue(), currentlyRenderingFiber.updateQueue = inst);
          create = inst.lastEffect;
          null === create ? inst.lastEffect = tag.next = tag : (deps = create.next, create.next = tag, tag.next = deps, inst.lastEffect = tag);
          return tag;
        }
        function updateRef() {
          return updateWorkInProgressHook().memoizedState;
        }
        function mountEffectImpl(fiberFlags, hookFlags, create, deps) {
          var hook = mountWorkInProgressHook();
          currentlyRenderingFiber.flags |= fiberFlags;
          hook.memoizedState = pushSimpleEffect(
            1 | hookFlags,
            { destroy: void 0 },
            create,
            void 0 === deps ? null : deps
          );
        }
        function updateEffectImpl(fiberFlags, hookFlags, create, deps) {
          var hook = updateWorkInProgressHook();
          deps = void 0 === deps ? null : deps;
          var inst = hook.memoizedState.inst;
          null !== currentHook && null !== deps && areHookInputsEqual(deps, currentHook.memoizedState.deps) ? hook.memoizedState = pushSimpleEffect(hookFlags, inst, create, deps) : (currentlyRenderingFiber.flags |= fiberFlags, hook.memoizedState = pushSimpleEffect(
            1 | hookFlags,
            inst,
            create,
            deps
          ));
        }
        function mountEffect(create, deps) {
          mountEffectImpl(8390656, 8, create, deps);
        }
        function updateEffect(create, deps) {
          updateEffectImpl(2048, 8, create, deps);
        }
        function useEffectEventImpl(payload) {
          currentlyRenderingFiber.flags |= 4;
          var componentUpdateQueue = currentlyRenderingFiber.updateQueue;
          if (null === componentUpdateQueue)
            componentUpdateQueue = createFunctionComponentUpdateQueue(), currentlyRenderingFiber.updateQueue = componentUpdateQueue, componentUpdateQueue.events = [payload];
          else {
            var events = componentUpdateQueue.events;
            null === events ? componentUpdateQueue.events = [payload] : events.push(payload);
          }
        }
        function updateEvent(callback) {
          var ref = updateWorkInProgressHook().memoizedState;
          useEffectEventImpl({ ref, nextImpl: callback });
          return function() {
            if (0 !== (executionContext & 2))
              throw Error(formatProdErrorMessage(440));
            return ref.impl.apply(void 0, arguments);
          };
        }
        function updateInsertionEffect(create, deps) {
          return updateEffectImpl(4, 2, create, deps);
        }
        function updateLayoutEffect(create, deps) {
          return updateEffectImpl(4, 4, create, deps);
        }
        function imperativeHandleEffect(create, ref) {
          if ("function" === typeof ref) {
            create = create();
            var refCleanup = ref(create);
            return function() {
              "function" === typeof refCleanup ? refCleanup() : ref(null);
            };
          }
          if (null !== ref && void 0 !== ref)
            return create = create(), ref.current = create, function() {
              ref.current = null;
            };
        }
        function updateImperativeHandle(ref, create, deps) {
          deps = null !== deps && void 0 !== deps ? deps.concat([ref]) : null;
          updateEffectImpl(
            4,
            4,
            imperativeHandleEffect.bind(null, create, ref),
            deps
          );
        }
        function mountDebugValue() {
        }
        function updateCallback(callback, deps) {
          var hook = updateWorkInProgressHook();
          deps = void 0 === deps ? null : deps;
          var prevState = hook.memoizedState;
          if (null !== deps && areHookInputsEqual(deps, prevState[1]))
            return prevState[0];
          hook.memoizedState = [callback, deps];
          return callback;
        }
        function updateMemo(nextCreate, deps) {
          var hook = updateWorkInProgressHook();
          deps = void 0 === deps ? null : deps;
          var prevState = hook.memoizedState;
          if (null !== deps && areHookInputsEqual(deps, prevState[1]))
            return prevState[0];
          prevState = nextCreate();
          if (shouldDoubleInvokeUserFnsInHooksDEV) {
            setIsStrictModeForDevtools(true);
            try {
              nextCreate();
            } finally {
              setIsStrictModeForDevtools(false);
            }
          }
          hook.memoizedState = [prevState, deps];
          return prevState;
        }
        function mountDeferredValueImpl(hook, value, initialValue) {
          if (void 0 === initialValue || 0 !== (renderLanes & 1073741824) && 0 === (workInProgressRootRenderLanes & 261930))
            return hook.memoizedState = value;
          hook.memoizedState = initialValue;
          hook = requestDeferredLane();
          currentlyRenderingFiber.lanes |= hook;
          workInProgressRootSkippedLanes |= hook;
          return initialValue;
        }
        function updateDeferredValueImpl(hook, prevValue, value, initialValue) {
          if (objectIs(value, prevValue)) return value;
          if (null !== currentTreeHiddenStackCursor.current)
            return hook = mountDeferredValueImpl(hook, value, initialValue), objectIs(hook, prevValue) || (didReceiveUpdate = true), hook;
          if (0 === (renderLanes & 106) || 0 !== (renderLanes & 1073741824) && 0 === (workInProgressRootRenderLanes & 261930))
            return didReceiveUpdate = true, hook.memoizedState = value;
          hook = requestDeferredLane();
          currentlyRenderingFiber.lanes |= hook;
          workInProgressRootSkippedLanes |= hook;
          return prevValue;
        }
        function startTransition(fiber, queue, pendingState, finishedState, callback) {
          var previousPriority = getCurrentUpdatePriority();
          setCurrentUpdatePriority(
            0 !== previousPriority && 8 > previousPriority ? previousPriority : 8
          );
          var prevTransition = ReactSharedInternals.T, currentTransition = {};
          currentTransition.types = null !== prevTransition ? prevTransition.types : null;
          ReactSharedInternals.T = currentTransition;
          dispatchOptimisticSetState(fiber, false, queue, pendingState);
          try {
            var returnValue = callback(), onStartTransitionFinish = ReactSharedInternals.S;
            null !== onStartTransitionFinish && onStartTransitionFinish(currentTransition, returnValue);
            if (null !== returnValue && "object" === typeof returnValue && "function" === typeof returnValue.then) {
              var thenableForFinishedState = chainThenableValue(
                returnValue,
                finishedState
              );
              dispatchSetStateInternal(
                fiber,
                queue,
                thenableForFinishedState,
                requestUpdateLane(fiber)
              );
            } else
              dispatchSetStateInternal(
                fiber,
                queue,
                finishedState,
                requestUpdateLane(fiber)
              );
          } catch (error) {
            dispatchSetStateInternal(
              fiber,
              queue,
              { then: function() {
              }, status: "rejected", reason: error },
              requestUpdateLane()
            );
          } finally {
            setCurrentUpdatePriority(previousPriority), null !== prevTransition && null !== currentTransition.types && (prevTransition.types = currentTransition.types), ReactSharedInternals.T = prevTransition;
          }
        }
        function ensureFormComponentIsStateful(formFiber) {
          var existingStateHook = formFiber.memoizedState;
          if (null !== existingStateHook) return existingStateHook;
          existingStateHook = {
            memoizedState: NotPendingTransition,
            baseState: NotPendingTransition,
            baseQueue: null,
            queue: {
              pending: null,
              lanes: 0,
              dispatch: null,
              lastRenderedReducer: basicStateReducer,
              lastRenderedState: NotPendingTransition
            },
            next: null
          };
          var initialResetState = {};
          existingStateHook.next = {
            memoizedState: initialResetState,
            baseState: initialResetState,
            baseQueue: null,
            queue: {
              pending: null,
              lanes: 0,
              dispatch: null,
              lastRenderedReducer: basicStateReducer,
              lastRenderedState: initialResetState
            },
            next: null
          };
          formFiber.memoizedState = existingStateHook;
          formFiber = formFiber.alternate;
          null !== formFiber && (formFiber.memoizedState = existingStateHook);
          return existingStateHook;
        }
        function useHostTransitionStatus() {
          return readContext(HostTransitionContext);
        }
        function updateId() {
          return updateWorkInProgressHook().memoizedState;
        }
        function updateRefresh() {
          return updateWorkInProgressHook().memoizedState;
        }
        function refreshCache(fiber) {
          for (var provider = fiber.return; null !== provider; ) {
            switch (provider.tag) {
              case 24:
              case 3:
                var lane = requestUpdateLane();
                fiber = createUpdate(lane);
                var root2 = enqueueUpdate(provider, fiber, lane);
                null !== root2 && (scheduleUpdateOnFiber(root2, provider, lane), entangleTransitions(root2, provider, lane));
                provider = { cache: createCache() };
                fiber.payload = provider;
                return;
            }
            provider = provider.return;
          }
        }
        function dispatchReducerAction(fiber, queue, action) {
          var lane = requestUpdateLane();
          action = {
            lane,
            revertLane: 0,
            gesture: null,
            action,
            hasEagerState: false,
            eagerState: null,
            next: null
          };
          isRenderPhaseUpdate(fiber) ? enqueueRenderPhaseUpdate(queue, action) : (action = enqueueConcurrentHookUpdate(fiber, queue, action, lane), null !== action && (scheduleUpdateOnFiber(action, fiber, lane), entangleTransitionUpdate(action, queue, lane)));
        }
        function dispatchSetState(fiber, queue, action) {
          var lane = requestUpdateLane();
          dispatchSetStateInternal(fiber, queue, action, lane);
        }
        function dispatchSetStateInternal(fiber, queue, action, lane) {
          var update = {
            lane,
            revertLane: 0,
            gesture: null,
            action,
            hasEagerState: false,
            eagerState: null,
            next: null
          };
          if (isRenderPhaseUpdate(fiber)) enqueueRenderPhaseUpdate(queue, update);
          else {
            var alternate = fiber.alternate;
            if (0 === fiber.lanes && (null === alternate || 0 === alternate.lanes) && (alternate = queue.lastRenderedReducer, null !== alternate))
              try {
                var currentState = queue.lastRenderedState, eagerState = alternate(currentState, action);
                update.hasEagerState = true;
                update.eagerState = eagerState;
                if (objectIs(eagerState, currentState))
                  return enqueueUpdate$1(fiber, queue, update, 0), null === workInProgressRoot && finishQueueingConcurrentUpdates(), false;
              } catch (error) {
              } finally {
              }
            action = enqueueConcurrentHookUpdate(fiber, queue, update, lane);
            if (null !== action)
              return scheduleUpdateOnFiber(action, fiber, lane), entangleTransitionUpdate(action, queue, lane), true;
          }
          return false;
        }
        function dispatchOptimisticSetState(fiber, throwIfDuringRender, queue, action) {
          action = {
            lane: 2,
            revertLane: requestTransitionLane(),
            gesture: null,
            action,
            hasEagerState: false,
            eagerState: null,
            next: null
          };
          if (isRenderPhaseUpdate(fiber)) {
            if (throwIfDuringRender) throw Error(formatProdErrorMessage(479));
          } else
            throwIfDuringRender = enqueueConcurrentHookUpdate(
              fiber,
              queue,
              action,
              2
            ), null !== throwIfDuringRender && scheduleUpdateOnFiber(throwIfDuringRender, fiber, 2);
        }
        function isRenderPhaseUpdate(fiber) {
          var alternate = fiber.alternate;
          return fiber === currentlyRenderingFiber || null !== alternate && alternate === currentlyRenderingFiber;
        }
        function enqueueRenderPhaseUpdate(queue, update) {
          didScheduleRenderPhaseUpdateDuringThisPass = didScheduleRenderPhaseUpdate = true;
          var pending = queue.pending;
          null === pending ? update.next = update : (update.next = pending.next, pending.next = update);
          queue.pending = update;
        }
        function entangleTransitionUpdate(root2, queue, lane) {
          if (0 !== (lane & 4194048)) {
            var queueLanes = queue.lanes;
            queueLanes &= root2.pendingLanes;
            lane |= queueLanes;
            queue.lanes = lane;
            markRootEntangled(root2, lane);
          }
        }
        function applyDerivedStateFromProps(workInProgress2, ctor, getDerivedStateFromProps, nextProps) {
          ctor = workInProgress2.memoizedState;
          getDerivedStateFromProps = getDerivedStateFromProps(nextProps, ctor);
          getDerivedStateFromProps = null === getDerivedStateFromProps || void 0 === getDerivedStateFromProps ? ctor : assign({}, ctor, getDerivedStateFromProps);
          workInProgress2.memoizedState = getDerivedStateFromProps;
          0 === workInProgress2.lanes && (workInProgress2.updateQueue.baseState = getDerivedStateFromProps);
        }
        function checkShouldComponentUpdate(workInProgress2, ctor, oldProps, newProps, oldState, newState, nextContext) {
          workInProgress2 = workInProgress2.stateNode;
          return "function" === typeof workInProgress2.shouldComponentUpdate ? workInProgress2.shouldComponentUpdate(newProps, newState, nextContext) : ctor.prototype && ctor.prototype.isPureReactComponent ? !shallowEqual(oldProps, newProps) || !shallowEqual(oldState, newState) : true;
        }
        function callComponentWillReceiveProps(workInProgress2, instance, newProps, nextContext) {
          workInProgress2 = instance.state;
          "function" === typeof instance.componentWillReceiveProps && instance.componentWillReceiveProps(newProps, nextContext);
          "function" === typeof instance.UNSAFE_componentWillReceiveProps && instance.UNSAFE_componentWillReceiveProps(newProps, nextContext);
          instance.state !== workInProgress2 && classComponentUpdater.enqueueReplaceState(instance, instance.state, null);
        }
        function resolveClassComponentProps(Component, baseProps) {
          var newProps = baseProps;
          if ("ref" in baseProps) {
            newProps = {};
            for (var propName in baseProps)
              "ref" !== propName && (newProps[propName] = baseProps[propName]);
          }
          if (Component = Component.defaultProps) {
            newProps === baseProps && (newProps = assign({}, newProps));
            for (var propName$59 in Component)
              void 0 === newProps[propName$59] && (newProps[propName$59] = Component[propName$59]);
          }
          return newProps;
        }
        function logUncaughtError(root2, errorInfo) {
          try {
            var onUncaughtError = root2.onUncaughtError;
            onUncaughtError(errorInfo.value, { componentStack: errorInfo.stack });
          } catch (e) {
            setTimeout(function() {
              throw e;
            });
          }
        }
        function logCaughtError(root2, boundary, errorInfo) {
          try {
            var onCaughtError = root2.onCaughtError;
            onCaughtError(errorInfo.value, {
              componentStack: errorInfo.stack,
              errorBoundary: 1 === boundary.tag ? boundary.stateNode : null
            });
          } catch (e) {
            setTimeout(function() {
              throw e;
            });
          }
        }
        function createRootErrorUpdate(root2, errorInfo, lane) {
          lane = createUpdate(lane);
          lane.tag = 3;
          lane.payload = { element: null };
          lane.callback = function() {
            logUncaughtError(root2, errorInfo);
          };
          return lane;
        }
        function createClassErrorUpdate(lane) {
          lane = createUpdate(lane);
          lane.tag = 3;
          return lane;
        }
        function initializeClassErrorUpdate(update, root2, fiber, errorInfo) {
          var getDerivedStateFromError = fiber.type.getDerivedStateFromError;
          if ("function" === typeof getDerivedStateFromError) {
            var error = errorInfo.value;
            update.payload = function() {
              return getDerivedStateFromError(error);
            };
            update.callback = function() {
              logCaughtError(root2, fiber, errorInfo);
            };
          }
          var inst = fiber.stateNode;
          null !== inst && "function" === typeof inst.componentDidCatch && (update.callback = function() {
            logCaughtError(root2, fiber, errorInfo);
            "function" !== typeof getDerivedStateFromError && (null === legacyErrorBoundariesThatAlreadyFailed ? legacyErrorBoundariesThatAlreadyFailed = /* @__PURE__ */ new Set([this]) : legacyErrorBoundariesThatAlreadyFailed.add(this));
            var stack = errorInfo.stack;
            this.componentDidCatch(errorInfo.value, {
              componentStack: null !== stack ? stack : ""
            });
          });
        }
        function throwException(root2, returnFiber, sourceFiber, value, rootRenderLanes) {
          sourceFiber.flags |= 32768;
          if (null !== value && "object" === typeof value && "function" === typeof value.then) {
            returnFiber = sourceFiber.alternate;
            null !== returnFiber && propagateParentContextChanges(
              returnFiber,
              sourceFiber,
              rootRenderLanes,
              true
            );
            sourceFiber = suspenseHandlerStackCursor.current;
            if (null !== sourceFiber) {
              switch (sourceFiber.tag) {
                case 31:
                case 13:
                case 19:
                  return null === shellBoundary ? renderDidSuspendDelayIfPossible() : null === sourceFiber.alternate && 0 === workInProgressRootExitStatus && (workInProgressRootExitStatus = 3), sourceFiber.flags &= -257, sourceFiber.flags |= 65536, sourceFiber.lanes = rootRenderLanes, value === noopSuspenseyCommitThenable ? sourceFiber.flags |= 16384 : (returnFiber = sourceFiber.updateQueue, null === returnFiber ? sourceFiber.updateQueue = /* @__PURE__ */ new Set([value]) : returnFiber.add(value), attachPingListener(root2, value, rootRenderLanes)), false;
                case 22:
                  return sourceFiber.flags |= 65536, value === noopSuspenseyCommitThenable ? sourceFiber.flags |= 16384 : (returnFiber = sourceFiber.updateQueue, null === returnFiber ? (returnFiber = {
                    transitions: null,
                    markerInstances: null,
                    retryQueue: /* @__PURE__ */ new Set([value])
                  }, sourceFiber.updateQueue = returnFiber) : (sourceFiber = returnFiber.retryQueue, null === sourceFiber ? returnFiber.retryQueue = /* @__PURE__ */ new Set([value]) : sourceFiber.add(value)), attachPingListener(root2, value, rootRenderLanes)), false;
              }
              throw Error(formatProdErrorMessage(435, sourceFiber.tag));
            }
            attachPingListener(root2, value, rootRenderLanes);
            renderDidSuspendDelayIfPossible();
            return false;
          }
          if (isHydrating)
            return returnFiber = suspenseHandlerStackCursor.current, null !== returnFiber ? (0 === (returnFiber.flags & 65536) && (returnFiber.flags |= 256), returnFiber.flags |= 65536, returnFiber.lanes = rootRenderLanes, value !== HydrationMismatchException && (root2 = Error(formatProdErrorMessage(422), { cause: value }), queueHydrationError(
              createCapturedValueAtFiber(root2, sourceFiber)
            ))) : (value !== HydrationMismatchException && (returnFiber = Error(formatProdErrorMessage(423), {
              cause: value
            }), queueHydrationError(
              createCapturedValueAtFiber(returnFiber, sourceFiber)
            )), root2 = root2.current.alternate, root2.flags |= 65536, rootRenderLanes &= -rootRenderLanes, root2.lanes |= rootRenderLanes, value = createCapturedValueAtFiber(value, sourceFiber), rootRenderLanes = createRootErrorUpdate(
              root2.stateNode,
              value,
              rootRenderLanes
            ), enqueueCapturedUpdate(root2, rootRenderLanes), 4 !== workInProgressRootExitStatus && (workInProgressRootExitStatus = 2)), false;
          var wrapperError = Error(formatProdErrorMessage(520), { cause: value });
          wrapperError = createCapturedValueAtFiber(wrapperError, sourceFiber);
          null === workInProgressRootConcurrentErrors ? workInProgressRootConcurrentErrors = [wrapperError] : workInProgressRootConcurrentErrors.push(wrapperError);
          4 !== workInProgressRootExitStatus && (workInProgressRootExitStatus = 2);
          if (null === returnFiber) return true;
          value = createCapturedValueAtFiber(value, sourceFiber);
          sourceFiber = returnFiber;
          do {
            switch (sourceFiber.tag) {
              case 3:
                return sourceFiber.flags |= 65536, root2 = rootRenderLanes & -rootRenderLanes, sourceFiber.lanes |= root2, root2 = createRootErrorUpdate(sourceFiber.stateNode, value, root2), enqueueCapturedUpdate(sourceFiber, root2), false;
              case 1:
                returnFiber = sourceFiber.type;
                wrapperError = sourceFiber.stateNode;
                if (0 === (sourceFiber.flags & 128) && ("function" === typeof returnFiber.getDerivedStateFromError || null !== wrapperError && "function" === typeof wrapperError.componentDidCatch && (null === legacyErrorBoundariesThatAlreadyFailed || !legacyErrorBoundariesThatAlreadyFailed.has(wrapperError))))
                  return sourceFiber.flags |= 65536, rootRenderLanes &= -rootRenderLanes, sourceFiber.lanes |= rootRenderLanes, rootRenderLanes = createClassErrorUpdate(rootRenderLanes), initializeClassErrorUpdate(
                    rootRenderLanes,
                    root2,
                    sourceFiber,
                    value
                  ), enqueueCapturedUpdate(sourceFiber, rootRenderLanes), false;
                break;
              case 22:
                if (null !== sourceFiber.memoizedState)
                  return sourceFiber.flags |= 65536, false;
            }
            sourceFiber = sourceFiber.return;
          } while (null !== sourceFiber);
          return false;
        }
        function reconcileChildren(current, workInProgress2, nextChildren, renderLanes2) {
          workInProgress2.child = null === current ? mountChildFibers(workInProgress2, null, nextChildren, renderLanes2) : reconcileChildFibers(
            workInProgress2,
            current.child,
            nextChildren,
            renderLanes2
          );
        }
        function updateForwardRef(current, workInProgress2, Component, nextProps, renderLanes2) {
          Component = Component.render;
          var ref = workInProgress2.ref;
          if ("ref" in nextProps) {
            var propsWithoutRef = {};
            for (var key in nextProps)
              "ref" !== key && (propsWithoutRef[key] = nextProps[key]);
          } else propsWithoutRef = nextProps;
          prepareToReadContext(workInProgress2);
          nextProps = renderWithHooks(
            current,
            workInProgress2,
            Component,
            propsWithoutRef,
            ref,
            renderLanes2
          );
          key = checkDidRenderIdHook();
          if (null !== current && !didReceiveUpdate)
            return bailoutHooks(current, workInProgress2, renderLanes2), bailoutOnAlreadyFinishedWork(current, workInProgress2, renderLanes2);
          isHydrating && key && pushMaterializedTreeId(workInProgress2);
          workInProgress2.flags |= 1;
          reconcileChildren(current, workInProgress2, nextProps, renderLanes2);
          return workInProgress2.child;
        }
        function updateMemoComponent(current, workInProgress2, Component, nextProps, renderLanes2) {
          if (null === current) {
            var type = Component.type;
            if ("function" === typeof type && !shouldConstruct(type) && void 0 === type.defaultProps && null === Component.compare)
              return workInProgress2.tag = 15, workInProgress2.type = type, updateSimpleMemoComponent(
                current,
                workInProgress2,
                type,
                nextProps,
                renderLanes2
              );
            current = createFiberFromTypeAndProps(
              Component.type,
              null,
              nextProps,
              workInProgress2,
              workInProgress2.mode,
              renderLanes2
            );
            current.ref = workInProgress2.ref;
            current.return = workInProgress2;
            return workInProgress2.child = current;
          }
          type = current.child;
          if (!checkScheduledUpdateOrContext(current, renderLanes2)) {
            var prevProps = type.memoizedProps;
            Component = Component.compare;
            Component = null !== Component ? Component : shallowEqual;
            if (Component(prevProps, nextProps) && current.ref === workInProgress2.ref)
              return bailoutOnAlreadyFinishedWork(
                current,
                workInProgress2,
                renderLanes2
              );
          }
          workInProgress2.flags |= 1;
          current = createWorkInProgress(type, nextProps);
          current.ref = workInProgress2.ref;
          current.return = workInProgress2;
          return workInProgress2.child = current;
        }
        function updateSimpleMemoComponent(current, workInProgress2, Component, nextProps, renderLanes2) {
          if (null !== current) {
            var prevProps = current.memoizedProps;
            if (shallowEqual(prevProps, nextProps) && current.ref === workInProgress2.ref)
              if (didReceiveUpdate = false, workInProgress2.pendingProps = nextProps = prevProps, checkScheduledUpdateOrContext(current, renderLanes2))
                0 !== (current.flags & 131072) && (didReceiveUpdate = true);
              else
                return workInProgress2.lanes = current.lanes, bailoutOnAlreadyFinishedWork(current, workInProgress2, renderLanes2);
          }
          return updateFunctionComponent(
            current,
            workInProgress2,
            Component,
            nextProps,
            renderLanes2
          );
        }
        function updateOffscreenComponent(current, workInProgress2, renderLanes2, nextProps) {
          var nextChildren = nextProps.children, prevState = null !== current ? current.memoizedState : null;
          null === current && null === workInProgress2.stateNode && (workInProgress2.stateNode = {
            _visibility: 1,
            _pendingMarkers: null,
            _retryCache: null,
            _transitions: null
          });
          if ("hidden" === nextProps.mode) {
            if (0 !== (workInProgress2.flags & 128)) {
              prevState = null !== prevState ? prevState.baseLanes | renderLanes2 : renderLanes2;
              if (null !== current) {
                nextProps = workInProgress2.child = current.child;
                for (nextChildren = 0; null !== nextProps; )
                  nextChildren = nextChildren | nextProps.lanes | nextProps.childLanes, nextProps = nextProps.sibling;
                nextProps = nextChildren & ~prevState;
              } else nextProps = 0, workInProgress2.child = null;
              return deferHiddenOffscreenComponent(
                current,
                workInProgress2,
                prevState,
                renderLanes2,
                nextProps
              );
            }
            if (0 !== (renderLanes2 & 536870912))
              workInProgress2.memoizedState = { baseLanes: 0, cachePool: null }, null !== current && pushTransition(
                workInProgress2,
                null !== prevState ? prevState.cachePool : null
              ), null !== prevState ? pushHiddenContext(workInProgress2, prevState) : reuseHiddenContextOnStack(), pushOffscreenSuspenseHandler(workInProgress2);
            else
              return nextProps = workInProgress2.lanes = 536870912, deferHiddenOffscreenComponent(
                current,
                workInProgress2,
                null !== prevState ? prevState.baseLanes | renderLanes2 : renderLanes2,
                renderLanes2,
                nextProps
              );
          } else
            null !== prevState ? (pushTransition(workInProgress2, prevState.cachePool), pushHiddenContext(workInProgress2, prevState), reuseSuspenseHandlerOnStack(), workInProgress2.memoizedState = null) : (null !== current && pushTransition(workInProgress2, null), reuseHiddenContextOnStack(), reuseSuspenseHandlerOnStack());
          reconcileChildren(current, workInProgress2, nextChildren, renderLanes2);
          return workInProgress2.child;
        }
        function bailoutOffscreenComponent(current, workInProgress2) {
          null !== current && 22 === current.tag || null !== workInProgress2.stateNode || (workInProgress2.stateNode = {
            _visibility: 1,
            _pendingMarkers: null,
            _retryCache: null,
            _transitions: null
          });
          return workInProgress2.sibling;
        }
        function deferHiddenOffscreenComponent(current, workInProgress2, nextBaseLanes, renderLanes2, remainingChildLanes) {
          var JSCompiler_inline_result = peekCacheFromPool();
          JSCompiler_inline_result = null === JSCompiler_inline_result ? null : {
            parent: isPrimaryRenderer ? CacheContext._currentValue : CacheContext._currentValue2,
            pool: JSCompiler_inline_result
          };
          workInProgress2.memoizedState = {
            baseLanes: nextBaseLanes,
            cachePool: JSCompiler_inline_result
          };
          null !== current && pushTransition(workInProgress2, null);
          reuseHiddenContextOnStack();
          pushOffscreenSuspenseHandler(workInProgress2);
          null !== current && propagateParentContextChanges(current, workInProgress2, renderLanes2, true);
          workInProgress2.childLanes = remainingChildLanes;
          return null;
        }
        function mountActivityChildren(workInProgress2, nextProps) {
          nextProps = mountWorkInProgressOffscreenFiber(
            { mode: nextProps.mode, children: nextProps.children },
            workInProgress2.mode
          );
          nextProps.ref = workInProgress2.ref;
          workInProgress2.child = nextProps;
          nextProps.return = workInProgress2;
          return nextProps;
        }
        function retryActivityComponentWithoutHydrating(current, workInProgress2, renderLanes2) {
          reconcileChildFibers(workInProgress2, current.child, null, renderLanes2);
          current = mountActivityChildren(
            workInProgress2,
            workInProgress2.pendingProps
          );
          current.flags |= 2;
          popSuspenseHandler(workInProgress2);
          workInProgress2.memoizedState = null;
          return current;
        }
        function updateActivityComponent(current, workInProgress2, renderLanes2) {
          var nextProps = workInProgress2.pendingProps, didSuspend = 0 !== (workInProgress2.flags & 128);
          workInProgress2.flags &= -129;
          if (null === current) {
            if (isHydrating) {
              if ("hidden" === nextProps.mode)
                return current = mountActivityChildren(workInProgress2, nextProps), workInProgress2.lanes = 536870912, current.memoizedState = { baseLanes: 0, cachePool: null }, bailoutOffscreenComponent(null, current);
              pushDehydratedActivitySuspenseHandler(workInProgress2);
              (current = nextHydratableInstance) ? (current = canHydrateActivityInstance(
                current,
                rootOrSingletonContext
              ), null !== current && (workInProgress2.memoizedState = {
                dehydrated: current,
                treeContext: null !== treeContextProvider ? { id: treeContextId, overflow: treeContextOverflow } : null,
                retryLane: 536870912,
                hydrationErrors: null
              }, renderLanes2 = createFiberFromDehydratedFragment(current), renderLanes2.return = workInProgress2, workInProgress2.child = renderLanes2, hydrationParentFiber = workInProgress2, nextHydratableInstance = null)) : current = null;
              if (null === current) throw throwOnHydrationMismatch(workInProgress2);
              workInProgress2.lanes = 536870912;
              return null;
            }
            return mountActivityChildren(workInProgress2, nextProps);
          }
          var prevState = current.memoizedState;
          if (null !== prevState) {
            var dehydrated = prevState.dehydrated;
            pushDehydratedActivitySuspenseHandler(workInProgress2);
            if (didSuspend)
              if (workInProgress2.flags & 256)
                workInProgress2.flags &= -257, workInProgress2 = retryActivityComponentWithoutHydrating(
                  current,
                  workInProgress2,
                  renderLanes2
                );
              else if (null !== workInProgress2.memoizedState)
                workInProgress2.child = current.child, workInProgress2.flags |= 128, workInProgress2 = null;
              else throw Error(formatProdErrorMessage(558));
            else if (didReceiveUpdate || propagateParentContextChanges(
              current,
              workInProgress2,
              renderLanes2,
              false
            ), didSuspend = 0 !== (renderLanes2 & current.childLanes), didReceiveUpdate || didSuspend) {
              if (null === currentTreeHiddenStackCursor.current) {
                nextProps = workInProgressRoot;
                if (null !== nextProps && (dehydrated = getBumpedLaneForHydration(nextProps, renderLanes2), 0 !== dehydrated && dehydrated !== prevState.retryLane))
                  throw prevState.retryLane = dehydrated, enqueueConcurrentRenderForLane(current, dehydrated), scheduleUpdateOnFiber(nextProps, current, dehydrated), SelectiveHydrationException;
                renderDidSuspendDelayIfPossible();
              }
              workInProgress2 = retryActivityComponentWithoutHydrating(
                current,
                workInProgress2,
                renderLanes2
              );
            } else
              current = prevState.treeContext, supportsHydration && (nextHydratableInstance = getFirstHydratableChildWithinActivityInstance(dehydrated), hydrationParentFiber = workInProgress2, isHydrating = true, hydrationErrors = null, rootOrSingletonContext = false, null !== current && restoreSuspendedTreeContext(workInProgress2, current)), workInProgress2 = mountActivityChildren(workInProgress2, nextProps), workInProgress2.flags |= 134221824;
            return workInProgress2;
          }
          current = createWorkInProgress(current.child, {
            mode: nextProps.mode,
            children: nextProps.children
          });
          current.ref = workInProgress2.ref;
          workInProgress2.child = current;
          current.return = workInProgress2;
          return current;
        }
        function markRef(current, workInProgress2) {
          var ref = workInProgress2.ref;
          if (null === ref)
            null !== current && null !== current.ref && (workInProgress2.flags |= 4194816);
          else {
            if ("function" !== typeof ref && "object" !== typeof ref)
              throw Error(formatProdErrorMessage(284));
            if (null === current || current.ref !== ref)
              workInProgress2.flags |= 4194816;
          }
        }
        function updateFunctionComponent(current, workInProgress2, Component, nextProps, renderLanes2) {
          prepareToReadContext(workInProgress2);
          Component = renderWithHooks(
            current,
            workInProgress2,
            Component,
            nextProps,
            void 0,
            renderLanes2
          );
          nextProps = checkDidRenderIdHook();
          if (null !== current && !didReceiveUpdate)
            return bailoutHooks(current, workInProgress2, renderLanes2), bailoutOnAlreadyFinishedWork(current, workInProgress2, renderLanes2);
          isHydrating && nextProps && pushMaterializedTreeId(workInProgress2);
          workInProgress2.flags |= 1;
          reconcileChildren(current, workInProgress2, Component, renderLanes2);
          return workInProgress2.child;
        }
        function replayFunctionComponent(current, workInProgress2, nextProps, Component, secondArg, renderLanes2) {
          prepareToReadContext(workInProgress2);
          workInProgress2.updateQueue = null;
          nextProps = renderWithHooksAgain(
            workInProgress2,
            Component,
            nextProps,
            secondArg
          );
          finishRenderingHooks(current);
          Component = checkDidRenderIdHook();
          if (null !== current && !didReceiveUpdate)
            return bailoutHooks(current, workInProgress2, renderLanes2), bailoutOnAlreadyFinishedWork(current, workInProgress2, renderLanes2);
          isHydrating && Component && pushMaterializedTreeId(workInProgress2);
          workInProgress2.flags |= 1;
          reconcileChildren(current, workInProgress2, nextProps, renderLanes2);
          return workInProgress2.child;
        }
        function updateClassComponent(current, workInProgress2, Component, nextProps, renderLanes2) {
          prepareToReadContext(workInProgress2);
          if (null === workInProgress2.stateNode) {
            var context = emptyContextObject, contextType = Component.contextType;
            "object" === typeof contextType && null !== contextType && (context = readContext(contextType));
            context = new Component(nextProps, context);
            workInProgress2.memoizedState = null !== context.state && void 0 !== context.state ? context.state : null;
            context.updater = classComponentUpdater;
            workInProgress2.stateNode = context;
            context._reactInternals = workInProgress2;
            context = workInProgress2.stateNode;
            context.props = nextProps;
            context.state = workInProgress2.memoizedState;
            context.refs = {};
            initializeUpdateQueue(workInProgress2);
            contextType = Component.contextType;
            context.context = "object" === typeof contextType && null !== contextType ? readContext(contextType) : emptyContextObject;
            context.state = workInProgress2.memoizedState;
            contextType = Component.getDerivedStateFromProps;
            "function" === typeof contextType && (applyDerivedStateFromProps(
              workInProgress2,
              Component,
              contextType,
              nextProps
            ), context.state = workInProgress2.memoizedState);
            "function" === typeof Component.getDerivedStateFromProps || "function" === typeof context.getSnapshotBeforeUpdate || "function" !== typeof context.UNSAFE_componentWillMount && "function" !== typeof context.componentWillMount || (contextType = context.state, "function" === typeof context.componentWillMount && context.componentWillMount(), "function" === typeof context.UNSAFE_componentWillMount && context.UNSAFE_componentWillMount(), contextType !== context.state && classComponentUpdater.enqueueReplaceState(
              context,
              context.state,
              null
            ), processUpdateQueue(workInProgress2, nextProps, context, renderLanes2), suspendIfUpdateReadFromEntangledAsyncAction(), context.state = workInProgress2.memoizedState);
            "function" === typeof context.componentDidMount && (workInProgress2.flags |= 4194308);
            nextProps = true;
          } else if (null === current) {
            context = workInProgress2.stateNode;
            var unresolvedOldProps = workInProgress2.memoizedProps, oldProps = resolveClassComponentProps(Component, unresolvedOldProps);
            context.props = oldProps;
            var oldContext = context.context, contextType$jscomp$0 = Component.contextType;
            contextType = emptyContextObject;
            "object" === typeof contextType$jscomp$0 && null !== contextType$jscomp$0 && (contextType = readContext(contextType$jscomp$0));
            var getDerivedStateFromProps = Component.getDerivedStateFromProps;
            contextType$jscomp$0 = "function" === typeof getDerivedStateFromProps || "function" === typeof context.getSnapshotBeforeUpdate;
            unresolvedOldProps = workInProgress2.pendingProps !== unresolvedOldProps;
            contextType$jscomp$0 || "function" !== typeof context.UNSAFE_componentWillReceiveProps && "function" !== typeof context.componentWillReceiveProps || (unresolvedOldProps || oldContext !== contextType) && callComponentWillReceiveProps(
              workInProgress2,
              context,
              nextProps,
              contextType
            );
            hasForceUpdate = false;
            var oldState = workInProgress2.memoizedState;
            context.state = oldState;
            processUpdateQueue(workInProgress2, nextProps, context, renderLanes2);
            suspendIfUpdateReadFromEntangledAsyncAction();
            oldContext = workInProgress2.memoizedState;
            unresolvedOldProps || oldState !== oldContext || hasForceUpdate ? ("function" === typeof getDerivedStateFromProps && (applyDerivedStateFromProps(
              workInProgress2,
              Component,
              getDerivedStateFromProps,
              nextProps
            ), oldContext = workInProgress2.memoizedState), (oldProps = hasForceUpdate || checkShouldComponentUpdate(
              workInProgress2,
              Component,
              oldProps,
              nextProps,
              oldState,
              oldContext,
              contextType
            )) ? (contextType$jscomp$0 || "function" !== typeof context.UNSAFE_componentWillMount && "function" !== typeof context.componentWillMount || ("function" === typeof context.componentWillMount && context.componentWillMount(), "function" === typeof context.UNSAFE_componentWillMount && context.UNSAFE_componentWillMount()), "function" === typeof context.componentDidMount && (workInProgress2.flags |= 4194308)) : ("function" === typeof context.componentDidMount && (workInProgress2.flags |= 4194308), workInProgress2.memoizedProps = nextProps, workInProgress2.memoizedState = oldContext), context.props = nextProps, context.state = oldContext, context.context = contextType, nextProps = oldProps) : ("function" === typeof context.componentDidMount && (workInProgress2.flags |= 4194308), nextProps = false);
          } else {
            context = workInProgress2.stateNode;
            cloneUpdateQueue(current, workInProgress2);
            contextType = workInProgress2.memoizedProps;
            contextType$jscomp$0 = resolveClassComponentProps(Component, contextType);
            context.props = contextType$jscomp$0;
            getDerivedStateFromProps = workInProgress2.pendingProps;
            oldState = context.context;
            oldContext = Component.contextType;
            oldProps = emptyContextObject;
            "object" === typeof oldContext && null !== oldContext && (oldProps = readContext(oldContext));
            unresolvedOldProps = Component.getDerivedStateFromProps;
            (oldContext = "function" === typeof unresolvedOldProps || "function" === typeof context.getSnapshotBeforeUpdate) || "function" !== typeof context.UNSAFE_componentWillReceiveProps && "function" !== typeof context.componentWillReceiveProps || (contextType !== getDerivedStateFromProps || oldState !== oldProps) && callComponentWillReceiveProps(
              workInProgress2,
              context,
              nextProps,
              oldProps
            );
            hasForceUpdate = false;
            oldState = workInProgress2.memoizedState;
            context.state = oldState;
            processUpdateQueue(workInProgress2, nextProps, context, renderLanes2);
            suspendIfUpdateReadFromEntangledAsyncAction();
            var newState = workInProgress2.memoizedState;
            contextType !== getDerivedStateFromProps || oldState !== newState || hasForceUpdate || null !== current && null !== current.dependencies && checkIfContextChanged(current.dependencies) ? ("function" === typeof unresolvedOldProps && (applyDerivedStateFromProps(
              workInProgress2,
              Component,
              unresolvedOldProps,
              nextProps
            ), newState = workInProgress2.memoizedState), (contextType$jscomp$0 = hasForceUpdate || checkShouldComponentUpdate(
              workInProgress2,
              Component,
              contextType$jscomp$0,
              nextProps,
              oldState,
              newState,
              oldProps
            ) || null !== current && null !== current.dependencies && checkIfContextChanged(current.dependencies)) ? (oldContext || "function" !== typeof context.UNSAFE_componentWillUpdate && "function" !== typeof context.componentWillUpdate || ("function" === typeof context.componentWillUpdate && context.componentWillUpdate(nextProps, newState, oldProps), "function" === typeof context.UNSAFE_componentWillUpdate && context.UNSAFE_componentWillUpdate(
              nextProps,
              newState,
              oldProps
            )), "function" === typeof context.componentDidUpdate && (workInProgress2.flags |= 4), "function" === typeof context.getSnapshotBeforeUpdate && (workInProgress2.flags |= 1024)) : ("function" !== typeof context.componentDidUpdate || contextType === current.memoizedProps && oldState === current.memoizedState || (workInProgress2.flags |= 4), "function" !== typeof context.getSnapshotBeforeUpdate || contextType === current.memoizedProps && oldState === current.memoizedState || (workInProgress2.flags |= 1024), workInProgress2.memoizedProps = nextProps, workInProgress2.memoizedState = newState), context.props = nextProps, context.state = newState, context.context = oldProps, nextProps = contextType$jscomp$0) : ("function" !== typeof context.componentDidUpdate || contextType === current.memoizedProps && oldState === current.memoizedState || (workInProgress2.flags |= 4), "function" !== typeof context.getSnapshotBeforeUpdate || contextType === current.memoizedProps && oldState === current.memoizedState || (workInProgress2.flags |= 1024), nextProps = false);
          }
          context = nextProps;
          markRef(current, workInProgress2);
          nextProps = 0 !== (workInProgress2.flags & 128);
          context || nextProps ? (context = workInProgress2.stateNode, Component = nextProps && "function" !== typeof Component.getDerivedStateFromError ? null : context.render(), workInProgress2.flags |= 1, null !== current && nextProps ? (workInProgress2.child = reconcileChildFibers(
            workInProgress2,
            current.child,
            null,
            renderLanes2
          ), workInProgress2.child = reconcileChildFibers(
            workInProgress2,
            null,
            Component,
            renderLanes2
          )) : reconcileChildren(current, workInProgress2, Component, renderLanes2), workInProgress2.memoizedState = context.state, current = workInProgress2.child) : current = bailoutOnAlreadyFinishedWork(
            current,
            workInProgress2,
            renderLanes2
          );
          return current;
        }
        function mountHostRootWithoutHydrating(current, workInProgress2, nextChildren, renderLanes2) {
          resetHydrationState();
          workInProgress2.flags |= 256;
          reconcileChildren(current, workInProgress2, nextChildren, renderLanes2);
          return workInProgress2.child;
        }
        function mountSuspenseOffscreenState(renderLanes2) {
          return { baseLanes: renderLanes2, cachePool: getSuspendedCache() };
        }
        function getRemainingWorkInPrimaryTree(current, primaryTreeDidDefer, renderLanes2) {
          current = null !== current ? current.childLanes & ~renderLanes2 : 0;
          primaryTreeDidDefer && (current |= workInProgressDeferredLane);
          return current;
        }
        function updateSuspenseComponent(current, workInProgress2, renderLanes2) {
          var nextProps = workInProgress2.pendingProps, showFallback = false, didSuspend = 0 !== (workInProgress2.flags & 128), JSCompiler_temp;
          (JSCompiler_temp = didSuspend) || (JSCompiler_temp = null !== current && null === current.memoizedState ? false : 0 !== (suspenseStackCursor.current & 2));
          JSCompiler_temp && (showFallback = true, workInProgress2.flags &= -129);
          JSCompiler_temp = 0 !== (workInProgress2.flags & 32);
          workInProgress2.flags &= -33;
          if (null === current) {
            if (isHydrating) {
              showFallback ? pushPrimaryTreeSuspenseHandler(workInProgress2) : reuseSuspenseHandlerOnStack();
              (current = nextHydratableInstance) ? (current = canHydrateSuspenseInstance(
                current,
                rootOrSingletonContext
              ), null !== current && (workInProgress2.memoizedState = {
                dehydrated: current,
                treeContext: null !== treeContextProvider ? { id: treeContextId, overflow: treeContextOverflow } : null,
                retryLane: 536870912,
                hydrationErrors: null
              }, renderLanes2 = createFiberFromDehydratedFragment(current), renderLanes2.return = workInProgress2, workInProgress2.child = renderLanes2, hydrationParentFiber = workInProgress2, nextHydratableInstance = null)) : current = null;
              if (null === current) throw throwOnHydrationMismatch(workInProgress2);
              isSuspenseInstanceFallback(current) ? workInProgress2.lanes = 32 : workInProgress2.lanes = 536870912;
              return null;
            }
            didSuspend = nextProps.children;
            nextProps = nextProps.fallback;
            if (showFallback)
              return reuseSuspenseHandlerOnStack(), showFallback = workInProgress2.mode, didSuspend = mountWorkInProgressOffscreenFiber(
                { mode: "hidden", children: didSuspend },
                showFallback
              ), nextProps = createFiberFromFragment(
                nextProps,
                showFallback,
                renderLanes2,
                null
              ), didSuspend.return = workInProgress2, nextProps.return = workInProgress2, didSuspend.sibling = nextProps, workInProgress2.child = didSuspend, nextProps = workInProgress2.child, nextProps.memoizedState = mountSuspenseOffscreenState(renderLanes2), nextProps.childLanes = getRemainingWorkInPrimaryTree(
                current,
                JSCompiler_temp,
                renderLanes2
              ), workInProgress2.memoizedState = SUSPENDED_MARKER, bailoutOffscreenComponent(null, nextProps);
            pushPrimaryTreeSuspenseHandler(workInProgress2);
            return mountSuspensePrimaryChildren(workInProgress2, didSuspend);
          }
          var prevState = current.memoizedState;
          if (null !== prevState) {
            var dehydrated$75 = prevState.dehydrated;
            if (null !== dehydrated$75)
              return updateDehydratedSuspenseComponent(
                current,
                workInProgress2,
                didSuspend,
                JSCompiler_temp,
                nextProps,
                dehydrated$75,
                prevState,
                renderLanes2
              );
          }
          if (showFallback)
            return reuseSuspenseHandlerOnStack(), showFallback = nextProps.fallback, didSuspend = workInProgress2.mode, prevState = current.child, dehydrated$75 = prevState.sibling, nextProps = createWorkInProgress(prevState, {
              mode: "hidden",
              children: nextProps.children
            }), nextProps.subtreeFlags = prevState.subtreeFlags & 1206910976, null !== dehydrated$75 ? showFallback = createWorkInProgress(dehydrated$75, showFallback) : (showFallback = createFiberFromFragment(
              showFallback,
              didSuspend,
              renderLanes2,
              null
            ), showFallback.flags |= 2), showFallback.return = workInProgress2, nextProps.return = workInProgress2, nextProps.sibling = showFallback, workInProgress2.child = nextProps, bailoutOffscreenComponent(null, nextProps), nextProps = workInProgress2.child, showFallback = current.child.memoizedState, null === showFallback ? showFallback = mountSuspenseOffscreenState(renderLanes2) : (didSuspend = showFallback.cachePool, null !== didSuspend ? (prevState = isPrimaryRenderer ? CacheContext._currentValue : CacheContext._currentValue2, didSuspend = didSuspend.parent !== prevState ? { parent: prevState, pool: prevState } : didSuspend) : didSuspend = getSuspendedCache(), showFallback = {
              baseLanes: showFallback.baseLanes | renderLanes2,
              cachePool: didSuspend
            }), nextProps.memoizedState = showFallback, nextProps.childLanes = getRemainingWorkInPrimaryTree(
              current,
              JSCompiler_temp,
              renderLanes2
            ), workInProgress2.memoizedState = SUSPENDED_MARKER, bailoutOffscreenComponent(current.child, nextProps);
          pushPrimaryTreeSuspenseHandler(workInProgress2);
          renderLanes2 = current.child;
          current = renderLanes2.sibling;
          renderLanes2 = createWorkInProgress(renderLanes2, {
            mode: "visible",
            children: nextProps.children
          });
          renderLanes2.return = workInProgress2;
          renderLanes2.sibling = null;
          null !== current && (JSCompiler_temp = workInProgress2.deletions, null === JSCompiler_temp ? (workInProgress2.deletions = [current], workInProgress2.flags |= 16) : JSCompiler_temp.push(current));
          workInProgress2.child = renderLanes2;
          workInProgress2.memoizedState = null;
          return renderLanes2;
        }
        function mountSuspensePrimaryChildren(workInProgress2, primaryChildren) {
          primaryChildren = mountWorkInProgressOffscreenFiber(
            { mode: "visible", children: primaryChildren },
            workInProgress2.mode
          );
          primaryChildren.return = workInProgress2;
          return workInProgress2.child = primaryChildren;
        }
        function mountWorkInProgressOffscreenFiber(offscreenProps, mode) {
          offscreenProps = createFiber(22, offscreenProps, null, mode);
          offscreenProps.lanes = 0;
          return offscreenProps;
        }
        function retrySuspenseComponentWithoutHydrating(current, workInProgress2, renderLanes2) {
          reconcileChildFibers(workInProgress2, current.child, null, renderLanes2);
          current = mountSuspensePrimaryChildren(
            workInProgress2,
            workInProgress2.pendingProps.children
          );
          current.flags |= 2;
          workInProgress2.memoizedState = null;
          return current;
        }
        function updateDehydratedSuspenseComponent(current, workInProgress2, didSuspend, didPrimaryChildrenDefer, nextProps, suspenseInstance, suspenseState, renderLanes2) {
          if (didSuspend) {
            if (workInProgress2.flags & 256)
              return pushPrimaryTreeSuspenseHandler(workInProgress2), workInProgress2.flags &= -257, retrySuspenseComponentWithoutHydrating(
                current,
                workInProgress2,
                renderLanes2
              );
            if (null !== workInProgress2.memoizedState)
              return reuseSuspenseHandlerOnStack(), workInProgress2.child = current.child, workInProgress2.flags |= 128, null;
            reuseSuspenseHandlerOnStack();
            suspenseInstance = nextProps.fallback;
            suspenseState = workInProgress2.mode;
            nextProps = mountWorkInProgressOffscreenFiber(
              { mode: "visible", children: nextProps.children },
              suspenseState
            );
            suspenseInstance = createFiberFromFragment(
              suspenseInstance,
              suspenseState,
              renderLanes2,
              null
            );
            suspenseInstance.flags |= 2;
            nextProps.return = workInProgress2;
            suspenseInstance.return = workInProgress2;
            nextProps.sibling = suspenseInstance;
            workInProgress2.child = nextProps;
            reconcileChildFibers(workInProgress2, current.child, null, renderLanes2);
            nextProps = workInProgress2.child;
            nextProps.memoizedState = mountSuspenseOffscreenState(renderLanes2);
            nextProps.childLanes = getRemainingWorkInPrimaryTree(
              current,
              didPrimaryChildrenDefer,
              renderLanes2
            );
            workInProgress2.memoizedState = SUSPENDED_MARKER;
            return bailoutOffscreenComponent(null, nextProps);
          }
          pushPrimaryTreeSuspenseHandler(workInProgress2);
          if (isSuspenseInstanceFallback(suspenseInstance))
            return didPrimaryChildrenDefer = getSuspenseInstanceFallbackErrorDetails(suspenseInstance).digest, "" !== didPrimaryChildrenDefer && (nextProps = Error(formatProdErrorMessage(419)), nextProps.stack = "", nextProps.digest = didPrimaryChildrenDefer, queueHydrationError({ value: nextProps, source: null, stack: null })), retrySuspenseComponentWithoutHydrating(
              current,
              workInProgress2,
              renderLanes2
            );
          didReceiveUpdate || propagateParentContextChanges(current, workInProgress2, renderLanes2, false);
          didPrimaryChildrenDefer = 0 !== (renderLanes2 & current.childLanes);
          if (didReceiveUpdate || didPrimaryChildrenDefer) {
            if (null !== currentTreeHiddenStackCursor.current)
              return retrySuspenseComponentWithoutHydrating(
                current,
                workInProgress2,
                renderLanes2
              );
            didPrimaryChildrenDefer = workInProgressRoot;
            if (null !== didPrimaryChildrenDefer && (nextProps = getBumpedLaneForHydration(
              didPrimaryChildrenDefer,
              renderLanes2
            ), 0 !== nextProps && nextProps !== suspenseState.retryLane))
              throw suspenseState.retryLane = nextProps, enqueueConcurrentRenderForLane(current, nextProps), scheduleUpdateOnFiber(didPrimaryChildrenDefer, current, nextProps), SelectiveHydrationException;
            isSuspenseInstancePending(suspenseInstance) || renderDidSuspendDelayIfPossible();
            return retrySuspenseComponentWithoutHydrating(
              current,
              workInProgress2,
              renderLanes2
            );
          }
          if (isSuspenseInstancePending(suspenseInstance))
            return workInProgress2.flags |= 192, workInProgress2.child = current.child, null;
          current = suspenseState.treeContext;
          supportsHydration && (nextHydratableInstance = getFirstHydratableChildWithinSuspenseInstance(suspenseInstance), hydrationParentFiber = workInProgress2, isHydrating = true, hydrationErrors = null, rootOrSingletonContext = false, null !== current && restoreSuspendedTreeContext(workInProgress2, current));
          workInProgress2 = mountSuspensePrimaryChildren(
            workInProgress2,
            nextProps.children
          );
          workInProgress2.flags |= 134221824;
          return workInProgress2;
        }
        function scheduleSuspenseWorkOnFiber(fiber, renderLanes2, propagationRoot) {
          fiber.lanes |= renderLanes2;
          var alternate = fiber.alternate;
          null !== alternate && (alternate.lanes |= renderLanes2);
          scheduleContextWorkOnParentPath(fiber.return, renderLanes2, propagationRoot);
        }
        function findLastContentRow(firstChild) {
          for (var lastContentRow = null; null !== firstChild; ) {
            var currentRow = firstChild.alternate;
            null !== currentRow && null === findFirstSuspended(currentRow) && (lastContentRow = firstChild);
            firstChild = firstChild.sibling;
          }
          return lastContentRow;
        }
        function initSuspenseListRenderState(workInProgress2, isBackwards, tail, lastContentRow, tailMode, treeForkCount2) {
          var renderState = workInProgress2.memoizedState;
          null === renderState ? workInProgress2.memoizedState = {
            isBackwards,
            rendering: null,
            renderingStartTime: 0,
            last: lastContentRow,
            tail,
            tailMode,
            treeForkCount: treeForkCount2
          } : (renderState.isBackwards = isBackwards, renderState.rendering = null, renderState.renderingStartTime = 0, renderState.last = lastContentRow, renderState.tail = tail, renderState.tailMode = tailMode, renderState.treeForkCount = treeForkCount2);
        }
        function reverseChildren(fiber) {
          var row = fiber.child;
          for (fiber.child = null; null !== row; ) {
            var nextRow = row.sibling;
            row.sibling = fiber.child;
            fiber.child = row;
            row = nextRow;
          }
        }
        function updateSuspenseListComponent(current, workInProgress2, renderLanes2) {
          var nextProps = workInProgress2.pendingProps, revealOrder = nextProps.revealOrder, tailMode = nextProps.tail;
          nextProps = nextProps.children;
          var suspenseContext = suspenseStackCursor.current;
          if (workInProgress2.flags & 128)
            return pushSuspenseListContext(workInProgress2, suspenseContext), null;
          var shouldForceFallback = 0 !== (suspenseContext & 2);
          shouldForceFallback ? (suspenseContext = suspenseContext & 1 | 2, workInProgress2.flags |= 128) : suspenseContext &= 1;
          pushSuspenseListContext(workInProgress2, suspenseContext);
          "backwards" === revealOrder && null !== current ? (reverseChildren(current), reconcileChildren(current, workInProgress2, nextProps, renderLanes2), reverseChildren(current)) : reconcileChildren(current, workInProgress2, nextProps, renderLanes2);
          nextProps = isHydrating ? treeForkCount : 0;
          if (!shouldForceFallback && null !== current && 0 !== (current.flags & 128))
            a: for (current = workInProgress2.child; null !== current; ) {
              if (13 === current.tag)
                null !== current.memoizedState && scheduleSuspenseWorkOnFiber(current, renderLanes2, workInProgress2);
              else if (19 === current.tag)
                scheduleSuspenseWorkOnFiber(current, renderLanes2, workInProgress2);
              else if (null !== current.child) {
                current.child.return = current;
                current = current.child;
                continue;
              }
              if (current === workInProgress2) break a;
              for (; null === current.sibling; ) {
                if (null === current.return || current.return === workInProgress2)
                  break a;
                current = current.return;
              }
              current.sibling.return = current.return;
              current = current.sibling;
            }
          switch (revealOrder) {
            case "backwards":
              renderLanes2 = findLastContentRow(workInProgress2.child);
              null === renderLanes2 ? (revealOrder = workInProgress2.child, workInProgress2.child = null) : (revealOrder = renderLanes2.sibling, renderLanes2.sibling = null, reverseChildren(workInProgress2));
              initSuspenseListRenderState(
                workInProgress2,
                true,
                revealOrder,
                null,
                tailMode,
                nextProps
              );
              break;
            case "unstable_legacy-backwards":
              renderLanes2 = null;
              revealOrder = workInProgress2.child;
              for (workInProgress2.child = null; null !== revealOrder; ) {
                current = revealOrder.alternate;
                if (null !== current && null === findFirstSuspended(current)) {
                  workInProgress2.child = revealOrder;
                  break;
                }
                current = revealOrder.sibling;
                revealOrder.sibling = renderLanes2;
                renderLanes2 = revealOrder;
                revealOrder = current;
              }
              initSuspenseListRenderState(
                workInProgress2,
                true,
                renderLanes2,
                null,
                tailMode,
                nextProps
              );
              break;
            case "together":
              initSuspenseListRenderState(
                workInProgress2,
                false,
                null,
                null,
                void 0,
                nextProps
              );
              break;
            case "independent":
              workInProgress2.memoizedState = null;
              break;
            default:
              renderLanes2 = findLastContentRow(workInProgress2.child), null === renderLanes2 ? (revealOrder = workInProgress2.child, workInProgress2.child = null) : (revealOrder = renderLanes2.sibling, renderLanes2.sibling = null), initSuspenseListRenderState(
                workInProgress2,
                false,
                revealOrder,
                renderLanes2,
                tailMode,
                nextProps
              );
          }
          return workInProgress2.child;
        }
        function updateContextProvider(current, workInProgress2, renderLanes2) {
          var newProps = workInProgress2.pendingProps;
          pushProvider(workInProgress2, workInProgress2.type, newProps.value);
          reconcileChildren(current, workInProgress2, newProps.children, renderLanes2);
          return workInProgress2.child;
        }
        function bailoutOnAlreadyFinishedWork(current, workInProgress2, renderLanes2) {
          null !== current && (workInProgress2.dependencies = current.dependencies);
          workInProgressRootSkippedLanes |= workInProgress2.lanes;
          if (0 === (renderLanes2 & workInProgress2.childLanes))
            if (null !== current) {
              if (propagateParentContextChanges(
                current,
                workInProgress2,
                renderLanes2,
                false
              ), 0 === (renderLanes2 & workInProgress2.childLanes))
                return null;
            } else return null;
          if (null !== current && workInProgress2.child !== current.child)
            throw Error(formatProdErrorMessage(153));
          if (null !== workInProgress2.child) {
            current = workInProgress2.child;
            renderLanes2 = createWorkInProgress(current, current.pendingProps);
            workInProgress2.child = renderLanes2;
            for (renderLanes2.return = workInProgress2; null !== current.sibling; )
              current = current.sibling, renderLanes2 = renderLanes2.sibling = createWorkInProgress(current, current.pendingProps), renderLanes2.return = workInProgress2;
            renderLanes2.sibling = null;
          }
          return workInProgress2.child;
        }
        function checkScheduledUpdateOrContext(current, renderLanes2) {
          if (0 !== (current.lanes & renderLanes2)) return true;
          current = current.dependencies;
          return null !== current && checkIfContextChanged(current) ? true : false;
        }
        function attemptEarlyBailoutIfNoScheduledUpdate(current, workInProgress2, renderLanes2) {
          switch (workInProgress2.tag) {
            case 3:
              pushHostContainer(
                workInProgress2,
                workInProgress2.stateNode.containerInfo
              );
              pushProvider(workInProgress2, CacheContext, current.memoizedState.cache);
              resetHydrationState();
              break;
            case 27:
            case 5:
              pushHostContext(workInProgress2);
              break;
            case 4:
              pushHostContainer(
                workInProgress2,
                workInProgress2.stateNode.containerInfo
              );
              break;
            case 10:
              pushProvider(
                workInProgress2,
                workInProgress2.type,
                workInProgress2.memoizedProps.value
              );
              break;
            case 31:
              if (null !== workInProgress2.memoizedState)
                return workInProgress2.flags |= 128, pushDehydratedActivitySuspenseHandler(workInProgress2), null;
              break;
            case 13:
              var state$86 = workInProgress2.memoizedState;
              if (null !== state$86) {
                if (null !== state$86.dehydrated)
                  return pushPrimaryTreeSuspenseHandler(workInProgress2), workInProgress2.flags |= 128, null;
                state$86 = propagateParentContextChanges(
                  current,
                  workInProgress2,
                  renderLanes2,
                  false
                );
                var primaryChildLanes = workInProgress2.child.childLanes;
                if (state$86 || 0 !== (renderLanes2 & primaryChildLanes))
                  return updateSuspenseComponent(
                    current,
                    workInProgress2,
                    renderLanes2
                  );
                pushPrimaryTreeSuspenseHandler(workInProgress2);
                current = bailoutOnAlreadyFinishedWork(
                  current,
                  workInProgress2,
                  renderLanes2
                );
                return null !== current ? current.sibling : null;
              }
              pushPrimaryTreeSuspenseHandler(workInProgress2);
              break;
            case 19:
              if (workInProgress2.flags & 128)
                return updateSuspenseListComponent(
                  current,
                  workInProgress2,
                  renderLanes2
                );
              primaryChildLanes = 0 !== (current.flags & 128);
              state$86 = 0 !== (renderLanes2 & workInProgress2.childLanes);
              state$86 || (propagateParentContextChanges(
                current,
                workInProgress2,
                renderLanes2,
                false
              ), state$86 = 0 !== (renderLanes2 & workInProgress2.childLanes));
              if (primaryChildLanes) {
                if (state$86)
                  return updateSuspenseListComponent(
                    current,
                    workInProgress2,
                    renderLanes2
                  );
                workInProgress2.flags |= 128;
              }
              primaryChildLanes = workInProgress2.memoizedState;
              null !== primaryChildLanes && (primaryChildLanes.rendering = null, primaryChildLanes.tail = null, primaryChildLanes.lastEffect = null);
              pushSuspenseListContext(workInProgress2, suspenseStackCursor.current);
              if (state$86) break;
              else return null;
            case 22:
              return workInProgress2.lanes = 0, updateOffscreenComponent(
                current,
                workInProgress2,
                renderLanes2,
                workInProgress2.pendingProps
              );
            case 24:
              pushProvider(workInProgress2, CacheContext, current.memoizedState.cache);
          }
          return bailoutOnAlreadyFinishedWork(current, workInProgress2, renderLanes2);
        }
        function beginWork(current, workInProgress2, renderLanes2) {
          if (null !== current)
            if (current.memoizedProps !== workInProgress2.pendingProps)
              didReceiveUpdate = true;
            else {
              if (!checkScheduledUpdateOrContext(current, renderLanes2) && 0 === (workInProgress2.flags & 128))
                return didReceiveUpdate = false, attemptEarlyBailoutIfNoScheduledUpdate(
                  current,
                  workInProgress2,
                  renderLanes2
                );
              didReceiveUpdate = 0 !== (current.flags & 131072) ? true : false;
            }
          else
            didReceiveUpdate = false, isHydrating && 0 !== (workInProgress2.flags & 1048576) && pushTreeId(workInProgress2, treeForkCount, workInProgress2.index);
          workInProgress2.lanes = 0;
          switch (workInProgress2.tag) {
            case 16:
              a: {
                var props = workInProgress2.pendingProps;
                current = resolveLazy(workInProgress2.elementType);
                workInProgress2.type = current;
                if ("function" === typeof current)
                  shouldConstruct(current) ? (props = resolveClassComponentProps(current, props), workInProgress2.tag = 1, workInProgress2 = updateClassComponent(
                    null,
                    workInProgress2,
                    current,
                    props,
                    renderLanes2
                  )) : (workInProgress2.tag = 0, workInProgress2 = updateFunctionComponent(
                    null,
                    workInProgress2,
                    current,
                    props,
                    renderLanes2
                  ));
                else {
                  if (void 0 !== current && null !== current) {
                    var $$typeof = current.$$typeof;
                    if ($$typeof === REACT_FORWARD_REF_TYPE) {
                      workInProgress2.tag = 11;
                      workInProgress2 = updateForwardRef(
                        null,
                        workInProgress2,
                        current,
                        props,
                        renderLanes2
                      );
                      break a;
                    } else if ($$typeof === REACT_MEMO_TYPE) {
                      workInProgress2.tag = 14;
                      workInProgress2 = updateMemoComponent(
                        null,
                        workInProgress2,
                        current,
                        props,
                        renderLanes2
                      );
                      break a;
                    } else if ($$typeof === REACT_CONTEXT_TYPE) {
                      workInProgress2.tag = 10;
                      workInProgress2.type = current;
                      workInProgress2 = updateContextProvider(
                        null,
                        workInProgress2,
                        renderLanes2
                      );
                      break a;
                    }
                  }
                  workInProgress2 = getComponentNameFromType(current) || current;
                  throw Error(formatProdErrorMessage(306, workInProgress2, ""));
                }
              }
              return workInProgress2;
            case 0:
              return updateFunctionComponent(
                current,
                workInProgress2,
                workInProgress2.type,
                workInProgress2.pendingProps,
                renderLanes2
              );
            case 1:
              return props = workInProgress2.type, $$typeof = resolveClassComponentProps(
                props,
                workInProgress2.pendingProps
              ), updateClassComponent(
                current,
                workInProgress2,
                props,
                $$typeof,
                renderLanes2
              );
            case 3:
              a: {
                pushHostContainer(
                  workInProgress2,
                  workInProgress2.stateNode.containerInfo
                );
                if (null === current) throw Error(formatProdErrorMessage(387));
                var nextProps = workInProgress2.pendingProps;
                $$typeof = workInProgress2.memoizedState;
                props = $$typeof.element;
                cloneUpdateQueue(current, workInProgress2);
                processUpdateQueue(workInProgress2, nextProps, null, renderLanes2);
                var nextState = workInProgress2.memoizedState;
                nextProps = nextState.cache;
                pushProvider(workInProgress2, CacheContext, nextProps);
                nextProps !== $$typeof.cache && propagateContextChanges(
                  workInProgress2,
                  [CacheContext],
                  renderLanes2,
                  true
                );
                suspendIfUpdateReadFromEntangledAsyncAction();
                nextProps = nextState.element;
                if (supportsHydration && $$typeof.isDehydrated)
                  if ($$typeof = {
                    element: nextProps,
                    isDehydrated: false,
                    cache: nextState.cache
                  }, workInProgress2.updateQueue.baseState = $$typeof, workInProgress2.memoizedState = $$typeof, workInProgress2.flags & 256) {
                    workInProgress2 = mountHostRootWithoutHydrating(
                      current,
                      workInProgress2,
                      nextProps,
                      renderLanes2
                    );
                    break a;
                  } else if (nextProps !== props) {
                    props = createCapturedValueAtFiber(
                      Error(formatProdErrorMessage(424)),
                      workInProgress2
                    );
                    queueHydrationError(props);
                    workInProgress2 = mountHostRootWithoutHydrating(
                      current,
                      workInProgress2,
                      nextProps,
                      renderLanes2
                    );
                    break a;
                  } else
                    for (supportsHydration && (nextHydratableInstance = getFirstHydratableChildWithinContainer(
                      workInProgress2.stateNode.containerInfo
                    ), hydrationParentFiber = workInProgress2, isHydrating = true, hydrationErrors = null, rootOrSingletonContext = true), renderLanes2 = mountChildFibers(
                      workInProgress2,
                      null,
                      nextProps,
                      renderLanes2
                    ), workInProgress2.child = renderLanes2; renderLanes2; )
                      renderLanes2.flags = renderLanes2.flags & -3 | 134221824, renderLanes2 = renderLanes2.sibling;
                else {
                  resetHydrationState();
                  if (nextProps === props) {
                    workInProgress2 = bailoutOnAlreadyFinishedWork(
                      current,
                      workInProgress2,
                      renderLanes2
                    );
                    break a;
                  }
                  reconcileChildren(current, workInProgress2, nextProps, renderLanes2);
                }
                workInProgress2 = workInProgress2.child;
              }
              return workInProgress2;
            case 26:
              if (supportsResources)
                return markRef(current, workInProgress2), null === current ? (renderLanes2 = getResource(
                  workInProgress2.type,
                  null,
                  workInProgress2.pendingProps,
                  null
                )) ? workInProgress2.memoizedState = renderLanes2 : isHydrating || (workInProgress2.stateNode = createHoistableInstance(
                  workInProgress2.type,
                  workInProgress2.pendingProps,
                  rootInstanceStackCursor.current,
                  workInProgress2
                )) : workInProgress2.memoizedState = getResource(
                  workInProgress2.type,
                  current.memoizedProps,
                  workInProgress2.pendingProps,
                  current.memoizedState
                ), null;
            case 27:
              if (supportsSingletons)
                return pushHostContext(workInProgress2), null === current && supportsSingletons && isHydrating && (props = workInProgress2.stateNode = resolveSingletonInstance(
                  workInProgress2.type,
                  workInProgress2.pendingProps,
                  rootInstanceStackCursor.current,
                  contextStackCursor.current,
                  false
                ), hydrationParentFiber = workInProgress2, rootOrSingletonContext = true, nextHydratableInstance = getFirstHydratableChildWithinSingleton(
                  workInProgress2.type,
                  props,
                  nextHydratableInstance
                )), reconcileChildren(
                  current,
                  workInProgress2,
                  workInProgress2.pendingProps.children,
                  renderLanes2
                ), markRef(current, workInProgress2), null === current && (workInProgress2.flags |= 4194304), workInProgress2.child;
            case 5:
              if (null === current && isHydrating) {
                validateHydratableInstance(
                  workInProgress2.type,
                  workInProgress2.pendingProps,
                  contextStackCursor.current
                );
                if ($$typeof = props = nextHydratableInstance)
                  props = canHydrateInstance(
                    props,
                    workInProgress2.type,
                    workInProgress2.pendingProps,
                    rootOrSingletonContext
                  ), null !== props ? (workInProgress2.stateNode = props, hydrationParentFiber = workInProgress2, nextHydratableInstance = getFirstHydratableChild(props), rootOrSingletonContext = false, $$typeof = true) : $$typeof = false;
                $$typeof || throwOnHydrationMismatch(workInProgress2);
              }
              pushHostContext(workInProgress2);
              $$typeof = workInProgress2.type;
              nextProps = workInProgress2.pendingProps;
              nextState = null !== current ? current.memoizedProps : null;
              props = nextProps.children;
              shouldSetTextContent($$typeof, nextProps) ? props = null : null !== nextState && shouldSetTextContent($$typeof, nextState) && (workInProgress2.flags |= 32);
              null !== workInProgress2.memoizedState && ($$typeof = renderWithHooks(
                current,
                workInProgress2,
                TransitionAwareHostComponent,
                null,
                null,
                renderLanes2
              ), isPrimaryRenderer ? HostTransitionContext._currentValue = $$typeof : HostTransitionContext._currentValue2 = $$typeof);
              markRef(current, workInProgress2);
              reconcileChildren(current, workInProgress2, props, renderLanes2);
              return workInProgress2.child;
            case 6:
              if (null === current && isHydrating) {
                validateHydratableTextInstance(
                  workInProgress2.pendingProps,
                  contextStackCursor.current
                );
                if (current = renderLanes2 = nextHydratableInstance)
                  renderLanes2 = canHydrateTextInstance(
                    renderLanes2,
                    workInProgress2.pendingProps,
                    rootOrSingletonContext
                  ), null !== renderLanes2 ? (workInProgress2.stateNode = renderLanes2, hydrationParentFiber = workInProgress2, nextHydratableInstance = null, current = true) : current = false;
                current || throwOnHydrationMismatch(workInProgress2);
              }
              return null;
            case 13:
              return updateSuspenseComponent(current, workInProgress2, renderLanes2);
            case 4:
              return pushHostContainer(
                workInProgress2,
                workInProgress2.stateNode.containerInfo
              ), props = workInProgress2.pendingProps, null === current ? workInProgress2.child = reconcileChildFibers(
                workInProgress2,
                null,
                props,
                renderLanes2
              ) : reconcileChildren(current, workInProgress2, props, renderLanes2), workInProgress2.child;
            case 11:
              return updateForwardRef(
                current,
                workInProgress2,
                workInProgress2.type,
                workInProgress2.pendingProps,
                renderLanes2
              );
            case 7:
              return props = workInProgress2.pendingProps, markRef(current, workInProgress2), reconcileChildren(current, workInProgress2, props, renderLanes2), workInProgress2.child;
            case 8:
              return reconcileChildren(
                current,
                workInProgress2,
                workInProgress2.pendingProps.children,
                renderLanes2
              ), workInProgress2.child;
            case 12:
              return reconcileChildren(
                current,
                workInProgress2,
                workInProgress2.pendingProps.children,
                renderLanes2
              ), workInProgress2.child;
            case 10:
              return updateContextProvider(current, workInProgress2, renderLanes2);
            case 9:
              return $$typeof = workInProgress2.type._context, props = workInProgress2.pendingProps.children, prepareToReadContext(workInProgress2), $$typeof = readContext($$typeof), props = props($$typeof), workInProgress2.flags |= 1, reconcileChildren(current, workInProgress2, props, renderLanes2), workInProgress2.child;
            case 14:
              return updateMemoComponent(
                current,
                workInProgress2,
                workInProgress2.type,
                workInProgress2.pendingProps,
                renderLanes2
              );
            case 15:
              return updateSimpleMemoComponent(
                current,
                workInProgress2,
                workInProgress2.type,
                workInProgress2.pendingProps,
                renderLanes2
              );
            case 19:
              return updateSuspenseListComponent(
                current,
                workInProgress2,
                renderLanes2
              );
            case 31:
              return updateActivityComponent(current, workInProgress2, renderLanes2);
            case 22:
              return updateOffscreenComponent(
                current,
                workInProgress2,
                renderLanes2,
                workInProgress2.pendingProps
              );
            case 24:
              return prepareToReadContext(workInProgress2), props = readContext(CacheContext), null === current ? ($$typeof = peekCacheFromPool(), null === $$typeof && ($$typeof = workInProgressRoot, nextProps = createCache(), $$typeof.pooledCache = nextProps, nextProps.refCount++, null !== nextProps && ($$typeof.pooledCacheLanes |= renderLanes2), $$typeof = nextProps), workInProgress2.memoizedState = {
                parent: props,
                cache: $$typeof
              }, initializeUpdateQueue(workInProgress2), pushProvider(workInProgress2, CacheContext, $$typeof)) : (0 !== (current.lanes & renderLanes2) && (cloneUpdateQueue(current, workInProgress2), processUpdateQueue(workInProgress2, null, null, renderLanes2), suspendIfUpdateReadFromEntangledAsyncAction()), $$typeof = current.memoizedState, nextProps = workInProgress2.memoizedState, $$typeof.parent !== props ? ($$typeof = { parent: props, cache: props }, workInProgress2.memoizedState = $$typeof, 0 === workInProgress2.lanes && (workInProgress2.memoizedState = workInProgress2.updateQueue.baseState = $$typeof), pushProvider(workInProgress2, CacheContext, props)) : (props = nextProps.cache, pushProvider(workInProgress2, CacheContext, props), props !== $$typeof.cache && propagateContextChanges(
                workInProgress2,
                [CacheContext],
                renderLanes2,
                true
              ))), reconcileChildren(
                current,
                workInProgress2,
                workInProgress2.pendingProps.children,
                renderLanes2
              ), workInProgress2.child;
            case 30:
              return null === workInProgress2.stateNode && (workInProgress2.stateNode = {
                autoName: null,
                paired: null,
                clones: null,
                ref: null
              }), props = workInProgress2.pendingProps, null != props.name && "auto" !== props.name ? workInProgress2.flags |= null === current ? 18882560 : 18874368 : isHydrating && pushMaterializedTreeId(workInProgress2), null !== current && current.memoizedProps.name !== props.name ? workInProgress2.flags |= 4194816 : markRef(current, workInProgress2), reconcileChildren(
                current,
                workInProgress2,
                props.children,
                renderLanes2
              ), workInProgress2.child;
            case 29:
              throw workInProgress2.pendingProps;
          }
          throw Error(formatProdErrorMessage(156, workInProgress2.tag));
        }
        function markUpdate(workInProgress2) {
          workInProgress2.flags |= 4;
        }
        function markCloned(workInProgress2) {
          supportsPersistence && (workInProgress2.flags |= 8);
        }
        function doesRequireClone(current, completedWork) {
          if (null !== current && current.child === completedWork.child) return false;
          if (0 !== (completedWork.flags & 16)) return true;
          for (current = completedWork.child; null !== current; ) {
            if (0 !== (current.flags & 8218) || 0 !== (current.subtreeFlags & 8218))
              return true;
            current = current.sibling;
          }
          return false;
        }
        function appendAllChildren(parent, workInProgress2, needsVisibilityToggle, isHidden) {
          if (supportsMutation)
            for (needsVisibilityToggle = workInProgress2.child; null !== needsVisibilityToggle; ) {
              if (5 === needsVisibilityToggle.tag || 6 === needsVisibilityToggle.tag)
                appendInitialChild(parent, needsVisibilityToggle.stateNode);
              else if (!(4 === needsVisibilityToggle.tag || supportsSingletons && 27 === needsVisibilityToggle.tag) && null !== needsVisibilityToggle.child) {
                needsVisibilityToggle.child.return = needsVisibilityToggle;
                needsVisibilityToggle = needsVisibilityToggle.child;
                continue;
              }
              if (needsVisibilityToggle === workInProgress2) break;
              for (; null === needsVisibilityToggle.sibling; ) {
                if (null === needsVisibilityToggle.return || needsVisibilityToggle.return === workInProgress2)
                  return;
                needsVisibilityToggle = needsVisibilityToggle.return;
              }
              needsVisibilityToggle.sibling.return = needsVisibilityToggle.return;
              needsVisibilityToggle = needsVisibilityToggle.sibling;
            }
          else if (supportsPersistence)
            for (var node$89 = workInProgress2.child; null !== node$89; ) {
              if (5 === node$89.tag) {
                var instance = node$89.stateNode;
                needsVisibilityToggle && isHidden && (instance = cloneHiddenInstance(
                  instance,
                  node$89.type,
                  node$89.memoizedProps
                ));
                appendInitialChild(parent, instance);
              } else if (6 === node$89.tag)
                instance = node$89.stateNode, needsVisibilityToggle && isHidden && (instance = cloneHiddenTextInstance(
                  instance,
                  node$89.memoizedProps
                )), appendInitialChild(parent, instance);
              else if (4 !== node$89.tag) {
                if (22 === node$89.tag && null !== node$89.memoizedState)
                  instance = node$89.child, null !== instance && (instance.return = node$89), appendAllChildren(parent, node$89, true, true);
                else if (null !== node$89.child) {
                  node$89.child.return = node$89;
                  node$89 = node$89.child;
                  continue;
                }
              }
              if (node$89 === workInProgress2) break;
              for (; null === node$89.sibling; ) {
                if (null === node$89.return || node$89.return === workInProgress2)
                  return;
                node$89 = node$89.return;
              }
              node$89.sibling.return = node$89.return;
              node$89 = node$89.sibling;
            }
        }
        function appendAllChildrenToContainer(containerChildSet, workInProgress2, needsVisibilityToggle, isHidden) {
          var hasOffscreenComponentChild = false;
          if (supportsPersistence)
            for (var node = workInProgress2.child; null !== node; ) {
              if (5 === node.tag) {
                var instance = node.stateNode;
                needsVisibilityToggle && isHidden && (instance = cloneHiddenInstance(
                  instance,
                  node.type,
                  node.memoizedProps
                ));
                appendChildToContainerChildSet(containerChildSet, instance);
              } else if (6 === node.tag)
                instance = node.stateNode, needsVisibilityToggle && isHidden && (instance = cloneHiddenTextInstance(
                  instance,
                  node.memoizedProps
                )), appendChildToContainerChildSet(containerChildSet, instance);
              else if (4 !== node.tag) {
                if (22 === node.tag && null !== node.memoizedState)
                  hasOffscreenComponentChild = node.child, null !== hasOffscreenComponentChild && (hasOffscreenComponentChild.return = node), appendAllChildrenToContainer(containerChildSet, node, true, true), hasOffscreenComponentChild = true;
                else if (null !== node.child) {
                  node.child.return = node;
                  node = node.child;
                  continue;
                }
              }
              if (node === workInProgress2) break;
              for (; null === node.sibling; ) {
                if (null === node.return || node.return === workInProgress2)
                  return hasOffscreenComponentChild;
                node = node.return;
              }
              node.sibling.return = node.return;
              node = node.sibling;
            }
          return hasOffscreenComponentChild;
        }
        function updateHostContainer(current, workInProgress2) {
          if (supportsPersistence && doesRequireClone(current, workInProgress2)) {
            current = workInProgress2.stateNode;
            var container2 = current.containerInfo, newChildSet = createContainerChildSet();
            appendAllChildrenToContainer(newChildSet, workInProgress2, false, false);
            current.pendingChildren = newChildSet;
            markUpdate(workInProgress2);
            finalizeContainerChildren(container2, newChildSet);
          }
        }
        function updateHostComponent(current, workInProgress2, type, newProps) {
          if (supportsMutation)
            current.memoizedProps !== newProps && markUpdate(workInProgress2);
          else if (supportsPersistence) {
            var currentInstance = current.stateNode, oldProps$92 = current.memoizedProps;
            if ((current = doesRequireClone(current, workInProgress2)) || oldProps$92 !== newProps) {
              var currentHostContext = contextStackCursor.current;
              oldProps$92 = cloneInstance(
                currentInstance,
                type,
                oldProps$92,
                newProps,
                !current,
                null
              );
              oldProps$92 === currentInstance ? workInProgress2.stateNode = currentInstance : (markCloned(workInProgress2), finalizeInitialChildren(
                oldProps$92,
                type,
                newProps,
                currentHostContext
              ) && markUpdate(workInProgress2), workInProgress2.stateNode = oldProps$92, current && appendAllChildren(oldProps$92, workInProgress2, false, false));
            } else workInProgress2.stateNode = currentInstance;
          }
        }
        function preloadInstanceAndSuspendIfNeeded(workInProgress2, type, oldProps, newProps, renderLanes2) {
          if (0 !== (workInProgress2.mode & 32) && (null === oldProps ? maySuspendCommit(type, newProps) : maySuspendCommitOnUpdate(type, oldProps, newProps))) {
            if (workInProgress2.flags |= 16777216, (renderLanes2 & 335544128) === renderLanes2 || maySuspendCommitInSyncRender(type, newProps))
              if (preloadInstance(workInProgress2.stateNode, type, newProps))
                workInProgress2.flags |= 8192;
              else if (shouldRemainOnPreviousScreen()) workInProgress2.flags |= 8192;
              else
                throw suspendedThenable = noopSuspenseyCommitThenable, SuspenseyCommitException;
          } else workInProgress2.flags &= -16777217;
        }
        function preloadResourceAndSuspendIfNeeded(workInProgress2, resource) {
          if (mayResourceSuspendCommit(resource)) {
            if (workInProgress2.flags |= 16777216, !preloadResource(resource))
              if (shouldRemainOnPreviousScreen()) workInProgress2.flags |= 8192;
              else
                throw suspendedThenable = noopSuspenseyCommitThenable, SuspenseyCommitException;
          } else workInProgress2.flags &= -16777217;
        }
        function scheduleRetryEffect(workInProgress2, retryQueue) {
          null !== retryQueue && (workInProgress2.flags |= 4);
          workInProgress2.flags & 16384 && (retryQueue = 22 !== workInProgress2.tag ? claimNextRetryLane() : 536870912, workInProgress2.lanes |= retryQueue, workInProgressSuspendedRetryLanes |= retryQueue);
        }
        function cutOffTailIfNeeded(renderState, hasRenderedATailFallback) {
          if (!isHydrating)
            switch (renderState.tailMode) {
              case "visible":
                break;
              case "collapsed":
                for (var tailNode = renderState.tail, lastTailNode = null; null !== tailNode; )
                  null !== tailNode.alternate && (lastTailNode = tailNode), tailNode = tailNode.sibling;
                null === lastTailNode ? hasRenderedATailFallback || null === renderState.tail ? renderState.tail = null : renderState.tail.sibling = null : lastTailNode.sibling = null;
                break;
              default:
                hasRenderedATailFallback = renderState.tail;
                for (tailNode = null; null !== hasRenderedATailFallback; )
                  null !== hasRenderedATailFallback.alternate && (tailNode = hasRenderedATailFallback), hasRenderedATailFallback = hasRenderedATailFallback.sibling;
                null === tailNode ? renderState.tail = null : tailNode.sibling = null;
            }
        }
        function bubbleProperties(completedWork) {
          var didBailout = null !== completedWork.alternate && completedWork.alternate.child === completedWork.child, newChildLanes = 0, subtreeFlags = 0;
          if (didBailout)
            for (var child$95 = completedWork.child; null !== child$95; )
              newChildLanes |= child$95.lanes | child$95.childLanes, subtreeFlags |= child$95.subtreeFlags & 1206910976, subtreeFlags |= child$95.flags & 1206910976, child$95.return = completedWork, child$95 = child$95.sibling;
          else
            for (child$95 = completedWork.child; null !== child$95; )
              newChildLanes |= child$95.lanes | child$95.childLanes, subtreeFlags |= child$95.subtreeFlags, subtreeFlags |= child$95.flags, child$95.return = completedWork, child$95 = child$95.sibling;
          completedWork.subtreeFlags |= subtreeFlags;
          completedWork.childLanes = newChildLanes;
          return didBailout;
        }
        function completeWork(current, workInProgress2, renderLanes2) {
          var newProps = workInProgress2.pendingProps;
          popTreeContext(workInProgress2);
          switch (workInProgress2.tag) {
            case 16:
            case 15:
            case 0:
            case 11:
            case 7:
            case 8:
            case 12:
            case 9:
            case 14:
              return bubbleProperties(workInProgress2), null;
            case 1:
              return bubbleProperties(workInProgress2), null;
            case 3:
              renderLanes2 = workInProgress2.stateNode;
              newProps = null;
              null !== current && (newProps = current.memoizedState.cache);
              workInProgress2.memoizedState.cache !== newProps && (workInProgress2.flags |= 2048);
              popProvider(CacheContext);
              popHostContainer();
              renderLanes2.pendingContext && (renderLanes2.context = renderLanes2.pendingContext, renderLanes2.pendingContext = null);
              if (null === current || null === current.child)
                popHydrationState(workInProgress2) ? markUpdate(workInProgress2) : null === current || current.memoizedState.isDehydrated && 0 === (workInProgress2.flags & 256) || (workInProgress2.flags |= 1024, upgradeHydrationErrorsToRecoverable());
              updateHostContainer(current, workInProgress2);
              bubbleProperties(workInProgress2);
              return null;
            case 26:
              if (supportsResources) {
                var type = workInProgress2.type, nextResource = workInProgress2.memoizedState;
                null === current ? (markUpdate(workInProgress2), null !== nextResource ? (bubbleProperties(workInProgress2), preloadResourceAndSuspendIfNeeded(
                  workInProgress2,
                  nextResource
                )) : (bubbleProperties(workInProgress2), preloadInstanceAndSuspendIfNeeded(
                  workInProgress2,
                  type,
                  null,
                  newProps,
                  renderLanes2
                ))) : nextResource ? nextResource !== current.memoizedState ? (markUpdate(workInProgress2), bubbleProperties(workInProgress2), preloadResourceAndSuspendIfNeeded(
                  workInProgress2,
                  nextResource
                )) : (bubbleProperties(workInProgress2), workInProgress2.flags &= -16777217) : (nextResource = current.memoizedProps, supportsMutation ? nextResource !== newProps && markUpdate(workInProgress2) : updateHostComponent(
                  current,
                  workInProgress2,
                  type,
                  newProps
                ), bubbleProperties(workInProgress2), preloadInstanceAndSuspendIfNeeded(
                  workInProgress2,
                  type,
                  nextResource,
                  newProps,
                  renderLanes2
                ));
                return null;
              }
            case 27:
              if (supportsSingletons) {
                popHostContext(workInProgress2);
                renderLanes2 = rootInstanceStackCursor.current;
                type = workInProgress2.type;
                if (null !== current && null != workInProgress2.stateNode)
                  supportsMutation ? current.memoizedProps !== newProps && markUpdate(workInProgress2) : updateHostComponent(current, workInProgress2, type, newProps);
                else {
                  if (!newProps) {
                    if (null === workInProgress2.stateNode)
                      throw Error(formatProdErrorMessage(166));
                    bubbleProperties(workInProgress2);
                    workInProgress2.subtreeFlags &= -33554433;
                    return null;
                  }
                  current = contextStackCursor.current;
                  popHydrationState(workInProgress2) ? prepareToHydrateHostInstance(workInProgress2, current) : (current = resolveSingletonInstance(
                    type,
                    newProps,
                    renderLanes2,
                    current,
                    true
                  ), workInProgress2.stateNode = current, markUpdate(workInProgress2));
                }
                bubbleProperties(workInProgress2);
                workInProgress2.subtreeFlags &= -33554433;
                return null;
              }
            case 5:
              popHostContext(workInProgress2);
              type = workInProgress2.type;
              if (null !== current && null != workInProgress2.stateNode)
                updateHostComponent(current, workInProgress2, type, newProps);
              else {
                if (!newProps) {
                  if (null === workInProgress2.stateNode)
                    throw Error(formatProdErrorMessage(166));
                  bubbleProperties(workInProgress2);
                  workInProgress2.subtreeFlags &= -33554433;
                  return null;
                }
                nextResource = contextStackCursor.current;
                if (popHydrationState(workInProgress2))
                  prepareToHydrateHostInstance(workInProgress2, nextResource), finalizeHydratedChildren(
                    workInProgress2.stateNode,
                    type,
                    newProps,
                    nextResource
                  ) && (workInProgress2.flags |= 64);
                else {
                  var instance$105 = createInstance(
                    type,
                    newProps,
                    rootInstanceStackCursor.current,
                    nextResource,
                    workInProgress2
                  );
                  markCloned(workInProgress2);
                  appendAllChildren(instance$105, workInProgress2, false, false);
                  workInProgress2.stateNode = instance$105;
                  finalizeInitialChildren(
                    instance$105,
                    type,
                    newProps,
                    nextResource
                  ) && markUpdate(workInProgress2);
                }
              }
              bubbleProperties(workInProgress2);
              workInProgress2.subtreeFlags &= -33554433;
              preloadInstanceAndSuspendIfNeeded(
                workInProgress2,
                workInProgress2.type,
                null === current ? null : current.memoizedProps,
                workInProgress2.pendingProps,
                renderLanes2
              );
              return null;
            case 6:
              if (current && null != workInProgress2.stateNode)
                renderLanes2 = current.memoizedProps, supportsMutation ? renderLanes2 !== newProps && markUpdate(workInProgress2) : supportsPersistence && (renderLanes2 !== newProps ? (current = rootInstanceStackCursor.current, renderLanes2 = contextStackCursor.current, markCloned(workInProgress2), workInProgress2.stateNode = createTextInstance(
                  newProps,
                  current,
                  renderLanes2,
                  workInProgress2
                )) : workInProgress2.stateNode = current.stateNode);
              else {
                if ("string" !== typeof newProps && null === workInProgress2.stateNode)
                  throw Error(formatProdErrorMessage(166));
                current = rootInstanceStackCursor.current;
                renderLanes2 = contextStackCursor.current;
                if (popHydrationState(workInProgress2)) {
                  if (!supportsHydration) throw Error(formatProdErrorMessage(176));
                  current = workInProgress2.stateNode;
                  renderLanes2 = workInProgress2.memoizedProps;
                  newProps = null;
                  type = hydrationParentFiber;
                  if (null !== type)
                    switch (type.tag) {
                      case 27:
                      case 5:
                        newProps = type.memoizedProps;
                    }
                  hydrateTextInstance(
                    current,
                    renderLanes2,
                    workInProgress2,
                    newProps
                  ) || throwOnHydrationMismatch(workInProgress2, true);
                } else
                  markCloned(workInProgress2), workInProgress2.stateNode = createTextInstance(
                    newProps,
                    current,
                    renderLanes2,
                    workInProgress2
                  );
              }
              bubbleProperties(workInProgress2);
              return null;
            case 31:
              renderLanes2 = workInProgress2.memoizedState;
              if (null === current || null !== current.memoizedState) {
                newProps = popHydrationState(workInProgress2);
                if (null !== renderLanes2) {
                  if (null === current) {
                    if (!newProps) throw Error(formatProdErrorMessage(318));
                    if (!supportsHydration) throw Error(formatProdErrorMessage(556));
                    current = workInProgress2.memoizedState;
                    current = null !== current ? current.dehydrated : null;
                    if (!current) throw Error(formatProdErrorMessage(557));
                    hydrateActivityInstance(current, workInProgress2);
                  } else
                    resetHydrationState(), 0 === (workInProgress2.flags & 128) && (workInProgress2.memoizedState = null), workInProgress2.flags |= 4;
                  bubbleProperties(workInProgress2);
                  current = false;
                } else
                  renderLanes2 = upgradeHydrationErrorsToRecoverable(), null !== current && null !== current.memoizedState && (current.memoizedState.hydrationErrors = renderLanes2), current = true;
                if (!current) {
                  if (workInProgress2.flags & 256)
                    return popSuspenseHandler(workInProgress2), workInProgress2;
                  popSuspenseHandler(workInProgress2);
                  return null;
                }
                if (0 !== (workInProgress2.flags & 128))
                  throw Error(formatProdErrorMessage(558));
              }
              bubbleProperties(workInProgress2);
              return null;
            case 13:
              newProps = workInProgress2.memoizedState;
              if (null === current || null !== current.memoizedState && null !== current.memoizedState.dehydrated) {
                type = popHydrationState(workInProgress2);
                if (null !== newProps && null !== newProps.dehydrated) {
                  if (null === current) {
                    if (!type) throw Error(formatProdErrorMessage(318));
                    if (!supportsHydration) throw Error(formatProdErrorMessage(344));
                    type = workInProgress2.memoizedState;
                    type = null !== type ? type.dehydrated : null;
                    if (!type) throw Error(formatProdErrorMessage(317));
                    hydrateSuspenseInstance(type, workInProgress2);
                  } else
                    resetHydrationState(), 0 === (workInProgress2.flags & 128) && (workInProgress2.memoizedState = null), workInProgress2.flags |= 4;
                  bubbleProperties(workInProgress2);
                  type = false;
                } else
                  type = upgradeHydrationErrorsToRecoverable(), null !== current && null !== current.memoizedState && (current.memoizedState.hydrationErrors = type), type = true;
                if (!type) {
                  if (workInProgress2.flags & 256)
                    return popSuspenseHandler(workInProgress2), workInProgress2;
                  popSuspenseHandler(workInProgress2);
                  return null;
                }
              }
              popSuspenseHandler(workInProgress2);
              if (0 !== (workInProgress2.flags & 128))
                return workInProgress2.lanes = renderLanes2, workInProgress2;
              renderLanes2 = null !== newProps;
              current = null !== current && null !== current.memoizedState;
              renderLanes2 && (newProps = workInProgress2.child, type = null, null !== newProps.alternate && null !== newProps.alternate.memoizedState && null !== newProps.alternate.memoizedState.cachePool && (type = newProps.alternate.memoizedState.cachePool.pool), nextResource = null, null !== newProps.memoizedState && null !== newProps.memoizedState.cachePool && (nextResource = newProps.memoizedState.cachePool.pool), nextResource !== type && (newProps.flags |= 2048));
              renderLanes2 !== current && renderLanes2 && (workInProgress2.child.flags |= 8192);
              scheduleRetryEffect(workInProgress2, workInProgress2.updateQueue);
              bubbleProperties(workInProgress2);
              return null;
            case 4:
              return popHostContainer(), updateHostContainer(current, workInProgress2), null === current && preparePortalMount(workInProgress2.stateNode.containerInfo), workInProgress2.flags |= 67108864, bubbleProperties(workInProgress2), null;
            case 10:
              return popProvider(workInProgress2.type), bubbleProperties(workInProgress2), null;
            case 19:
              popSuspenseListContext(workInProgress2);
              newProps = workInProgress2.memoizedState;
              if (null === newProps) return bubbleProperties(workInProgress2), null;
              type = 0 !== (workInProgress2.flags & 128);
              nextResource = newProps.rendering;
              if (null === nextResource)
                if (type) cutOffTailIfNeeded(newProps, false);
                else {
                  if (0 !== workInProgressRootExitStatus || null !== current && 0 !== (current.flags & 128))
                    for (current = workInProgress2.child; null !== current; ) {
                      nextResource = findFirstSuspended(current);
                      if (null !== nextResource) {
                        workInProgress2.flags |= 128;
                        cutOffTailIfNeeded(newProps, false);
                        current = nextResource.updateQueue;
                        workInProgress2.updateQueue = current;
                        scheduleRetryEffect(workInProgress2, current);
                        workInProgress2.subtreeFlags = 0;
                        current = renderLanes2;
                        for (renderLanes2 = workInProgress2.child; null !== renderLanes2; )
                          resetWorkInProgress(renderLanes2, current), renderLanes2 = renderLanes2.sibling;
                        pushSuspenseListContext(
                          workInProgress2,
                          suspenseStackCursor.current & 1 | 2
                        );
                        isHydrating && pushTreeFork(workInProgress2, newProps.treeForkCount);
                        return workInProgress2.child;
                      }
                      current = current.sibling;
                    }
                  null !== newProps.tail && now2() > workInProgressRootRenderTargetTime && (workInProgress2.flags |= 128, type = true, cutOffTailIfNeeded(newProps, false), workInProgress2.lanes = 4194304);
                }
              else {
                if (!type)
                  if (current = findFirstSuspended(nextResource), null !== current) {
                    if (workInProgress2.flags |= 128, type = true, current = current.updateQueue, workInProgress2.updateQueue = current, scheduleRetryEffect(workInProgress2, current), cutOffTailIfNeeded(newProps, true), null === newProps.tail && "collapsed" !== newProps.tailMode && "visible" !== newProps.tailMode && !nextResource.alternate && !isHydrating)
                      return bubbleProperties(workInProgress2), null;
                  } else
                    2 * now2() - newProps.renderingStartTime > workInProgressRootRenderTargetTime && 536870912 !== renderLanes2 && (workInProgress2.flags |= 128, type = true, cutOffTailIfNeeded(newProps, false), workInProgress2.lanes = 4194304);
                newProps.isBackwards ? (nextResource.sibling = workInProgress2.child, workInProgress2.child = nextResource) : (current = newProps.last, null !== current ? current.sibling = nextResource : workInProgress2.child = nextResource, newProps.last = nextResource);
              }
              if (null !== newProps.tail) {
                current = newProps.tail;
                a: {
                  for (renderLanes2 = current; null !== renderLanes2; ) {
                    if (null !== renderLanes2.alternate) {
                      renderLanes2 = false;
                      break a;
                    }
                    renderLanes2 = renderLanes2.sibling;
                  }
                  renderLanes2 = true;
                }
                newProps.rendering = current;
                newProps.tail = current.sibling;
                newProps.renderingStartTime = now2();
                current.sibling = null;
                nextResource = suspenseStackCursor.current;
                nextResource = type ? nextResource & 1 | 2 : nextResource & 1;
                "visible" === newProps.tailMode || "collapsed" === newProps.tailMode || !renderLanes2 || isHydrating ? pushSuspenseListContext(workInProgress2, nextResource) : (renderLanes2 = nextResource, push(suspenseHandlerStackCursor, workInProgress2), push(suspenseStackCursor, renderLanes2), null === shellBoundary && (shellBoundary = workInProgress2));
                isHydrating && pushTreeFork(workInProgress2, newProps.treeForkCount);
                return current;
              }
              bubbleProperties(workInProgress2);
              return null;
            case 22:
            case 23:
              return popSuspenseHandler(workInProgress2), popHiddenContext(), newProps = null !== workInProgress2.memoizedState, null !== current ? null !== current.memoizedState !== newProps && (workInProgress2.flags |= 8192) : newProps && (workInProgress2.flags |= 8192), newProps ? 0 !== (renderLanes2 & 536870912) && 0 === (workInProgress2.flags & 128) && (bubbleProperties(workInProgress2), workInProgress2.subtreeFlags & 6 && (workInProgress2.flags |= 8192)) : bubbleProperties(workInProgress2), renderLanes2 = workInProgress2.updateQueue, null !== renderLanes2 && scheduleRetryEffect(workInProgress2, renderLanes2.retryQueue), renderLanes2 = null, null !== current && null !== current.memoizedState && null !== current.memoizedState.cachePool && (renderLanes2 = current.memoizedState.cachePool.pool), newProps = null, null !== workInProgress2.memoizedState && null !== workInProgress2.memoizedState.cachePool && (newProps = workInProgress2.memoizedState.cachePool.pool), newProps !== renderLanes2 && (workInProgress2.flags |= 2048), null !== current && pop(resumedCache), null;
            case 24:
              return renderLanes2 = null, null !== current && (renderLanes2 = current.memoizedState.cache), workInProgress2.memoizedState.cache !== renderLanes2 && (workInProgress2.flags |= 2048), popProvider(CacheContext), bubbleProperties(workInProgress2), null;
            case 25:
              return null;
            case 30:
              return workInProgress2.flags |= 33554432, bubbleProperties(workInProgress2), null;
          }
          throw Error(formatProdErrorMessage(156, workInProgress2.tag));
        }
        function unwindWork(current, workInProgress2) {
          popTreeContext(workInProgress2);
          switch (workInProgress2.tag) {
            case 1:
              return current = workInProgress2.flags, current & 65536 ? (workInProgress2.flags = current & -65537 | 128, workInProgress2) : null;
            case 3:
              return popProvider(CacheContext), popHostContainer(), current = workInProgress2.flags, 0 !== (current & 65536) && 0 === (current & 128) ? (workInProgress2.flags = current & -65537 | 128, workInProgress2) : null;
            case 26:
            case 27:
            case 5:
              return popHostContext(workInProgress2), null;
            case 31:
              if (null !== workInProgress2.memoizedState) {
                popSuspenseHandler(workInProgress2);
                if (null === workInProgress2.alternate)
                  throw Error(formatProdErrorMessage(340));
                resetHydrationState();
              }
              current = workInProgress2.flags;
              return current & 65536 ? (workInProgress2.flags = current & -65537 | 128, workInProgress2) : null;
            case 13:
              popSuspenseHandler(workInProgress2);
              current = workInProgress2.memoizedState;
              if (null !== current && null !== current.dehydrated) {
                if (null === workInProgress2.alternate)
                  throw Error(formatProdErrorMessage(340));
                resetHydrationState();
              }
              current = workInProgress2.flags;
              return current & 65536 ? (workInProgress2.flags = current & -65537 | 128, workInProgress2) : null;
            case 19:
              return popSuspenseListContext(workInProgress2), current = workInProgress2.flags, current & 65536 ? (workInProgress2.flags = current & -65537 | 128, current = workInProgress2.memoizedState, null !== current && (current.rendering = null, current.tail = null), workInProgress2.flags |= 4, workInProgress2) : null;
            case 4:
              return popHostContainer(), null;
            case 10:
              return popProvider(workInProgress2.type), null;
            case 22:
            case 23:
              return popSuspenseHandler(workInProgress2), popHiddenContext(), null !== current && pop(resumedCache), current = workInProgress2.flags, current & 65536 ? (workInProgress2.flags = current & -65537 | 128, workInProgress2) : null;
            case 24:
              return popProvider(CacheContext), null;
            case 25:
              return null;
            default:
              return null;
          }
        }
        function unwindInterruptedWork(current, interruptedWork) {
          popTreeContext(interruptedWork);
          switch (interruptedWork.tag) {
            case 3:
              popProvider(CacheContext);
              popHostContainer();
              break;
            case 26:
            case 27:
            case 5:
              popHostContext(interruptedWork);
              break;
            case 4:
              popHostContainer();
              break;
            case 31:
              null !== interruptedWork.memoizedState && popSuspenseHandler(interruptedWork);
              break;
            case 13:
              popSuspenseHandler(interruptedWork);
              break;
            case 19:
              popSuspenseListContext(interruptedWork);
              break;
            case 10:
              popProvider(interruptedWork.type);
              break;
            case 22:
            case 23:
              popSuspenseHandler(interruptedWork);
              popHiddenContext();
              null !== current && pop(resumedCache);
              break;
            case 24:
              popProvider(CacheContext);
          }
        }
        function commitHookEffectListMount(flags, finishedWork) {
          try {
            var updateQueue = finishedWork.updateQueue, lastEffect = null !== updateQueue ? updateQueue.lastEffect : null;
            if (null !== lastEffect) {
              var firstEffect = lastEffect.next;
              updateQueue = firstEffect;
              do {
                if ((updateQueue.tag & flags) === flags) {
                  lastEffect = void 0;
                  var create = updateQueue.create, inst = updateQueue.inst;
                  lastEffect = create();
                  inst.destroy = lastEffect;
                }
                updateQueue = updateQueue.next;
              } while (updateQueue !== firstEffect);
            }
          } catch (error) {
            captureCommitPhaseError(finishedWork, finishedWork.return, error);
          }
        }
        function commitHookEffectListUnmount(flags, finishedWork, nearestMountedAncestor$jscomp$0) {
          try {
            var updateQueue = finishedWork.updateQueue, lastEffect = null !== updateQueue ? updateQueue.lastEffect : null;
            if (null !== lastEffect) {
              var firstEffect = lastEffect.next;
              updateQueue = firstEffect;
              do {
                if ((updateQueue.tag & flags) === flags) {
                  var inst = updateQueue.inst, destroy = inst.destroy;
                  if (void 0 !== destroy) {
                    inst.destroy = void 0;
                    lastEffect = finishedWork;
                    var nearestMountedAncestor = nearestMountedAncestor$jscomp$0, destroy_ = destroy;
                    try {
                      destroy_();
                    } catch (error) {
                      captureCommitPhaseError(
                        lastEffect,
                        nearestMountedAncestor,
                        error
                      );
                    }
                  }
                }
                updateQueue = updateQueue.next;
              } while (updateQueue !== firstEffect);
            }
          } catch (error) {
            captureCommitPhaseError(finishedWork, finishedWork.return, error);
          }
        }
        function commitClassCallbacks(finishedWork) {
          var updateQueue = finishedWork.updateQueue;
          if (null !== updateQueue) {
            var instance = finishedWork.stateNode;
            try {
              commitCallbacks(updateQueue, instance);
            } catch (error) {
              captureCommitPhaseError(finishedWork, finishedWork.return, error);
            }
          }
        }
        function safelyCallComponentWillUnmount(current, nearestMountedAncestor, instance) {
          instance.props = resolveClassComponentProps(
            current.type,
            current.memoizedProps
          );
          instance.state = current.memoizedState;
          try {
            instance.componentWillUnmount();
          } catch (error) {
            captureCommitPhaseError(current, nearestMountedAncestor, error);
          }
        }
        function safelyAttachRef(current, nearestMountedAncestor) {
          try {
            var ref = current.ref;
            if (null !== ref) {
              switch (current.tag) {
                case 26:
                case 27:
                case 5:
                  var instanceToUse = getPublicInstance(current.stateNode);
                  break;
                case 30:
                  var instance = current.stateNode, name = getViewTransitionName(current.memoizedProps, instance);
                  if (null === instance.ref || instance.ref.name !== name)
                    instance.ref = createViewTransitionInstance(name);
                  instanceToUse = instance.ref;
                  break;
                case 7:
                  null === current.stateNode && (current.stateNode = createFragmentInstance(current));
                  instanceToUse = current.stateNode;
                  break;
                default:
                  instanceToUse = current.stateNode;
              }
              "function" === typeof ref ? current.refCleanup = ref(instanceToUse) : ref.current = instanceToUse;
            }
          } catch (error) {
            captureCommitPhaseError(current, nearestMountedAncestor, error);
          }
        }
        function safelyDetachRef(current, nearestMountedAncestor) {
          var ref = current.ref, refCleanup = current.refCleanup;
          if (null !== ref)
            if ("function" === typeof refCleanup)
              try {
                refCleanup();
              } catch (error) {
                captureCommitPhaseError(current, nearestMountedAncestor, error);
              } finally {
                current.refCleanup = null, current = current.alternate, null != current && (current.refCleanup = null);
              }
            else if ("function" === typeof ref)
              try {
                ref(null);
              } catch (error$130) {
                captureCommitPhaseError(current, nearestMountedAncestor, error$130);
              }
            else ref.current = null;
        }
        function pushMutationContext() {
          var prev = viewTransitionMutationContext;
          viewTransitionMutationContext = false;
          return prev;
        }
        function commitNewChildToFragmentInstances(fiber, parentFragmentInstances) {
          if ((5 === fiber.tag || 27 === fiber.tag || 6 === fiber.tag) && null === fiber.alternate && null !== parentFragmentInstances)
            for (var i = 0; i < parentFragmentInstances.length; i++)
              commitNewChildToFragmentInstance(
                fiber.stateNode,
                parentFragmentInstances[i]
              );
        }
        function commitFragmentInstanceInsertionEffects(fiber) {
          for (var parent = fiber.return; null !== parent; ) {
            isFragmentInstanceParent(parent) && commitNewChildToFragmentInstance(fiber.stateNode, parent.stateNode);
            if (isFragmentInstanceHostBoundary(parent)) break;
            parent = parent.return;
          }
        }
        function commitFragmentInstanceDeletionEffects(fiber) {
          for (var parent = fiber.return; null !== parent; ) {
            isFragmentInstanceParent(parent) && deleteChildFromFragmentInstance(fiber.stateNode, parent.stateNode);
            if (isFragmentInstanceHostBoundary(parent)) break;
            parent = parent.return;
          }
        }
        function isFragmentInstanceHostBoundary(fiber) {
          return 5 === fiber.tag || 3 === fiber.tag || (supportsSingletons ? 27 === fiber.tag : false);
        }
        function isFragmentInstanceParent(fiber) {
          return fiber && 7 === fiber.tag && null !== fiber.stateNode;
        }
        function commitHostMount(finishedWork) {
          var type = finishedWork.type, props = finishedWork.memoizedProps, instance = finishedWork.stateNode;
          try {
            commitMount(instance, type, props, finishedWork);
          } catch (error) {
            captureCommitPhaseError(finishedWork, finishedWork.return, error);
          }
        }
        function commitHostUpdate(finishedWork, newProps, oldProps) {
          try {
            commitUpdate(
              finishedWork.stateNode,
              finishedWork.type,
              oldProps,
              newProps,
              finishedWork
            );
          } catch (error) {
            captureCommitPhaseError(finishedWork, finishedWork.return, error);
          }
        }
        function isHostParent(fiber) {
          return 5 === fiber.tag || 3 === fiber.tag || (supportsResources ? 26 === fiber.tag : false) || (supportsSingletons ? 27 === fiber.tag && isSingletonScope(fiber.type) : false) || 4 === fiber.tag;
        }
        function getHostSibling(fiber) {
          a: for (; ; ) {
            for (; null === fiber.sibling; ) {
              if (null === fiber.return || isHostParent(fiber.return)) return null;
              fiber = fiber.return;
            }
            fiber.sibling.return = fiber.return;
            for (fiber = fiber.sibling; 5 !== fiber.tag && 6 !== fiber.tag && 18 !== fiber.tag; ) {
              if (supportsSingletons && 27 === fiber.tag && isSingletonScope(fiber.type))
                continue a;
              if (fiber.flags & 2) continue a;
              if (null === fiber.child || 4 === fiber.tag) continue a;
              else fiber.child.return = fiber, fiber = fiber.child;
            }
            if (!(fiber.flags & 2)) return fiber.stateNode;
          }
        }
        function insertOrAppendPlacementNodeIntoContainer(node, before, parent, parentFragmentInstances) {
          var tag = node.tag;
          if (5 === tag || 6 === tag)
            tag = node.stateNode, before ? insertInContainerBefore(parent, tag, before) : appendChildToContainer(parent, tag), commitNewChildToFragmentInstances(node, parentFragmentInstances), viewTransitionMutationContext = true;
          else if (4 !== tag && (supportsSingletons && 27 === tag && (commitNewChildToFragmentInstances(node, parentFragmentInstances), parentFragmentInstances = null, isSingletonScope(node.type) && (parent = node.stateNode, before = null)), node = node.child, null !== node))
            for (insertOrAppendPlacementNodeIntoContainer(
              node,
              before,
              parent,
              parentFragmentInstances
            ), node = node.sibling; null !== node; )
              insertOrAppendPlacementNodeIntoContainer(
                node,
                before,
                parent,
                parentFragmentInstances
              ), node = node.sibling;
        }
        function insertOrAppendPlacementNode(node, before, parent, parentFragmentInstances) {
          var tag = node.tag;
          if (5 === tag || 6 === tag)
            tag = node.stateNode, before ? insertBefore(parent, tag, before) : appendChild(parent, tag), commitNewChildToFragmentInstances(node, parentFragmentInstances), viewTransitionMutationContext = true;
          else if (4 !== tag && (supportsSingletons && 27 === tag && (commitNewChildToFragmentInstances(node, parentFragmentInstances), parentFragmentInstances = null, isSingletonScope(node.type) && (parent = node.stateNode)), node = node.child, null !== node))
            for (insertOrAppendPlacementNode(
              node,
              before,
              parent,
              parentFragmentInstances
            ), node = node.sibling; null !== node; )
              insertOrAppendPlacementNode(
                node,
                before,
                parent,
                parentFragmentInstances
              ), node = node.sibling;
        }
        function commitImmutablePlacementNodeToFragmentInstances(finishedWork, parentFragmentInstances) {
          if (5 === finishedWork.tag || supportsSingletons && 27 === finishedWork.tag)
            commitNewChildToFragmentInstances(finishedWork, parentFragmentInstances);
          else if (4 !== finishedWork.tag && (finishedWork = finishedWork.child, null !== finishedWork))
            for (commitImmutablePlacementNodeToFragmentInstances(
              finishedWork,
              parentFragmentInstances
            ), finishedWork = finishedWork.sibling; null !== finishedWork; )
              commitImmutablePlacementNodeToFragmentInstances(
                finishedWork,
                parentFragmentInstances
              ), finishedWork = finishedWork.sibling;
        }
        function commitHostPortalContainerChildren(portal, finishedWork, pendingChildren) {
          portal = portal.containerInfo;
          try {
            replaceContainerChildren(portal, pendingChildren);
          } catch (error) {
            captureCommitPhaseError(finishedWork, finishedWork.return, error);
          }
        }
        function commitHostSingletonAcquisition(finishedWork) {
          var singleton = finishedWork.stateNode, props = finishedWork.memoizedProps;
          try {
            acquireSingletonInstance(
              finishedWork.type,
              props,
              singleton,
              finishedWork
            );
          } catch (error) {
            captureCommitPhaseError(finishedWork, finishedWork.return, error);
          }
        }
        function trackEnterViewTransitions(placement) {
          if (30 === placement.tag || 0 !== (placement.subtreeFlags & 33554432))
            shouldStartViewTransition = true;
        }
        function pushViewTransitionCancelableScope() {
          var prevChildren = viewTransitionCancelableChildren;
          viewTransitionCancelableChildren = null;
          return prevChildren;
        }
        function applyViewTransitionToHostInstances(fiber, name, className, collectMeasurements, stopAtNestedViewTransitions) {
          viewTransitionHostInstanceIdx = 0;
          return applyViewTransitionToHostInstancesRecursive(
            fiber.child,
            name,
            className,
            collectMeasurements,
            stopAtNestedViewTransitions
          );
        }
        function applyViewTransitionToHostInstancesRecursive(child, name, className, collectMeasurements, stopAtNestedViewTransitions) {
          if (!supportsMutation) return false;
          for (var inViewport = false; null !== child; ) {
            if (5 === child.tag) {
              var instance = child.stateNode;
              if (null !== collectMeasurements) {
                var measurement = measureInstance(instance);
                collectMeasurements.push(measurement);
                wasInstanceInViewport(measurement) && (inViewport = true);
              } else
                inViewport || wasInstanceInViewport(measureInstance(instance)) && (inViewport = true);
              shouldStartViewTransition = true;
              applyViewTransitionName(
                instance,
                0 === viewTransitionHostInstanceIdx ? name : name + "_" + viewTransitionHostInstanceIdx,
                className
              );
              viewTransitionHostInstanceIdx++;
            } else if (22 !== child.tag || null === child.memoizedState)
              30 === child.tag && stopAtNestedViewTransitions || applyViewTransitionToHostInstancesRecursive(
                child.child,
                name,
                className,
                collectMeasurements,
                stopAtNestedViewTransitions
              ) && (inViewport = true);
            child = child.sibling;
          }
          return inViewport;
        }
        function restoreViewTransitionOnHostInstances(child, stopAtNestedViewTransitions) {
          if (supportsMutation)
            for (; null !== child; ) {
              if (5 === child.tag)
                restoreViewTransitionName(child.stateNode, child.memoizedProps);
              else if (22 !== child.tag || null === child.memoizedState)
                30 === child.tag && stopAtNestedViewTransitions || restoreViewTransitionOnHostInstances(
                  child.child,
                  stopAtNestedViewTransitions
                );
              child = child.sibling;
            }
        }
        function commitAppearingPairViewTransitions(placement) {
          if (0 !== (placement.subtreeFlags & 18874368))
            for (placement = placement.child; null !== placement; ) {
              if (22 !== placement.tag || null === placement.memoizedState) {
                if (commitAppearingPairViewTransitions(placement), 30 === placement.tag && 0 !== (placement.flags & 18874368) && placement.stateNode.paired) {
                  var props = placement.memoizedProps;
                  if (null == props.name || "auto" === props.name)
                    throw Error(formatProdErrorMessage(544));
                  var name = props.name;
                  props = getViewTransitionClassName(props.default, props.share);
                  "none" !== props && (applyViewTransitionToHostInstances(
                    placement,
                    name,
                    props,
                    null,
                    false
                  ) || restoreViewTransitionOnHostInstances(placement.child, false));
                }
              }
              placement = placement.sibling;
            }
        }
        function commitEnterViewTransitions(placement, gesture) {
          if (30 === placement.tag) {
            var state = placement.stateNode, props = placement.memoizedProps, name = getViewTransitionName(props, state), className = getViewTransitionClassName(
              props.default,
              state.paired ? props.share : props.enter
            );
            "none" !== className ? applyViewTransitionToHostInstances(
              placement,
              name,
              className,
              null,
              false
            ) ? (commitAppearingPairViewTransitions(placement), state.paired || gesture || scheduleViewTransitionEvent(placement, props.onEnter)) : restoreViewTransitionOnHostInstances(placement.child, false) : commitAppearingPairViewTransitions(placement);
          } else if (0 !== (placement.subtreeFlags & 33554432))
            for (placement = placement.child; null !== placement; )
              commitEnterViewTransitions(placement, gesture), placement = placement.sibling;
          else commitAppearingPairViewTransitions(placement);
        }
        function commitDeletedPairViewTransitions(deletion) {
          if (null !== appearingViewTransitions && 0 !== appearingViewTransitions.size) {
            var pairs = appearingViewTransitions;
            if (0 !== (deletion.subtreeFlags & 18874368))
              for (deletion = deletion.child; null !== deletion; ) {
                if (22 !== deletion.tag || null === deletion.memoizedState) {
                  if (30 === deletion.tag && 0 !== (deletion.flags & 18874368)) {
                    var props = deletion.memoizedProps, name = props.name;
                    if (null != name && "auto" !== name) {
                      var pair = pairs.get(name);
                      if (void 0 !== pair) {
                        var className = getViewTransitionClassName(
                          props.default,
                          props.share
                        );
                        "none" !== className && (applyViewTransitionToHostInstances(
                          deletion,
                          name,
                          className,
                          null,
                          false
                        ) ? (className = deletion.stateNode, pair.paired = className, className.paired = pair, scheduleViewTransitionEvent(deletion, props.onShare)) : restoreViewTransitionOnHostInstances(
                          deletion.child,
                          false
                        ));
                        pairs.delete(name);
                        if (0 === pairs.size) break;
                      }
                    }
                  }
                  commitDeletedPairViewTransitions(deletion);
                }
                deletion = deletion.sibling;
              }
          }
        }
        function commitExitViewTransitions(deletion) {
          if (30 === deletion.tag) {
            var props = deletion.memoizedProps, name = getViewTransitionName(props, deletion.stateNode), pair = null !== appearingViewTransitions ? appearingViewTransitions.get(name) : void 0, className = getViewTransitionClassName(
              props.default,
              void 0 !== pair ? props.share : props.exit
            );
            "none" !== className && (applyViewTransitionToHostInstances(deletion, name, className, null, false) ? void 0 !== pair ? (className = deletion.stateNode, pair.paired = className, className.paired = pair, appearingViewTransitions.delete(name), scheduleViewTransitionEvent(deletion, props.onShare)) : scheduleViewTransitionEvent(deletion, props.onExit) : restoreViewTransitionOnHostInstances(deletion.child, false));
            null !== appearingViewTransitions && commitDeletedPairViewTransitions(deletion);
          } else if (0 !== (deletion.subtreeFlags & 33554432))
            for (deletion = deletion.child; null !== deletion; )
              commitExitViewTransitions(deletion), deletion = deletion.sibling;
          else
            null !== appearingViewTransitions && commitDeletedPairViewTransitions(deletion);
        }
        function commitNestedViewTransitions(changedParent) {
          for (changedParent = changedParent.child; null !== changedParent; ) {
            if (30 === changedParent.tag) {
              var props = changedParent.memoizedProps, name = getViewTransitionName(props, changedParent.stateNode);
              props = getViewTransitionClassName(props.default, props.update);
              changedParent.flags &= -5;
              "none" !== props && applyViewTransitionToHostInstances(
                changedParent,
                name,
                props,
                changedParent.memoizedState = [],
                false
              );
            } else
              0 !== (changedParent.subtreeFlags & 33554432) && commitNestedViewTransitions(changedParent);
            changedParent = changedParent.sibling;
          }
        }
        function restorePairedViewTransitions(parent) {
          if (0 !== (parent.subtreeFlags & 18874368))
            for (parent = parent.child; null !== parent; ) {
              if (22 !== parent.tag || null === parent.memoizedState) {
                if (30 === parent.tag && 0 !== (parent.flags & 18874368)) {
                  var instance = parent.stateNode;
                  null !== instance.paired && (instance.paired = null, restoreViewTransitionOnHostInstances(parent.child, false));
                }
                restorePairedViewTransitions(parent);
              }
              parent = parent.sibling;
            }
        }
        function restoreEnterOrExitViewTransitions(fiber) {
          if (30 === fiber.tag)
            fiber.stateNode.paired = null, restoreViewTransitionOnHostInstances(fiber.child, false), restorePairedViewTransitions(fiber);
          else if (0 !== (fiber.subtreeFlags & 33554432))
            for (fiber = fiber.child; null !== fiber; )
              restoreEnterOrExitViewTransitions(fiber), fiber = fiber.sibling;
          else restorePairedViewTransitions(fiber);
        }
        function restoreNestedViewTransitions(changedParent) {
          for (changedParent = changedParent.child; null !== changedParent; )
            30 === changedParent.tag ? restoreViewTransitionOnHostInstances(changedParent.child, false) : 0 !== (changedParent.subtreeFlags & 33554432) && restoreNestedViewTransitions(changedParent), changedParent = changedParent.sibling;
        }
        function measureViewTransitionHostInstancesRecursive(parentViewTransition, child, newName, oldName, className, previousMeasurements, stopAtNestedViewTransitions) {
          if (!supportsMutation) return false;
          for (var inViewport = false; null !== child; ) {
            if (5 === child.tag) {
              var instance = child.stateNode;
              if (null !== previousMeasurements && viewTransitionHostInstanceIdx < previousMeasurements.length) {
                var previousMeasurement = previousMeasurements[viewTransitionHostInstanceIdx], nextMeasurement = measureInstance(instance);
                if (wasInstanceInViewport(previousMeasurement) || wasInstanceInViewport(nextMeasurement))
                  inViewport = true;
                0 === (parentViewTransition.flags & 4) && hasInstanceChanged(previousMeasurement, nextMeasurement) && (parentViewTransition.flags |= 4);
                hasInstanceAffectedParent(previousMeasurement, nextMeasurement) && (parentViewTransition.flags |= 32);
              } else parentViewTransition.flags |= 32;
              0 !== (parentViewTransition.flags & 4) && applyViewTransitionName(
                instance,
                0 === viewTransitionHostInstanceIdx ? newName : newName + "_" + viewTransitionHostInstanceIdx,
                className
              );
              inViewport && 0 !== (parentViewTransition.flags & 4) || (null === viewTransitionCancelableChildren && (viewTransitionCancelableChildren = []), viewTransitionCancelableChildren.push(
                instance,
                0 === viewTransitionHostInstanceIdx ? oldName : oldName + "_" + viewTransitionHostInstanceIdx,
                child.memoizedProps
              ));
              viewTransitionHostInstanceIdx++;
            } else if (22 !== child.tag || null === child.memoizedState)
              30 === child.tag && stopAtNestedViewTransitions ? parentViewTransition.flags |= child.flags & 32 : measureViewTransitionHostInstancesRecursive(
                parentViewTransition,
                child.child,
                newName,
                oldName,
                className,
                previousMeasurements,
                stopAtNestedViewTransitions
              ) && (inViewport = true);
            child = child.sibling;
          }
          return inViewport;
        }
        function measureNestedViewTransitions(changedParent, gesture) {
          for (changedParent = changedParent.child; null !== changedParent; ) {
            if (30 === changedParent.tag) {
              var props = changedParent.memoizedProps, state = changedParent.stateNode, name = getViewTransitionName(props, state), className = getViewTransitionClassName(props.default, props.update);
              if (gesture) {
                state = state.clones;
                var previousMeasurements = null === state ? null : state.map(measureClonedInstance);
              } else
                previousMeasurements = changedParent.memoizedState, changedParent.memoizedState = null;
              state = changedParent;
              var child = changedParent.child;
              viewTransitionHostInstanceIdx = 0;
              name = measureViewTransitionHostInstancesRecursive(
                state,
                child,
                name,
                name,
                className,
                previousMeasurements,
                false
              );
              0 !== (changedParent.flags & 4) && name && (gesture || scheduleViewTransitionEvent(changedParent, props.onUpdate));
            } else
              0 !== (changedParent.subtreeFlags & 33554432) && measureNestedViewTransitions(changedParent, gesture);
            changedParent = changedParent.sibling;
          }
        }
        function commitBeforeMutationEffects(root2, firstChild, committedLanes) {
          prepareForCommit(root2.containerInfo);
          root2 = (committedLanes & 335544064) === committedLanes;
          nextEffect = firstChild;
          for (firstChild = root2 ? 9270 : 1024; null !== nextEffect; ) {
            committedLanes = nextEffect;
            if (root2) {
              var deletions = committedLanes.deletions;
              if (null !== deletions)
                for (var i = 0; i < deletions.length; i++)
                  root2 && commitExitViewTransitions(deletions[i]);
            }
            if (null === committedLanes.alternate && 0 !== (committedLanes.flags & 2))
              root2 && trackEnterViewTransitions(committedLanes), commitBeforeMutationEffects_complete(root2);
            else {
              if (22 === committedLanes.tag) {
                if (deletions = committedLanes.alternate, null !== committedLanes.memoizedState) {
                  null !== deletions && null === deletions.memoizedState && root2 && commitExitViewTransitions(deletions);
                  commitBeforeMutationEffects_complete(root2);
                  continue;
                } else if (null !== deletions && null !== deletions.memoizedState) {
                  root2 && trackEnterViewTransitions(committedLanes);
                  commitBeforeMutationEffects_complete(root2);
                  continue;
                }
              }
              deletions = committedLanes.child;
              0 !== (committedLanes.subtreeFlags & firstChild) && null !== deletions ? (deletions.return = committedLanes, nextEffect = deletions) : (root2 && commitNestedViewTransitions(committedLanes), commitBeforeMutationEffects_complete(root2));
            }
          }
          appearingViewTransitions = null;
        }
        function commitBeforeMutationEffects_complete(isViewTransitionEligible$jscomp$0) {
          for (; null !== nextEffect; ) {
            var fiber = nextEffect, isViewTransitionEligible = isViewTransitionEligible$jscomp$0, current = fiber.alternate, flags = fiber.flags;
            switch (fiber.tag) {
              case 0:
              case 11:
              case 15:
                break;
              case 1:
                if (0 !== (flags & 1024) && null !== current) {
                  isViewTransitionEligible = void 0;
                  flags = current.memoizedProps;
                  current = current.memoizedState;
                  var instance = fiber.stateNode;
                  try {
                    var resolvedPrevProps = resolveClassComponentProps(
                      fiber.type,
                      flags
                    );
                    isViewTransitionEligible = instance.getSnapshotBeforeUpdate(
                      resolvedPrevProps,
                      current
                    );
                    instance.__reactInternalSnapshotBeforeUpdate = isViewTransitionEligible;
                  } catch (error) {
                    captureCommitPhaseError(fiber, fiber.return, error);
                  }
                }
                break;
              case 3:
                0 !== (flags & 1024) && supportsMutation && clearContainer(fiber.stateNode.containerInfo);
                break;
              case 5:
              case 26:
              case 27:
              case 6:
              case 4:
              case 17:
                break;
              case 30:
                isViewTransitionEligible && null !== current && (isViewTransitionEligible = getViewTransitionName(
                  current.memoizedProps,
                  current.stateNode
                ), flags = fiber.memoizedProps, flags = getViewTransitionClassName(flags.default, flags.update), "none" !== flags && applyViewTransitionToHostInstances(
                  current,
                  isViewTransitionEligible,
                  flags,
                  current.memoizedState = [],
                  true
                ));
                break;
              default:
                if (0 !== (flags & 1024)) throw Error(formatProdErrorMessage(163));
            }
            current = fiber.sibling;
            if (null !== current) {
              current.return = fiber.return;
              nextEffect = current;
              break;
            }
            nextEffect = fiber.return;
          }
        }
        function commitLayoutEffectOnFiber(finishedRoot, current, finishedWork) {
          var flags = finishedWork.flags;
          switch (finishedWork.tag) {
            case 0:
            case 11:
            case 15:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              flags & 4 && commitHookEffectListMount(5, finishedWork);
              break;
            case 1:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              if (flags & 4)
                if (finishedRoot = finishedWork.stateNode, null === current)
                  try {
                    finishedRoot.componentDidMount();
                  } catch (error) {
                    captureCommitPhaseError(finishedWork, finishedWork.return, error);
                  }
                else {
                  var prevProps = resolveClassComponentProps(
                    finishedWork.type,
                    current.memoizedProps
                  );
                  current = current.memoizedState;
                  try {
                    finishedRoot.componentDidUpdate(
                      prevProps,
                      current,
                      finishedRoot.__reactInternalSnapshotBeforeUpdate
                    );
                  } catch (error$128) {
                    captureCommitPhaseError(
                      finishedWork,
                      finishedWork.return,
                      error$128
                    );
                  }
                }
              flags & 64 && commitClassCallbacks(finishedWork);
              flags & 512 && safelyAttachRef(finishedWork, finishedWork.return);
              break;
            case 3:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              if (flags & 64 && (flags = finishedWork.updateQueue, null !== flags)) {
                finishedRoot = null;
                if (null !== finishedWork.child)
                  switch (finishedWork.child.tag) {
                    case 27:
                    case 5:
                      finishedRoot = getPublicInstance(finishedWork.child.stateNode);
                      break;
                    case 1:
                      finishedRoot = finishedWork.child.stateNode;
                  }
                try {
                  commitCallbacks(flags, finishedRoot);
                } catch (error) {
                  captureCommitPhaseError(finishedWork, finishedWork.return, error);
                }
              }
              break;
            case 27:
              supportsSingletons && null === current && flags & 4 && commitHostSingletonAcquisition(finishedWork);
            case 26:
            case 5:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              if (null === current) {
                if (flags & 4) commitHostMount(finishedWork);
                else if (flags & 64) {
                  finishedRoot = finishedWork.type;
                  current = finishedWork.memoizedProps;
                  prevProps = finishedWork.stateNode;
                  try {
                    commitHydratedInstance(
                      prevProps,
                      finishedRoot,
                      current,
                      finishedWork
                    );
                  } catch (error) {
                    captureCommitPhaseError(finishedWork, finishedWork.return, error);
                  }
                }
              }
              flags & 512 && safelyAttachRef(finishedWork, finishedWork.return);
              break;
            case 12:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              break;
            case 31:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              flags & 4 && commitActivityHydrationCallbacks(finishedRoot, finishedWork);
              break;
            case 13:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              flags & 4 && commitSuspenseHydrationCallbacks(finishedRoot, finishedWork);
              flags & 64 && (flags = finishedWork.memoizedState, null !== flags && (flags = flags.dehydrated, null !== flags && (finishedWork = retryDehydratedSuspenseBoundary.bind(
                null,
                finishedWork
              ), registerSuspenseInstanceRetry(flags, finishedWork))));
              break;
            case 22:
              flags = null !== finishedWork.memoizedState || offscreenSubtreeIsHidden;
              if (!flags) {
                var newOffscreenSubtreeWasHidden = null !== current && null !== current.memoizedState || offscreenSubtreeWasHidden;
                current = offscreenSubtreeIsHidden;
                prevProps = offscreenSubtreeWasHidden;
                offscreenSubtreeIsHidden = flags;
                (offscreenSubtreeWasHidden = newOffscreenSubtreeWasHidden) && !prevProps ? (flags = supportsSingletons ? 2 : 0, 0 !== (finishedWork.subtreeFlags & 8772) && (flags |= 1), recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  flags
                )) : recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
                offscreenSubtreeIsHidden = current;
                offscreenSubtreeWasHidden = prevProps;
              }
              break;
            case 30:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
              flags & 512 && safelyAttachRef(finishedWork, finishedWork.return);
              break;
            case 7:
              flags & 512 && safelyAttachRef(finishedWork, finishedWork.return);
            default:
              recursivelyTraverseLayoutEffects(finishedRoot, finishedWork);
          }
        }
        function hideOrUnhideAllChildren(parentFiber, isHidden) {
          if (supportsMutation)
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              hideOrUnhideAllChildrenOnFiber(parentFiber, isHidden), parentFiber = parentFiber.sibling;
        }
        function hideOrUnhideAllChildrenOnFiber(fiber, isHidden) {
          if (supportsMutation)
            switch (fiber.tag) {
              case 5:
              case 26:
                try {
                  var instance = fiber.stateNode;
                  isHidden ? hideInstance(instance) : unhideInstance(fiber.stateNode, fiber.memoizedProps);
                } catch (error) {
                  captureCommitPhaseError(fiber, fiber.return, error);
                }
                hideOrUnhideNearestPortals(fiber, isHidden);
                break;
              case 6:
                try {
                  var instance$jscomp$0 = fiber.stateNode;
                  isHidden ? hideTextInstance(instance$jscomp$0) : unhideTextInstance(instance$jscomp$0, fiber.memoizedProps);
                  viewTransitionMutationContext = true;
                } catch (error) {
                  captureCommitPhaseError(fiber, fiber.return, error);
                }
                break;
              case 18:
                try {
                  var instance$jscomp$1 = fiber.stateNode;
                  isHidden ? hideDehydratedBoundary(instance$jscomp$1) : unhideDehydratedBoundary(fiber.stateNode);
                } catch (error) {
                  captureCommitPhaseError(fiber, fiber.return, error);
                }
                break;
              case 22:
              case 23:
                null === fiber.memoizedState && hideOrUnhideAllChildren(fiber, isHidden);
                break;
              default:
                hideOrUnhideAllChildren(fiber, isHidden);
            }
        }
        function hideOrUnhideNearestPortals(parentFiber, isHidden$jscomp$0) {
          if (supportsMutation && parentFiber.subtreeFlags & 67108864)
            for (parentFiber = parentFiber.child; null !== parentFiber; ) {
              var fiber = parentFiber, isHidden = isHidden$jscomp$0;
              if (supportsMutation)
                switch (fiber.tag) {
                  case 4:
                    hideOrUnhideAllChildrenOnFiber(fiber, isHidden);
                    break;
                  case 22:
                    null === fiber.memoizedState && hideOrUnhideNearestPortals(fiber, isHidden);
                    break;
                  default:
                    hideOrUnhideNearestPortals(fiber, isHidden);
                }
              parentFiber = parentFiber.sibling;
            }
        }
        function detachFiberAfterEffects(fiber) {
          var alternate = fiber.alternate;
          null !== alternate && (fiber.alternate = null, detachFiberAfterEffects(alternate));
          fiber.child = null;
          fiber.deletions = null;
          fiber.sibling = null;
          5 === fiber.tag && (alternate = fiber.stateNode, null !== alternate && detachDeletedInstance(alternate));
          fiber.stateNode = null;
          fiber.return = null;
          fiber.dependencies = null;
          fiber.memoizedProps = null;
          fiber.memoizedState = null;
          fiber.pendingProps = null;
          fiber.stateNode = null;
          fiber.updateQueue = null;
        }
        function recursivelyTraverseDeletionEffects(finishedRoot, nearestMountedAncestor, parent) {
          for (parent = parent.child; null !== parent; )
            commitDeletionEffectsOnFiber(
              finishedRoot,
              nearestMountedAncestor,
              parent
            ), parent = parent.sibling;
        }
        function commitDeletionEffectsOnFiber(finishedRoot, nearestMountedAncestor, deletedFiber) {
          if (injectedHook && "function" === typeof injectedHook.onCommitFiberUnmount)
            try {
              injectedHook.onCommitFiberUnmount(rendererID, deletedFiber);
            } catch (err) {
            }
          switch (deletedFiber.tag) {
            case 26:
              if (supportsResources) {
                offscreenSubtreeWasHidden || safelyDetachRef(deletedFiber, nearestMountedAncestor);
                recursivelyTraverseDeletionEffects(
                  finishedRoot,
                  nearestMountedAncestor,
                  deletedFiber
                );
                deletedFiber.memoizedState ? releaseResource(deletedFiber.memoizedState) : deletedFiber.stateNode && (offscreenSubtreeWasHidden || unmountHoistable(deletedFiber.stateNode));
                break;
              }
            case 27:
              if (supportsSingletons) {
                offscreenSubtreeWasHidden || safelyDetachRef(deletedFiber, nearestMountedAncestor);
                commitFragmentInstanceDeletionEffects(deletedFiber);
                var prevHostParent = hostParent, prevHostParentIsContainer = hostParentIsContainer;
                isSingletonScope(deletedFiber.type) && (hostParent = deletedFiber.stateNode, hostParentIsContainer = false);
                recursivelyTraverseDeletionEffects(
                  finishedRoot,
                  nearestMountedAncestor,
                  deletedFiber
                );
                releaseSingletonInstance(
                  deletedFiber.stateNode,
                  deletedFiber.type,
                  deletedFiber.memoizedProps
                );
                hostParent = prevHostParent;
                hostParentIsContainer = prevHostParentIsContainer;
                break;
              }
            case 5:
              offscreenSubtreeWasHidden || safelyDetachRef(deletedFiber, nearestMountedAncestor), commitFragmentInstanceDeletionEffects(deletedFiber);
            case 6:
              6 === deletedFiber.tag && commitFragmentInstanceDeletionEffects(deletedFiber);
              if (supportsMutation) {
                if (prevHostParent = hostParent, prevHostParentIsContainer = hostParentIsContainer, hostParent = null, recursivelyTraverseDeletionEffects(
                  finishedRoot,
                  nearestMountedAncestor,
                  deletedFiber
                ), hostParent = prevHostParent, hostParentIsContainer = prevHostParentIsContainer, null !== hostParent)
                  if (hostParentIsContainer)
                    try {
                      removeChildFromContainer(hostParent, deletedFiber.stateNode), viewTransitionMutationContext = true;
                    } catch (error) {
                      captureCommitPhaseError(
                        deletedFiber,
                        nearestMountedAncestor,
                        error
                      );
                    }
                  else
                    try {
                      removeChild(hostParent, deletedFiber.stateNode), viewTransitionMutationContext = true;
                    } catch (error) {
                      captureCommitPhaseError(
                        deletedFiber,
                        nearestMountedAncestor,
                        error
                      );
                    }
              } else
                recursivelyTraverseDeletionEffects(
                  finishedRoot,
                  nearestMountedAncestor,
                  deletedFiber
                );
              break;
            case 18:
              supportsMutation && null !== hostParent && (hostParentIsContainer ? clearSuspenseBoundaryFromContainer(
                hostParent,
                deletedFiber.stateNode
              ) : clearSuspenseBoundary(hostParent, deletedFiber.stateNode));
              break;
            case 4:
              supportsMutation ? (prevHostParent = hostParent, prevHostParentIsContainer = hostParentIsContainer, hostParent = deletedFiber.stateNode.containerInfo, hostParentIsContainer = true, recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              ), hostParent = prevHostParent, hostParentIsContainer = prevHostParentIsContainer) : (supportsPersistence && commitHostPortalContainerChildren(
                deletedFiber.stateNode,
                deletedFiber,
                createContainerChildSet()
              ), recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              ));
              break;
            case 0:
            case 11:
            case 14:
            case 15:
              commitHookEffectListUnmount(2, deletedFiber, nearestMountedAncestor);
              offscreenSubtreeWasHidden || commitHookEffectListUnmount(4, deletedFiber, nearestMountedAncestor);
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
              break;
            case 1:
              offscreenSubtreeWasHidden || (safelyDetachRef(deletedFiber, nearestMountedAncestor), prevHostParent = deletedFiber.stateNode, "function" === typeof prevHostParent.componentWillUnmount && safelyCallComponentWillUnmount(
                deletedFiber,
                nearestMountedAncestor,
                prevHostParent
              ));
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
              break;
            case 21:
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
              break;
            case 22:
              offscreenSubtreeWasHidden = (prevHostParent = offscreenSubtreeWasHidden) || null !== deletedFiber.memoizedState;
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
              offscreenSubtreeWasHidden = prevHostParent;
              break;
            case 30:
              safelyDetachRef(deletedFiber, nearestMountedAncestor);
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
              break;
            case 7:
              offscreenSubtreeWasHidden || safelyDetachRef(deletedFiber, nearestMountedAncestor);
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
              break;
            default:
              recursivelyTraverseDeletionEffects(
                finishedRoot,
                nearestMountedAncestor,
                deletedFiber
              );
          }
        }
        function commitActivityHydrationCallbacks(finishedRoot, finishedWork) {
          if (supportsHydration && null === finishedWork.memoizedState && (finishedRoot = finishedWork.alternate, null !== finishedRoot && (finishedRoot = finishedRoot.memoizedState, null !== finishedRoot))) {
            finishedRoot = finishedRoot.dehydrated;
            try {
              commitHydratedActivityInstance(finishedRoot);
            } catch (error) {
              captureCommitPhaseError(finishedWork, finishedWork.return, error);
            }
          }
        }
        function commitSuspenseHydrationCallbacks(finishedRoot, finishedWork) {
          if (supportsHydration && null === finishedWork.memoizedState && (finishedRoot = finishedWork.alternate, null !== finishedRoot && (finishedRoot = finishedRoot.memoizedState, null !== finishedRoot && (finishedRoot = finishedRoot.dehydrated, null !== finishedRoot))))
            try {
              commitHydratedSuspenseInstance(finishedRoot);
            } catch (error) {
              captureCommitPhaseError(finishedWork, finishedWork.return, error);
            }
        }
        function getRetryCache(finishedWork) {
          switch (finishedWork.tag) {
            case 31:
            case 13:
            case 19:
              var retryCache = finishedWork.stateNode;
              null === retryCache && (retryCache = finishedWork.stateNode = new PossiblyWeakSet());
              return retryCache;
            case 22:
              return finishedWork = finishedWork.stateNode, retryCache = finishedWork._retryCache, null === retryCache && (retryCache = finishedWork._retryCache = new PossiblyWeakSet()), retryCache;
            default:
              throw Error(formatProdErrorMessage(435, finishedWork.tag));
          }
        }
        function attachSuspenseRetryListeners(finishedWork, wakeables) {
          var retryCache = getRetryCache(finishedWork);
          wakeables.forEach(function(wakeable) {
            if (!retryCache.has(wakeable)) {
              retryCache.add(wakeable);
              var retry = resolveRetryWakeable.bind(null, finishedWork, wakeable);
              wakeable.then(retry, retry);
            }
          });
        }
        function recursivelyTraverseMutationEffects(root$jscomp$0, parentFiber, lanes) {
          var deletions = parentFiber.deletions;
          if (null !== deletions)
            for (var i = 0; i < deletions.length; i++) {
              var childToDelete = deletions[i], root2 = root$jscomp$0, returnFiber = parentFiber;
              if (supportsMutation) {
                var parent = returnFiber;
                a: for (; null !== parent; ) {
                  switch (parent.tag) {
                    case 27:
                      if (supportsSingletons) {
                        if (isSingletonScope(parent.type)) {
                          hostParent = parent.stateNode;
                          hostParentIsContainer = false;
                          break a;
                        }
                        break;
                      }
                    case 5:
                      hostParent = parent.stateNode;
                      hostParentIsContainer = false;
                      break a;
                    case 3:
                    case 4:
                      hostParent = parent.stateNode.containerInfo;
                      hostParentIsContainer = true;
                      break a;
                  }
                  parent = parent.return;
                }
                if (null === hostParent) throw Error(formatProdErrorMessage(160));
                commitDeletionEffectsOnFiber(root2, returnFiber, childToDelete);
                hostParent = null;
                hostParentIsContainer = false;
              } else commitDeletionEffectsOnFiber(root2, returnFiber, childToDelete);
              root2 = childToDelete.alternate;
              null !== root2 && (root2.return = null);
              childToDelete.return = null;
            }
          if (parentFiber.subtreeFlags & 13886)
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              commitMutationEffectsOnFiber(parentFiber, root$jscomp$0, lanes), parentFiber = parentFiber.sibling;
        }
        function commitMutationEffectsOnFiber(finishedWork, root2, lanes) {
          var current = finishedWork.alternate, flags = finishedWork.flags;
          switch (finishedWork.tag) {
            case 0:
            case 11:
            case 14:
            case 15:
              if (flags & 4 && (current = finishedWork.updateQueue, current = null !== current ? current.events : null, null !== current))
                for (var ii = 0; ii < current.length; ii++) {
                  var _eventPayloads$ii2 = current[ii];
                  _eventPayloads$ii2.ref.impl = _eventPayloads$ii2.nextImpl;
                }
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              flags & 4 && (commitHookEffectListUnmount(3, finishedWork, finishedWork.return), commitHookEffectListMount(3, finishedWork), commitHookEffectListUnmount(5, finishedWork, finishedWork.return));
              break;
            case 1:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              flags & 512 && (offscreenSubtreeWasHidden || null === current || safelyDetachRef(current, current.return));
              flags & 64 && offscreenSubtreeIsHidden && (finishedWork = finishedWork.updateQueue, null !== finishedWork && (root2 = finishedWork.callbacks, null !== root2 && (flags = finishedWork.shared.hiddenCallbacks, finishedWork.shared.hiddenCallbacks = null === flags ? root2 : flags.concat(root2))));
              break;
            case 26:
              if (supportsResources) {
                ii = currentHoistableRoot;
                recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
                commitReconciliationEffects(finishedWork);
                flags & 512 && (offscreenSubtreeWasHidden || null === current || safelyDetachRef(current, current.return));
                flags & 4 && (lanes = null !== current ? current.memoizedState : null, flags = finishedWork.memoizedState, null === current ? null === flags ? null === finishedWork.stateNode ? finishedWork.stateNode = offscreenSubtreeIsHidden ? createHoistableInstance(
                  finishedWork.type,
                  finishedWork.memoizedProps,
                  root2.containerInfo,
                  finishedWork
                ) : hydrateHoistable(
                  ii,
                  finishedWork.type,
                  finishedWork.memoizedProps,
                  finishedWork
                ) : offscreenSubtreeIsHidden || mountHoistable(
                  ii,
                  finishedWork.type,
                  finishedWork.stateNode
                ) : finishedWork.stateNode = acquireResource(
                  ii,
                  flags,
                  finishedWork.memoizedProps
                ) : lanes !== flags ? (null === lanes ? (root2 = current.stateNode, null === root2 || offscreenSubtreeWasHidden || unmountHoistable(root2)) : releaseResource(lanes), null === flags ? offscreenSubtreeIsHidden || mountHoistable(
                  ii,
                  finishedWork.type,
                  finishedWork.stateNode
                ) : acquireResource(ii, flags, finishedWork.memoizedProps)) : null === flags && null !== finishedWork.stateNode && commitHostUpdate(
                  finishedWork,
                  finishedWork.memoizedProps,
                  current.memoizedProps
                ));
                break;
              }
            case 27:
              if (supportsSingletons) {
                recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
                commitReconciliationEffects(finishedWork);
                flags & 512 && (offscreenSubtreeWasHidden || null === current || safelyDetachRef(current, current.return));
                null !== current && flags & 4 && commitHostUpdate(
                  finishedWork,
                  finishedWork.memoizedProps,
                  current.memoizedProps
                );
                break;
              }
            case 5:
              ii = offscreenDirectParentIsHidden;
              offscreenDirectParentIsHidden = false;
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              offscreenDirectParentIsHidden = ii;
              commitReconciliationEffects(finishedWork);
              flags & 512 && (offscreenSubtreeWasHidden || null === current || safelyDetachRef(current, current.return));
              if (supportsMutation) {
                if (finishedWork.flags & 32) {
                  root2 = finishedWork.stateNode;
                  try {
                    resetTextContent(root2), viewTransitionMutationContext = true;
                  } catch (error) {
                    captureCommitPhaseError(finishedWork, finishedWork.return, error);
                  }
                }
                flags & 4 && null != finishedWork.stateNode && (root2 = finishedWork.memoizedProps, commitHostUpdate(
                  finishedWork,
                  root2,
                  null !== current ? current.memoizedProps : root2
                ));
                flags & 1024 && (needsFormReset = true);
              } else
                supportsPersistence && null !== finishedWork.alternate && (finishedWork.alternate.stateNode = finishedWork.stateNode);
              break;
            case 6:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              if (flags & 4 && supportsMutation) {
                if (null === finishedWork.stateNode)
                  throw Error(formatProdErrorMessage(162));
                root2 = finishedWork.memoizedProps;
                flags = null !== current ? current.memoizedProps : root2;
                lanes = finishedWork.stateNode;
                try {
                  commitTextUpdate(lanes, flags, root2), viewTransitionMutationContext = true;
                } catch (error) {
                  captureCommitPhaseError(finishedWork, finishedWork.return, error);
                }
              }
              break;
            case 3:
              viewTransitionMutationContext = false;
              supportsResources ? (prepareToCommitHoistables(), ii = currentHoistableRoot, currentHoistableRoot = getHoistableRoot(root2.containerInfo), recursivelyTraverseMutationEffects(root2, finishedWork, lanes), currentHoistableRoot = ii) : recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              if (flags & 4) {
                if (supportsMutation && supportsHydration && null !== current && current.memoizedState.isDehydrated)
                  try {
                    commitHydratedContainer(root2.containerInfo);
                  } catch (error) {
                    captureCommitPhaseError(finishedWork, finishedWork.return, error);
                  }
                if (supportsPersistence) {
                  flags = root2.containerInfo;
                  root2 = root2.pendingChildren;
                  try {
                    replaceContainerChildren(flags, root2), viewTransitionMutationContext = true;
                  } catch (error) {
                    captureCommitPhaseError(finishedWork, finishedWork.return, error);
                  }
                }
              }
              needsFormReset && (needsFormReset = false, recursivelyResetForms(finishedWork));
              viewTransitionMutationContext = false;
              break;
            case 4:
              current = offscreenDirectParentIsHidden;
              offscreenDirectParentIsHidden = offscreenSubtreeIsHidden;
              ii = pushMutationContext();
              supportsResources ? (_eventPayloads$ii2 = currentHoistableRoot, currentHoistableRoot = getHoistableRoot(
                finishedWork.stateNode.containerInfo
              ), recursivelyTraverseMutationEffects(root2, finishedWork, lanes), commitReconciliationEffects(finishedWork), currentHoistableRoot = _eventPayloads$ii2) : (recursivelyTraverseMutationEffects(root2, finishedWork, lanes), commitReconciliationEffects(finishedWork));
              viewTransitionMutationContext && inUpdateViewTransition && (rootViewTransitionAffected = true);
              viewTransitionMutationContext = ii;
              offscreenDirectParentIsHidden = current;
              flags & 4 && supportsPersistence && commitHostPortalContainerChildren(
                finishedWork.stateNode,
                finishedWork,
                finishedWork.stateNode.pendingChildren
              );
              break;
            case 12:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              break;
            case 31:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              flags & 4 && (root2 = finishedWork.updateQueue, null !== root2 && (finishedWork.updateQueue = null, attachSuspenseRetryListeners(finishedWork, root2)));
              break;
            case 13:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              finishedWork.child.flags & 8192 && null !== finishedWork.memoizedState !== (null !== current && null !== current.memoizedState) && (globalMostRecentFallbackTime = now2());
              flags & 4 && (root2 = finishedWork.updateQueue, null !== root2 && (finishedWork.updateQueue = null, attachSuspenseRetryListeners(finishedWork, root2)));
              break;
            case 22:
              ii = null !== finishedWork.memoizedState;
              _eventPayloads$ii2 = null !== current && null !== current.memoizedState;
              var prevOffscreenSubtreeIsHidden = offscreenSubtreeIsHidden, prevOffscreenSubtreeWasHidden = offscreenSubtreeWasHidden, prevOffscreenDirectParentIsHidden$147 = offscreenDirectParentIsHidden;
              offscreenSubtreeIsHidden = prevOffscreenSubtreeIsHidden || ii;
              offscreenDirectParentIsHidden = prevOffscreenDirectParentIsHidden$147 || ii;
              offscreenSubtreeWasHidden = prevOffscreenSubtreeWasHidden || _eventPayloads$ii2;
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              offscreenSubtreeWasHidden = prevOffscreenSubtreeWasHidden;
              offscreenDirectParentIsHidden = prevOffscreenDirectParentIsHidden$147;
              offscreenSubtreeIsHidden = prevOffscreenSubtreeIsHidden;
              commitReconciliationEffects(finishedWork);
              flags & 8192 && (root2 = finishedWork.stateNode, root2._visibility = ii ? root2._visibility & -2 : root2._visibility | 1, !ii || null === current || _eventPayloads$ii2 || offscreenSubtreeIsHidden || offscreenSubtreeWasHidden || (root2 = supportsSingletons ? 2 : 0, lanes = _eventPayloads$ii2 || offscreenSubtreeWasHidden, current = offscreenSubtreeIsHidden, _eventPayloads$ii2 = offscreenSubtreeWasHidden, offscreenSubtreeIsHidden = ii || offscreenSubtreeIsHidden, offscreenSubtreeWasHidden = lanes, recursivelyTraverseDisappearLayoutEffects(finishedWork, root2), offscreenSubtreeIsHidden = current, offscreenSubtreeWasHidden = _eventPayloads$ii2), supportsMutation && (ii || !offscreenDirectParentIsHidden) && hideOrUnhideAllChildren(finishedWork, ii));
              flags & 4 && (root2 = finishedWork.updateQueue, null !== root2 && (flags = root2.retryQueue, null !== flags && (root2.retryQueue = null, attachSuspenseRetryListeners(finishedWork, flags))));
              break;
            case 19:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              flags & 4 && (root2 = finishedWork.updateQueue, null !== root2 && (finishedWork.updateQueue = null, attachSuspenseRetryListeners(finishedWork, root2)));
              break;
            case 30:
              flags & 512 && (offscreenSubtreeWasHidden || null === current || safelyDetachRef(current, current.return));
              flags = pushMutationContext();
              ii = inUpdateViewTransition;
              _eventPayloads$ii2 = (lanes & 335544064) === lanes;
              prevOffscreenSubtreeIsHidden = finishedWork.memoizedProps;
              inUpdateViewTransition = _eventPayloads$ii2 && "none" !== getViewTransitionClassName(
                prevOffscreenSubtreeIsHidden.default,
                prevOffscreenSubtreeIsHidden.update
              );
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes);
              commitReconciliationEffects(finishedWork);
              _eventPayloads$ii2 && null !== current && viewTransitionMutationContext && (finishedWork.flags |= 4);
              inUpdateViewTransition = ii;
              viewTransitionMutationContext = flags;
              break;
            case 21:
              break;
            case 7:
              flags & 512 && (offscreenSubtreeWasHidden || null === current || safelyDetachRef(current, current.return)), current && null !== current.stateNode && updateFragmentInstanceFiber(finishedWork, current.stateNode);
            default:
              recursivelyTraverseMutationEffects(root2, finishedWork, lanes), commitReconciliationEffects(finishedWork);
          }
        }
        function commitReconciliationEffects(finishedWork) {
          var flags = finishedWork.flags;
          if (flags & 2) {
            try {
              for (var hostParentFiber, parentFiber = finishedWork.return; null !== parentFiber; ) {
                if (isHostParent(parentFiber)) {
                  hostParentFiber = parentFiber;
                  break;
                }
                parentFiber = parentFiber.return;
              }
              parentFiber = null;
              for (var parent = finishedWork.return; null !== parent; ) {
                if (isFragmentInstanceParent(parent)) {
                  var fragmentInstance = parent.stateNode;
                  null === parentFiber ? parentFiber = [fragmentInstance] : parentFiber.push(fragmentInstance);
                }
                if (isFragmentInstanceHostBoundary(parent)) break;
                parent = parent.return;
              }
              var JSCompiler_inline_result = parentFiber;
              if (supportsMutation) {
                if (null == hostParentFiber) throw Error(formatProdErrorMessage(160));
                switch (hostParentFiber.tag) {
                  case 27:
                    if (supportsSingletons) {
                      var parent$jscomp$0 = hostParentFiber.stateNode, before = getHostSibling(finishedWork);
                      insertOrAppendPlacementNode(
                        finishedWork,
                        before,
                        parent$jscomp$0,
                        JSCompiler_inline_result
                      );
                      break;
                    }
                  case 5:
                    var parent$131 = hostParentFiber.stateNode;
                    hostParentFiber.flags & 32 && (resetTextContent(parent$131), hostParentFiber.flags &= -33);
                    var before$132 = getHostSibling(finishedWork);
                    insertOrAppendPlacementNode(
                      finishedWork,
                      before$132,
                      parent$131,
                      JSCompiler_inline_result
                    );
                    break;
                  case 3:
                  case 4:
                    var parent$133 = hostParentFiber.stateNode.containerInfo, before$134 = getHostSibling(finishedWork);
                    insertOrAppendPlacementNodeIntoContainer(
                      finishedWork,
                      before$134,
                      parent$133,
                      JSCompiler_inline_result
                    );
                    break;
                  default:
                    throw Error(formatProdErrorMessage(161));
                }
              } else
                commitImmutablePlacementNodeToFragmentInstances(
                  finishedWork,
                  JSCompiler_inline_result
                );
            } catch (error) {
              captureCommitPhaseError(finishedWork, finishedWork.return, error);
            }
            finishedWork.flags &= -3;
          }
          flags & 4096 && (finishedWork.flags &= -4097);
        }
        function recursivelyResetForms(parentFiber) {
          if (parentFiber.subtreeFlags & 1024)
            for (parentFiber = parentFiber.child; null !== parentFiber; ) {
              var fiber = parentFiber;
              recursivelyResetForms(fiber);
              5 === fiber.tag && fiber.flags & 1024 && resetFormInstance(fiber.stateNode);
              parentFiber = parentFiber.sibling;
            }
        }
        function recursivelyTraverseAfterMutationEffects(root2, parentFiber) {
          if (parentFiber.subtreeFlags & 9270)
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              commitAfterMutationEffectsOnFiber(parentFiber, root2), parentFiber = parentFiber.sibling;
          else measureNestedViewTransitions(parentFiber, false);
        }
        function commitAfterMutationEffectsOnFiber(finishedWork, root2) {
          var current = finishedWork.alternate;
          if (null === current) commitEnterViewTransitions(finishedWork, false);
          else
            switch (finishedWork.tag) {
              case 3:
                rootViewTransitionNameCanceled = viewTransitionContextChanged = false;
                pushViewTransitionCancelableScope();
                recursivelyTraverseAfterMutationEffects(root2, finishedWork);
                if (!viewTransitionContextChanged && !rootViewTransitionAffected) {
                  finishedWork = viewTransitionCancelableChildren;
                  if (null !== finishedWork)
                    for (var i = 0; i < finishedWork.length; i += 3)
                      cancelViewTransitionName(
                        finishedWork[i],
                        finishedWork[i + 1],
                        finishedWork[i + 2]
                      );
                  cancelRootViewTransitionName(root2.containerInfo);
                  rootViewTransitionNameCanceled = true;
                }
                viewTransitionCancelableChildren = null;
                break;
              case 5:
                recursivelyTraverseAfterMutationEffects(root2, finishedWork);
                break;
              case 4:
                i = viewTransitionContextChanged;
                viewTransitionContextChanged = false;
                recursivelyTraverseAfterMutationEffects(root2, finishedWork);
                viewTransitionContextChanged && (rootViewTransitionAffected = true);
                viewTransitionContextChanged = i;
                break;
              case 22:
                null === finishedWork.memoizedState && (null !== current.memoizedState ? commitEnterViewTransitions(finishedWork, false) : recursivelyTraverseAfterMutationEffects(root2, finishedWork));
                break;
              case 30:
                i = viewTransitionContextChanged;
                var prevCancelableChildren = pushViewTransitionCancelableScope();
                viewTransitionContextChanged = false;
                recursivelyTraverseAfterMutationEffects(root2, finishedWork);
                viewTransitionContextChanged && (finishedWork.flags |= 4);
                var props = finishedWork.memoizedProps, state = finishedWork.stateNode;
                root2 = getViewTransitionName(props, state);
                state = getViewTransitionName(current.memoizedProps, state);
                var className = getViewTransitionClassName(
                  props.default,
                  props.update
                );
                "none" === className ? current = false : (props = current.memoizedState, current.memoizedState = null, current = finishedWork.child, viewTransitionHostInstanceIdx = 0, current = measureViewTransitionHostInstancesRecursive(
                  finishedWork,
                  current,
                  root2,
                  state,
                  className,
                  props,
                  true
                ), viewTransitionHostInstanceIdx !== (null === props ? 0 : props.length) && (finishedWork.flags |= 32));
                0 !== (finishedWork.flags & 4) && current ? (scheduleViewTransitionEvent(
                  finishedWork,
                  finishedWork.memoizedProps.onUpdate
                ), viewTransitionCancelableChildren = prevCancelableChildren) : null !== prevCancelableChildren && (prevCancelableChildren.push.apply(
                  prevCancelableChildren,
                  viewTransitionCancelableChildren
                ), viewTransitionCancelableChildren = prevCancelableChildren);
                viewTransitionContextChanged = 0 !== (finishedWork.flags & 32) ? true : i;
                break;
              default:
                recursivelyTraverseAfterMutationEffects(root2, finishedWork);
            }
        }
        function recursivelyTraverseLayoutEffects(root2, parentFiber) {
          if (parentFiber.subtreeFlags & 8772)
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              commitLayoutEffectOnFiber(root2, parentFiber.alternate, parentFiber), parentFiber = parentFiber.sibling;
        }
        function recursivelyTraverseDisappearLayoutEffects(parentFiber, layoutEffectTraversalFlags$jscomp$0) {
          for (parentFiber = parentFiber.child; null !== parentFiber; ) {
            var finishedWork = parentFiber, layoutEffectTraversalFlags = layoutEffectTraversalFlags$jscomp$0;
            switch (finishedWork.tag) {
              case 0:
              case 11:
              case 14:
              case 15:
                commitHookEffectListUnmount(4, finishedWork, finishedWork.return);
                recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 1:
                safelyDetachRef(finishedWork, finishedWork.return);
                var instance = finishedWork.stateNode;
                "function" === typeof instance.componentWillUnmount && safelyCallComponentWillUnmount(
                  finishedWork,
                  finishedWork.return,
                  instance
                );
                recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 27:
                supportsSingletons && 0 !== (layoutEffectTraversalFlags & 2) && releaseSingletonInstance(
                  finishedWork.stateNode,
                  finishedWork.type,
                  finishedWork.memoizedProps
                );
              case 5:
                safelyDetachRef(finishedWork, finishedWork.return);
                5 !== finishedWork.tag && 27 !== finishedWork.tag || commitFragmentInstanceDeletionEffects(finishedWork);
                recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 6:
                commitFragmentInstanceDeletionEffects(finishedWork);
                break;
              case 26:
                safelyDetachRef(finishedWork, finishedWork.return);
                supportsResources && (instance = finishedWork.stateNode, null !== finishedWork.memoizedState || null === instance || offscreenSubtreeWasHidden || unmountHoistable(instance));
                recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 22:
                null === finishedWork.memoizedState && recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 30:
                safelyDetachRef(finishedWork, finishedWork.return);
                recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 7:
                safelyDetachRef(finishedWork, finishedWork.return);
              default:
                recursivelyTraverseDisappearLayoutEffects(
                  finishedWork,
                  layoutEffectTraversalFlags
                );
            }
            parentFiber = parentFiber.sibling;
          }
        }
        function recursivelyTraverseReappearLayoutEffects(finishedRoot$jscomp$0, parentFiber, layoutEffectTraversalFlags) {
          layoutEffectTraversalFlags = 0 !== (parentFiber.subtreeFlags & 8772) ? layoutEffectTraversalFlags : layoutEffectTraversalFlags & -2;
          for (parentFiber = parentFiber.child; null !== parentFiber; ) {
            var current = parentFiber.alternate, finishedRoot = finishedRoot$jscomp$0, finishedWork = parentFiber, flags = finishedWork.flags, includeWorkInProgressEffects = 0 !== (layoutEffectTraversalFlags & 1);
            switch (finishedWork.tag) {
              case 0:
              case 11:
              case 15:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                commitHookEffectListMount(4, finishedWork);
                break;
              case 1:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                current = finishedWork;
                finishedRoot = current.stateNode;
                if ("function" === typeof finishedRoot.componentDidMount)
                  try {
                    finishedRoot.componentDidMount();
                  } catch (error) {
                    captureCommitPhaseError(current, current.return, error);
                  }
                current = finishedWork;
                finishedRoot = current.updateQueue;
                if (null !== finishedRoot) {
                  var instance = current.stateNode;
                  try {
                    var hiddenCallbacks = finishedRoot.shared.hiddenCallbacks;
                    if (null !== hiddenCallbacks)
                      for (finishedRoot.shared.hiddenCallbacks = null, finishedRoot = 0; finishedRoot < hiddenCallbacks.length; finishedRoot++)
                        callCallback(hiddenCallbacks[finishedRoot], instance);
                  } catch (error) {
                    captureCommitPhaseError(current, current.return, error);
                  }
                }
                includeWorkInProgressEffects && flags & 64 && commitClassCallbacks(finishedWork);
                safelyAttachRef(finishedWork, finishedWork.return);
                break;
              case 27:
                supportsSingletons && 0 !== (layoutEffectTraversalFlags & 2) && commitHostSingletonAcquisition(finishedWork);
              case 5:
                5 !== finishedWork.tag && 27 !== finishedWork.tag || commitFragmentInstanceInsertionEffects(finishedWork);
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                includeWorkInProgressEffects && null === current && flags & 4 && commitHostMount(finishedWork);
                safelyAttachRef(finishedWork, finishedWork.return);
                break;
              case 6:
                commitFragmentInstanceInsertionEffects(finishedWork);
                break;
              case 26:
                supportsResources && (instance = finishedWork.stateNode, null !== finishedWork.memoizedState || null === instance || offscreenSubtreeIsHidden || mountHoistable(
                  getHoistableRoot(instance.ownerDocument),
                  finishedWork.type,
                  instance
                ));
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                includeWorkInProgressEffects && null === current && flags & 4 && commitHostMount(finishedWork);
                safelyAttachRef(finishedWork, finishedWork.return);
                break;
              case 12:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                break;
              case 31:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                includeWorkInProgressEffects && flags & 4 && commitActivityHydrationCallbacks(finishedRoot, finishedWork);
                break;
              case 13:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                includeWorkInProgressEffects && flags & 4 && commitSuspenseHydrationCallbacks(finishedRoot, finishedWork);
                break;
              case 22:
                null === finishedWork.memoizedState && recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                safelyAttachRef(finishedWork, finishedWork.return);
                break;
              case 30:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
                safelyAttachRef(finishedWork, finishedWork.return);
                break;
              case 7:
                safelyAttachRef(finishedWork, finishedWork.return);
              default:
                recursivelyTraverseReappearLayoutEffects(
                  finishedRoot,
                  finishedWork,
                  layoutEffectTraversalFlags
                );
            }
            parentFiber = parentFiber.sibling;
          }
        }
        function commitOffscreenPassiveMountEffects(current, finishedWork) {
          var previousCache = null;
          null !== current && null !== current.memoizedState && null !== current.memoizedState.cachePool && (previousCache = current.memoizedState.cachePool.pool);
          current = null;
          null !== finishedWork.memoizedState && null !== finishedWork.memoizedState.cachePool && (current = finishedWork.memoizedState.cachePool.pool);
          current !== previousCache && (null != current && current.refCount++, null != previousCache && releaseCache(previousCache));
        }
        function commitCachePassiveMountEffect(current, finishedWork) {
          current = null;
          null !== finishedWork.alternate && (current = finishedWork.alternate.memoizedState.cache);
          finishedWork = finishedWork.memoizedState.cache;
          finishedWork !== current && (finishedWork.refCount++, null != current && releaseCache(current));
        }
        function recursivelyTraversePassiveMountEffects(root2, parentFiber, committedLanes, committedTransitions) {
          var isViewTransitionEligible = (committedLanes & 335544064) === committedLanes;
          if (parentFiber.subtreeFlags & (isViewTransitionEligible ? 10262 : 10256))
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              commitPassiveMountOnFiber(
                root2,
                parentFiber,
                committedLanes,
                committedTransitions
              ), parentFiber = parentFiber.sibling;
          else isViewTransitionEligible && restoreNestedViewTransitions(parentFiber);
        }
        function commitPassiveMountOnFiber(finishedRoot, finishedWork, committedLanes, committedTransitions) {
          var isViewTransitionEligible = (committedLanes & 335544064) === committedLanes;
          isViewTransitionEligible && null === finishedWork.alternate && null !== finishedWork.return && null !== finishedWork.return.alternate && restoreEnterOrExitViewTransitions(finishedWork);
          var flags = finishedWork.flags;
          switch (finishedWork.tag) {
            case 0:
            case 11:
            case 15:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              flags & 2048 && commitHookEffectListMount(9, finishedWork);
              break;
            case 1:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              break;
            case 3:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              isViewTransitionEligible && supportsMutation && rootViewTransitionNameCanceled && restoreRootViewTransitionName(finishedRoot.containerInfo);
              flags & 2048 && (finishedRoot = null, null !== finishedWork.alternate && (finishedRoot = finishedWork.alternate.memoizedState.cache), finishedWork = finishedWork.memoizedState.cache, finishedWork !== finishedRoot && (finishedWork.refCount++, null != finishedRoot && releaseCache(finishedRoot)));
              break;
            case 12:
              if (flags & 2048) {
                recursivelyTraversePassiveMountEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions
                );
                finishedRoot = finishedWork.stateNode;
                try {
                  var _finishedWork$memoize2 = finishedWork.memoizedProps, id = _finishedWork$memoize2.id, onPostCommit = _finishedWork$memoize2.onPostCommit;
                  "function" === typeof onPostCommit && onPostCommit(
                    id,
                    null === finishedWork.alternate ? "mount" : "update",
                    finishedRoot.passiveEffectDuration,
                    -0
                  );
                } catch (error) {
                  captureCommitPhaseError(finishedWork, finishedWork.return, error);
                }
              } else
                recursivelyTraversePassiveMountEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions
                );
              break;
            case 31:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              break;
            case 13:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              break;
            case 23:
              break;
            case 22:
              _finishedWork$memoize2 = finishedWork.stateNode;
              id = finishedWork.alternate;
              null !== finishedWork.memoizedState ? (isViewTransitionEligible && null !== id && null === id.memoizedState && restoreEnterOrExitViewTransitions(id), _finishedWork$memoize2._visibility & 2 ? recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              ) : recursivelyTraverseAtomicPassiveEffects(
                finishedRoot,
                finishedWork
              )) : (isViewTransitionEligible && null !== id && null !== id.memoizedState && restoreEnterOrExitViewTransitions(finishedWork), _finishedWork$memoize2._visibility & 2 ? recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              ) : (_finishedWork$memoize2._visibility |= 2, recursivelyTraverseReconnectPassiveEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions,
                0 !== (finishedWork.subtreeFlags & 10256) || false
              )));
              flags & 2048 && commitOffscreenPassiveMountEffects(id, finishedWork);
              break;
            case 24:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              flags & 2048 && commitCachePassiveMountEffect(finishedWork.alternate, finishedWork);
              break;
            case 30:
              isViewTransitionEligible && (isViewTransitionEligible = finishedWork.alternate, null !== isViewTransitionEligible && (restoreViewTransitionOnHostInstances(
                isViewTransitionEligible.child,
                true
              ), restoreViewTransitionOnHostInstances(finishedWork.child, true)));
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
              break;
            default:
              recursivelyTraversePassiveMountEffects(
                finishedRoot,
                finishedWork,
                committedLanes,
                committedTransitions
              );
          }
        }
        function recursivelyTraverseReconnectPassiveEffects(finishedRoot$jscomp$0, parentFiber, committedLanes$jscomp$0, committedTransitions$jscomp$0, includeWorkInProgressEffects) {
          includeWorkInProgressEffects = includeWorkInProgressEffects && (0 !== (parentFiber.subtreeFlags & 10256) || false);
          for (parentFiber = parentFiber.child; null !== parentFiber; ) {
            var finishedRoot = finishedRoot$jscomp$0, finishedWork = parentFiber, committedLanes = committedLanes$jscomp$0, committedTransitions = committedTransitions$jscomp$0, flags = finishedWork.flags;
            switch (finishedWork.tag) {
              case 0:
              case 11:
              case 15:
                recursivelyTraverseReconnectPassiveEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions,
                  includeWorkInProgressEffects
                );
                commitHookEffectListMount(8, finishedWork);
                break;
              case 23:
                break;
              case 22:
                var instance = finishedWork.stateNode;
                null !== finishedWork.memoizedState ? instance._visibility & 2 ? recursivelyTraverseReconnectPassiveEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions,
                  includeWorkInProgressEffects
                ) : recursivelyTraverseAtomicPassiveEffects(
                  finishedRoot,
                  finishedWork
                ) : (instance._visibility |= 2, recursivelyTraverseReconnectPassiveEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions,
                  includeWorkInProgressEffects
                ));
                includeWorkInProgressEffects && flags & 2048 && commitOffscreenPassiveMountEffects(
                  finishedWork.alternate,
                  finishedWork
                );
                break;
              case 24:
                recursivelyTraverseReconnectPassiveEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions,
                  includeWorkInProgressEffects
                );
                includeWorkInProgressEffects && flags & 2048 && commitCachePassiveMountEffect(finishedWork.alternate, finishedWork);
                break;
              default:
                recursivelyTraverseReconnectPassiveEffects(
                  finishedRoot,
                  finishedWork,
                  committedLanes,
                  committedTransitions,
                  includeWorkInProgressEffects
                );
            }
            parentFiber = parentFiber.sibling;
          }
        }
        function recursivelyTraverseAtomicPassiveEffects(finishedRoot$jscomp$0, parentFiber) {
          if (parentFiber.subtreeFlags & 10256)
            for (parentFiber = parentFiber.child; null !== parentFiber; ) {
              var finishedRoot = finishedRoot$jscomp$0, finishedWork = parentFiber, flags = finishedWork.flags;
              switch (finishedWork.tag) {
                case 22:
                  recursivelyTraverseAtomicPassiveEffects(finishedRoot, finishedWork);
                  flags & 2048 && commitOffscreenPassiveMountEffects(
                    finishedWork.alternate,
                    finishedWork
                  );
                  break;
                case 24:
                  recursivelyTraverseAtomicPassiveEffects(finishedRoot, finishedWork);
                  flags & 2048 && commitCachePassiveMountEffect(
                    finishedWork.alternate,
                    finishedWork
                  );
                  break;
                default:
                  recursivelyTraverseAtomicPassiveEffects(finishedRoot, finishedWork);
              }
              parentFiber = parentFiber.sibling;
            }
        }
        function recursivelyAccumulateSuspenseyCommit(parentFiber, committedLanes, suspendedState) {
          if (parentFiber.subtreeFlags & suspenseyCommitFlag)
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              accumulateSuspenseyCommitOnFiber(
                parentFiber,
                committedLanes,
                suspendedState
              ), parentFiber = parentFiber.sibling;
        }
        function accumulateSuspenseyCommitOnFiber(fiber, committedLanes, suspendedState) {
          switch (fiber.tag) {
            case 26:
              recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              );
              if (fiber.flags & suspenseyCommitFlag)
                if (null !== fiber.memoizedState)
                  suspendResource(
                    suspendedState,
                    currentHoistableRoot,
                    fiber.memoizedState,
                    fiber.memoizedProps
                  );
                else {
                  var instance = fiber.stateNode, type = fiber.type;
                  fiber = fiber.memoizedProps;
                  ((committedLanes & 335544128) === committedLanes || maySuspendCommitInSyncRender(type, fiber)) && suspendInstance(suspendedState, instance, type, fiber);
                }
              break;
            case 5:
              recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              );
              fiber.flags & suspenseyCommitFlag && (instance = fiber.stateNode, type = fiber.type, fiber = fiber.memoizedProps, ((committedLanes & 335544128) === committedLanes || maySuspendCommitInSyncRender(type, fiber)) && suspendInstance(suspendedState, instance, type, fiber));
              break;
            case 3:
            case 4:
              supportsResources ? (instance = currentHoistableRoot, currentHoistableRoot = getHoistableRoot(
                fiber.stateNode.containerInfo
              ), recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              ), currentHoistableRoot = instance) : recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              );
              break;
            case 22:
              null === fiber.memoizedState && (instance = fiber.alternate, null !== instance && null !== instance.memoizedState ? (instance = suspenseyCommitFlag, suspenseyCommitFlag = 16777216, recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              ), suspenseyCommitFlag = instance) : recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              ));
              break;
            case 30:
              0 !== (fiber.flags & suspenseyCommitFlag) && (instance = fiber.memoizedProps.name, null != instance && "auto" !== instance && (type = fiber.stateNode, type.paired = null, null === appearingViewTransitions && (appearingViewTransitions = /* @__PURE__ */ new Map()), appearingViewTransitions.set(instance, type)));
              recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              );
              break;
            default:
              recursivelyAccumulateSuspenseyCommit(
                fiber,
                committedLanes,
                suspendedState
              );
          }
        }
        function detachAlternateSiblings(parentFiber) {
          var previousFiber = parentFiber.alternate;
          if (null !== previousFiber && (parentFiber = previousFiber.child, null !== parentFiber)) {
            previousFiber.child = null;
            do
              previousFiber = parentFiber.sibling, parentFiber.sibling = null, parentFiber = previousFiber;
            while (null !== parentFiber);
          }
        }
        function recursivelyTraversePassiveUnmountEffects(parentFiber) {
          var deletions = parentFiber.deletions;
          if (0 !== (parentFiber.flags & 16)) {
            if (null !== deletions)
              for (var i = 0; i < deletions.length; i++) {
                var childToDelete = deletions[i];
                nextEffect = childToDelete;
                commitPassiveUnmountEffectsInsideOfDeletedTree_begin(
                  childToDelete,
                  parentFiber
                );
              }
            detachAlternateSiblings(parentFiber);
          }
          if (parentFiber.subtreeFlags & 10256)
            for (parentFiber = parentFiber.child; null !== parentFiber; )
              commitPassiveUnmountOnFiber(parentFiber), parentFiber = parentFiber.sibling;
        }
        function commitPassiveUnmountOnFiber(finishedWork) {
          switch (finishedWork.tag) {
            case 0:
            case 11:
            case 15:
              recursivelyTraversePassiveUnmountEffects(finishedWork);
              finishedWork.flags & 2048 && commitHookEffectListUnmount(9, finishedWork, finishedWork.return);
              break;
            case 3:
              recursivelyTraversePassiveUnmountEffects(finishedWork);
              break;
            case 12:
              recursivelyTraversePassiveUnmountEffects(finishedWork);
              break;
            case 22:
              var instance = finishedWork.stateNode;
              null !== finishedWork.memoizedState && instance._visibility & 2 && (null === finishedWork.return || 13 !== finishedWork.return.tag) ? (instance._visibility &= -3, recursivelyTraverseDisconnectPassiveEffects(finishedWork)) : recursivelyTraversePassiveUnmountEffects(finishedWork);
              break;
            default:
              recursivelyTraversePassiveUnmountEffects(finishedWork);
          }
        }
        function recursivelyTraverseDisconnectPassiveEffects(parentFiber) {
          var deletions = parentFiber.deletions;
          if (0 !== (parentFiber.flags & 16)) {
            if (null !== deletions)
              for (var i = 0; i < deletions.length; i++) {
                var childToDelete = deletions[i];
                nextEffect = childToDelete;
                commitPassiveUnmountEffectsInsideOfDeletedTree_begin(
                  childToDelete,
                  parentFiber
                );
              }
            detachAlternateSiblings(parentFiber);
          }
          for (parentFiber = parentFiber.child; null !== parentFiber; ) {
            deletions = parentFiber;
            switch (deletions.tag) {
              case 0:
              case 11:
              case 15:
                commitHookEffectListUnmount(8, deletions, deletions.return);
                recursivelyTraverseDisconnectPassiveEffects(deletions);
                break;
              case 22:
                i = deletions.stateNode;
                i._visibility & 2 && (i._visibility &= -3, recursivelyTraverseDisconnectPassiveEffects(deletions));
                break;
              default:
                recursivelyTraverseDisconnectPassiveEffects(deletions);
            }
            parentFiber = parentFiber.sibling;
          }
        }
        function commitPassiveUnmountEffectsInsideOfDeletedTree_begin(deletedSubtreeRoot, nearestMountedAncestor) {
          for (; null !== nextEffect; ) {
            var fiber = nextEffect;
            switch (fiber.tag) {
              case 0:
              case 11:
              case 15:
                commitHookEffectListUnmount(8, fiber, nearestMountedAncestor);
                break;
              case 23:
              case 22:
                if (null !== fiber.memoizedState && null !== fiber.memoizedState.cachePool) {
                  var cache = fiber.memoizedState.cachePool.pool;
                  null != cache && cache.refCount++;
                }
                break;
              case 24:
                releaseCache(fiber.memoizedState.cache);
            }
            cache = fiber.child;
            if (null !== cache) cache.return = fiber, nextEffect = cache;
            else
              a: for (fiber = deletedSubtreeRoot; null !== nextEffect; ) {
                cache = nextEffect;
                var sibling = cache.sibling, returnFiber = cache.return;
                detachFiberAfterEffects(cache);
                if (cache === fiber) {
                  nextEffect = null;
                  break a;
                }
                if (null !== sibling) {
                  sibling.return = returnFiber;
                  nextEffect = sibling;
                  break a;
                }
                nextEffect = returnFiber;
              }
          }
        }
        function findFiberRootForHostRoot(hostRoot) {
          var maybeFiber = getInstanceFromNode(hostRoot);
          if (null != maybeFiber) {
            if ("string" !== typeof maybeFiber.memoizedProps["data-testname"])
              throw Error(formatProdErrorMessage(364));
            return maybeFiber;
          }
          hostRoot = findFiberRoot(hostRoot);
          if (null === hostRoot) throw Error(formatProdErrorMessage(362));
          return hostRoot.stateNode.current;
        }
        function matchSelector(fiber$jscomp$0, selector) {
          var tag = fiber$jscomp$0.tag;
          switch (selector.$$typeof) {
            case COMPONENT_TYPE:
              if (fiber$jscomp$0.type === selector.value) return true;
              break;
            case HAS_PSEUDO_CLASS_TYPE:
              a: {
                selector = selector.value;
                fiber$jscomp$0 = [fiber$jscomp$0, 0];
                for (tag = 0; tag < fiber$jscomp$0.length; ) {
                  var fiber = fiber$jscomp$0[tag++], tag$jscomp$0 = fiber.tag, selectorIndex = fiber$jscomp$0[tag++], selector$jscomp$0 = selector[selectorIndex];
                  if (5 !== tag$jscomp$0 && 26 !== tag$jscomp$0 && 27 !== tag$jscomp$0 || !isHiddenSubtree(fiber)) {
                    for (; null != selector$jscomp$0 && matchSelector(fiber, selector$jscomp$0); )
                      selectorIndex++, selector$jscomp$0 = selector[selectorIndex];
                    if (selectorIndex === selector.length) {
                      selector = true;
                      break a;
                    } else
                      for (fiber = fiber.child; null !== fiber; )
                        fiber$jscomp$0.push(fiber, selectorIndex), fiber = fiber.sibling;
                  }
                }
                selector = false;
              }
              return selector;
            case ROLE_TYPE:
              if ((5 === tag || 26 === tag || 27 === tag) && matchAccessibilityRole(fiber$jscomp$0.stateNode, selector.value))
                return true;
              break;
            case TEXT_TYPE:
              if (5 === tag || 6 === tag || 26 === tag || 27 === tag) {
                if (fiber$jscomp$0 = getTextContent(fiber$jscomp$0), null !== fiber$jscomp$0 && 0 <= fiber$jscomp$0.indexOf(selector.value))
                  return true;
              }
              break;
            case TEST_NAME_TYPE:
              if (5 === tag || 26 === tag || 27 === tag) {
                if (fiber$jscomp$0 = fiber$jscomp$0.memoizedProps["data-testname"], "string" === typeof fiber$jscomp$0 && fiber$jscomp$0.toLowerCase() === selector.value.toLowerCase())
                  return true;
              }
              break;
            default:
              throw Error(formatProdErrorMessage(365));
          }
          return false;
        }
        function selectorToString(selector) {
          switch (selector.$$typeof) {
            case COMPONENT_TYPE:
              return "<" + (getComponentNameFromType(selector.value) || "Unknown") + ">";
            case HAS_PSEUDO_CLASS_TYPE:
              return ":has(" + (selectorToString(selector) || "") + ")";
            case ROLE_TYPE:
              return '[role="' + selector.value + '"]';
            case TEXT_TYPE:
              return '"' + selector.value + '"';
            case TEST_NAME_TYPE:
              return '[data-testname="' + selector.value + '"]';
            default:
              throw Error(formatProdErrorMessage(365));
          }
        }
        function findPaths(root2, selectors) {
          var matchingFibers = [];
          root2 = [root2, 0];
          for (var index = 0; index < root2.length; ) {
            var fiber = root2[index++], tag = fiber.tag, selectorIndex = root2[index++], selector = selectors[selectorIndex];
            if (5 !== tag && 26 !== tag && 27 !== tag || !isHiddenSubtree(fiber)) {
              for (; null != selector && matchSelector(fiber, selector); )
                selectorIndex++, selector = selectors[selectorIndex];
              if (selectorIndex === selectors.length) matchingFibers.push(fiber);
              else
                for (fiber = fiber.child; null !== fiber; )
                  root2.push(fiber, selectorIndex), fiber = fiber.sibling;
            }
          }
          return matchingFibers;
        }
        function findAllNodes(hostRoot, selectors) {
          if (!supportsTestSelectors) throw Error(formatProdErrorMessage(363));
          hostRoot = findFiberRootForHostRoot(hostRoot);
          hostRoot = findPaths(hostRoot, selectors);
          selectors = [];
          hostRoot = Array.from(hostRoot);
          for (var index = 0; index < hostRoot.length; ) {
            var node = hostRoot[index++], tag = node.tag;
            if (5 === tag || 26 === tag || 27 === tag)
              isHiddenSubtree(node) || selectors.push(node.stateNode);
            else
              for (node = node.child; null !== node; )
                hostRoot.push(node), node = node.sibling;
          }
          return selectors;
        }
        function requestUpdateLane() {
          return 0 !== (executionContext & 2) && 0 !== workInProgressRootRenderLanes ? workInProgressRootRenderLanes & -workInProgressRootRenderLanes : null !== ReactSharedInternals.T ? requestTransitionLane() : resolveUpdatePriority();
        }
        function requestDeferredLane() {
          if (0 === workInProgressDeferredLane)
            if (0 === (workInProgressRootRenderLanes & 536870912) || isHydrating) {
              var lane = nextTransitionDeferredLane;
              nextTransitionDeferredLane <<= 1;
              0 === (nextTransitionDeferredLane & 3932160) && (nextTransitionDeferredLane = 262144);
              workInProgressDeferredLane = lane;
            } else workInProgressDeferredLane = 536870912;
          lane = suspenseHandlerStackCursor.current;
          null !== lane && (lane.flags |= 32);
          return workInProgressDeferredLane;
        }
        function scheduleViewTransitionEvent(fiber, callback) {
          if (null != callback) {
            var state = fiber.stateNode, instance = state.ref;
            null === instance && (instance = state.ref = createViewTransitionInstance(
              getViewTransitionName(fiber.memoizedProps, state)
            ));
            null === pendingViewTransitionEvents && (pendingViewTransitionEvents = []);
            pendingViewTransitionEvents.push(callback.bind(null, instance));
          }
        }
        function scheduleUpdateOnFiber(root2, fiber, lane) {
          if (root2 === workInProgressRoot && (2 === workInProgressSuspendedReason || 9 === workInProgressSuspendedReason) || null !== root2.cancelPendingCommit)
            prepareFreshStack(root2, 0), markRootSuspended(
              root2,
              workInProgressRootRenderLanes,
              workInProgressDeferredLane,
              false
            );
          markRootUpdated$1(root2, lane);
          if (0 === (executionContext & 2) || root2 !== workInProgressRoot)
            root2 === workInProgressRoot && (0 === (executionContext & 2) && (workInProgressRootInterleavedUpdatedLanes |= lane), 4 === workInProgressRootExitStatus && markRootSuspended(
              root2,
              workInProgressRootRenderLanes,
              workInProgressDeferredLane,
              false
            )), ensureRootIsScheduled(root2);
        }
        function performWorkOnRoot(root$jscomp$0, lanes, forceSync) {
          if (0 !== (executionContext & 6)) throw Error(formatProdErrorMessage(327));
          var shouldTimeSlice = !forceSync && 0 === (lanes & 127) && 0 === (lanes & root$jscomp$0.expiredLanes) || checkIfRootIsPrerendering(root$jscomp$0, lanes), exitStatus = shouldTimeSlice ? renderRootConcurrent(root$jscomp$0, lanes) : renderRootSync(root$jscomp$0, lanes, true), renderWasConcurrent = shouldTimeSlice;
          do {
            if (0 === exitStatus) {
              workInProgressRootIsPrerendering && !shouldTimeSlice && markRootSuspended(root$jscomp$0, lanes, 0, false);
              break;
            } else {
              forceSync = root$jscomp$0.current.alternate;
              if (renderWasConcurrent && !isRenderConsistentWithExternalStores(forceSync)) {
                exitStatus = renderRootSync(root$jscomp$0, lanes, false);
                renderWasConcurrent = false;
                continue;
              }
              if (2 === exitStatus) {
                renderWasConcurrent = lanes;
                if (root$jscomp$0.errorRecoveryDisabledLanes & renderWasConcurrent)
                  var JSCompiler_inline_result = 0;
                else
                  JSCompiler_inline_result = root$jscomp$0.pendingLanes & -536870913, JSCompiler_inline_result = 0 !== JSCompiler_inline_result ? JSCompiler_inline_result : JSCompiler_inline_result & 536870912 ? 536870912 : 0;
                if (0 !== JSCompiler_inline_result) {
                  lanes = JSCompiler_inline_result;
                  a: {
                    var root2 = root$jscomp$0;
                    exitStatus = workInProgressRootConcurrentErrors;
                    var wasRootDehydrated = supportsHydration && root2.current.memoizedState.isDehydrated;
                    wasRootDehydrated && (prepareFreshStack(root2, JSCompiler_inline_result).flags |= 256);
                    JSCompiler_inline_result = renderRootSync(
                      root2,
                      JSCompiler_inline_result,
                      false
                    );
                    if (2 !== JSCompiler_inline_result && 6 !== JSCompiler_inline_result) {
                      if (workInProgressRootDidAttachPingListener && !wasRootDehydrated) {
                        root2.errorRecoveryDisabledLanes |= renderWasConcurrent;
                        workInProgressRootInterleavedUpdatedLanes |= renderWasConcurrent;
                        exitStatus = 4;
                        break a;
                      }
                      renderWasConcurrent = workInProgressRootRecoverableErrors;
                      workInProgressRootRecoverableErrors = exitStatus;
                      null !== renderWasConcurrent && (null === workInProgressRootRecoverableErrors ? workInProgressRootRecoverableErrors = renderWasConcurrent : workInProgressRootRecoverableErrors.push.apply(
                        workInProgressRootRecoverableErrors,
                        renderWasConcurrent
                      ));
                    }
                    exitStatus = JSCompiler_inline_result;
                  }
                  renderWasConcurrent = false;
                  if (2 !== exitStatus) continue;
                }
              }
              if (1 === exitStatus) {
                prepareFreshStack(root$jscomp$0, 0);
                markRootSuspended(root$jscomp$0, lanes, 0, true);
                break;
              }
              a: {
                shouldTimeSlice = root$jscomp$0;
                renderWasConcurrent = exitStatus;
                switch (renderWasConcurrent) {
                  case 0:
                  case 1:
                    throw Error(formatProdErrorMessage(345));
                  case 4:
                    if ((lanes & 4194048) !== lanes && (lanes & 62914560) !== lanes)
                      break;
                  case 6:
                    markRootSuspended(
                      shouldTimeSlice,
                      lanes,
                      workInProgressDeferredLane,
                      !workInProgressRootDidSkipSuspendedSiblings
                    );
                    break a;
                  case 2:
                    workInProgressRootRecoverableErrors = null;
                    break;
                  case 3:
                  case 5:
                    break;
                  default:
                    throw Error(formatProdErrorMessage(329));
                }
                if ((lanes & 62914560) === lanes && (exitStatus = globalMostRecentFallbackTime + 300 - now2(), 10 < exitStatus)) {
                  markRootSuspended(
                    shouldTimeSlice,
                    lanes,
                    workInProgressDeferredLane,
                    !workInProgressRootDidSkipSuspendedSiblings
                  );
                  if (0 !== getNextLanes(shouldTimeSlice, 0, true)) break a;
                  pendingEffectsLanes = lanes;
                  shouldTimeSlice.timeoutHandle = scheduleTimeout(
                    completeRootWhenReady.bind(
                      null,
                      shouldTimeSlice,
                      forceSync,
                      workInProgressRootRecoverableErrors,
                      workInProgressTransitions,
                      workInProgressRootDidIncludeRecursiveRenderUpdate,
                      lanes,
                      workInProgressDeferredLane,
                      workInProgressRootInterleavedUpdatedLanes,
                      workInProgressSuspendedRetryLanes,
                      workInProgressRootDidSkipSuspendedSiblings,
                      renderWasConcurrent,
                      "Throttled",
                      -0,
                      0
                    ),
                    exitStatus
                  );
                  break a;
                }
                completeRootWhenReady(
                  shouldTimeSlice,
                  forceSync,
                  workInProgressRootRecoverableErrors,
                  workInProgressTransitions,
                  workInProgressRootDidIncludeRecursiveRenderUpdate,
                  lanes,
                  workInProgressDeferredLane,
                  workInProgressRootInterleavedUpdatedLanes,
                  workInProgressSuspendedRetryLanes,
                  workInProgressRootDidSkipSuspendedSiblings,
                  renderWasConcurrent,
                  null,
                  -0,
                  0
                );
              }
            }
            break;
          } while (1);
          ensureRootIsScheduled(root$jscomp$0);
        }
        function completeRootWhenReady(root2, finishedWork, recoverableErrors, transitions, didIncludeRenderPhaseUpdate, lanes, spawnedLane, updatedLanes, suspendedRetryLanes, didSkipSuspendedSiblings, exitStatus, suspendedCommitReason, completedRenderStartTime, completedRenderEndTime) {
          root2.timeoutHandle = noTimeout;
          var subtreeFlags = finishedWork.subtreeFlags, isViewTransitionEligible = (lanes & 335544064) === lanes;
          suspendedCommitReason = null;
          if (isViewTransitionEligible || subtreeFlags & 8192 || 16785408 === (subtreeFlags & 16785408)) {
            if (suspendedCommitReason = startSuspendingCommit(), appearingViewTransitions = null, accumulateSuspenseyCommitOnFiber(
              finishedWork,
              lanes,
              suspendedCommitReason
            ), isViewTransitionEligible && suspendOnActiveViewTransition(
              suspendedCommitReason,
              root2.containerInfo
            ), subtreeFlags = (lanes & 62914560) === lanes ? globalMostRecentFallbackTime - now2() : (lanes & 4194048) === lanes ? globalMostRecentTransitionTime - now2() : 0, subtreeFlags = waitForCommitToBeReady(
              suspendedCommitReason,
              subtreeFlags
            ), null !== subtreeFlags) {
              pendingEffectsLanes = lanes;
              root2.cancelPendingCommit = subtreeFlags(
                completeRoot.bind(
                  null,
                  root2,
                  finishedWork,
                  lanes,
                  recoverableErrors,
                  transitions,
                  didIncludeRenderPhaseUpdate,
                  spawnedLane,
                  updatedLanes,
                  suspendedRetryLanes,
                  didSkipSuspendedSiblings,
                  exitStatus,
                  suspendedCommitReason,
                  null,
                  completedRenderStartTime,
                  completedRenderEndTime
                )
              );
              markRootSuspended(root2, lanes, spawnedLane, !didSkipSuspendedSiblings);
              return;
            }
          }
          completeRoot(
            root2,
            finishedWork,
            lanes,
            recoverableErrors,
            transitions,
            didIncludeRenderPhaseUpdate,
            spawnedLane,
            updatedLanes,
            suspendedRetryLanes,
            didSkipSuspendedSiblings,
            exitStatus,
            suspendedCommitReason
          );
        }
        function isRenderConsistentWithExternalStores(finishedWork) {
          for (var node = finishedWork; ; ) {
            var tag = node.tag;
            if ((0 === tag || 11 === tag || 15 === tag) && node.flags & 16384 && (tag = node.updateQueue, null !== tag && (tag = tag.stores, null !== tag)))
              for (var i = 0; i < tag.length; i++) {
                var check = tag[i], getSnapshot = check.getSnapshot;
                check = check.value;
                try {
                  if (!objectIs(getSnapshot(), check)) return false;
                } catch (error) {
                  return false;
                }
              }
            tag = node.child;
            if (node.subtreeFlags & 16384 && null !== tag)
              tag.return = node, node = tag;
            else {
              if (node === finishedWork) break;
              for (; null === node.sibling; ) {
                if (null === node.return || node.return === finishedWork) return true;
                node = node.return;
              }
              node.sibling.return = node.return;
              node = node.sibling;
            }
          }
          return true;
        }
        function markRootSuspended(root2, suspendedLanes, spawnedLane, didAttemptEntireTree) {
          suspendedLanes = getEntangledLanes(root2, suspendedLanes);
          suspendedLanes &= ~workInProgressRootPingedLanes;
          suspendedLanes &= ~workInProgressRootInterleavedUpdatedLanes;
          root2.suspendedLanes |= suspendedLanes;
          root2.pingedLanes &= ~suspendedLanes;
          didAttemptEntireTree && (root2.warmLanes |= suspendedLanes);
          didAttemptEntireTree = root2.expirationTimes;
          for (var lanes = suspendedLanes; 0 < lanes; ) {
            var index$4 = 31 - clz32(lanes), lane = 1 << index$4;
            didAttemptEntireTree[index$4] = -1;
            lanes &= ~lane;
          }
          0 !== spawnedLane && markSpawnedDeferredLane(root2, spawnedLane, suspendedLanes);
        }
        function flushSyncWork() {
          return 0 === (executionContext & 6) ? (flushSyncWorkAcrossRoots_impl(0, false), false) : true;
        }
        function resetWorkInProgressStack() {
          if (null !== workInProgress) {
            if (0 === workInProgressSuspendedReason)
              var interruptedWork = workInProgress.return;
            else
              interruptedWork = workInProgress, lastContextDependency = currentlyRenderingFiber$1 = null, resetHooksOnUnwind(interruptedWork), thenableState$1 = null, thenableIndexCounter$1 = 0, interruptedWork = workInProgress;
            for (; null !== interruptedWork; )
              unwindInterruptedWork(interruptedWork.alternate, interruptedWork), interruptedWork = interruptedWork.return;
            workInProgress = null;
          }
        }
        function prepareFreshStack(root2, lanes) {
          var timeoutHandle = root2.timeoutHandle;
          timeoutHandle !== noTimeout && (root2.timeoutHandle = noTimeout, cancelTimeout(timeoutHandle));
          timeoutHandle = root2.cancelPendingCommit;
          null !== timeoutHandle && (root2.cancelPendingCommit = null, timeoutHandle());
          pendingEffectsLanes = 0;
          resetWorkInProgressStack();
          workInProgressRoot = root2;
          workInProgress = timeoutHandle = createWorkInProgress(root2.current, null);
          workInProgressRootRenderLanes = lanes;
          workInProgressSuspendedReason = 0;
          workInProgressThrownValue = null;
          workInProgressRootDidSkipSuspendedSiblings = false;
          workInProgressRootIsPrerendering = checkIfRootIsPrerendering(root2, lanes);
          workInProgressRootDidAttachPingListener = false;
          workInProgressSuspendedRetryLanes = workInProgressDeferredLane = workInProgressRootPingedLanes = workInProgressRootInterleavedUpdatedLanes = workInProgressRootSkippedLanes = workInProgressRootExitStatus = 0;
          workInProgressRootRecoverableErrors = workInProgressRootConcurrentErrors = null;
          workInProgressRootDidIncludeRecursiveRenderUpdate = false;
          entangledRenderLanes = getEntangledLanes(root2, lanes);
          finishQueueingConcurrentUpdates();
          return timeoutHandle;
        }
        function handleThrow(root2, thrownValue) {
          currentlyRenderingFiber = null;
          ReactSharedInternals.H = ContextOnlyDispatcher;
          thrownValue === SuspenseException || thrownValue === SuspenseActionException ? (thrownValue = getSuspendedThenable(), workInProgressSuspendedReason = 3) : thrownValue === SuspenseyCommitException ? (thrownValue = getSuspendedThenable(), workInProgressSuspendedReason = 4) : workInProgressSuspendedReason = thrownValue === SelectiveHydrationException ? 8 : null !== thrownValue && "object" === typeof thrownValue && "function" === typeof thrownValue.then ? 6 : 1;
          workInProgressThrownValue = thrownValue;
          null === workInProgress && (workInProgressRootExitStatus = 1, logUncaughtError(
            root2,
            createCapturedValueAtFiber(thrownValue, root2.current)
          ));
        }
        function shouldRemainOnPreviousScreen() {
          var handler = suspenseHandlerStackCursor.current;
          return null === handler ? true : (workInProgressRootRenderLanes & 4194048) === workInProgressRootRenderLanes ? null === shellBoundary ? true : false : (workInProgressRootRenderLanes & 62914560) === workInProgressRootRenderLanes || 0 !== (workInProgressRootRenderLanes & 536870912) ? handler === shellBoundary : false;
        }
        function pushDispatcher() {
          var prevDispatcher = ReactSharedInternals.H;
          ReactSharedInternals.H = ContextOnlyDispatcher;
          return null === prevDispatcher ? ContextOnlyDispatcher : prevDispatcher;
        }
        function pushAsyncDispatcher() {
          var prevAsyncDispatcher = ReactSharedInternals.A;
          ReactSharedInternals.A = DefaultAsyncDispatcher;
          return prevAsyncDispatcher;
        }
        function renderDidSuspendDelayIfPossible() {
          workInProgressRootExitStatus = 4;
          workInProgressRootDidSkipSuspendedSiblings || (workInProgressRootRenderLanes & 4194048) !== workInProgressRootRenderLanes && null !== suspenseHandlerStackCursor.current || (workInProgressRootIsPrerendering = true);
          0 === (workInProgressRootSkippedLanes & 134217727) && 0 === (workInProgressRootInterleavedUpdatedLanes & 134217727) || null === workInProgressRoot || markRootSuspended(
            workInProgressRoot,
            workInProgressRootRenderLanes,
            workInProgressDeferredLane,
            false
          );
        }
        function renderRootSync(root2, lanes, shouldYieldForPrerendering) {
          var prevExecutionContext = executionContext;
          executionContext |= 2;
          var prevDispatcher = pushDispatcher(), prevAsyncDispatcher = pushAsyncDispatcher();
          if (workInProgressRoot !== root2 || workInProgressRootRenderLanes !== lanes)
            workInProgressTransitions = null, prepareFreshStack(root2, lanes);
          lanes = false;
          var exitStatus = workInProgressRootExitStatus;
          a: do
            try {
              if (0 !== workInProgressSuspendedReason && null !== workInProgress) {
                var unitOfWork = workInProgress, thrownValue = workInProgressThrownValue;
                switch (workInProgressSuspendedReason) {
                  case 8:
                    resetWorkInProgressStack();
                    exitStatus = 6;
                    break a;
                  case 3:
                  case 2:
                  case 9:
                  case 6:
                    null === suspenseHandlerStackCursor.current && (lanes = true);
                    var reason = workInProgressSuspendedReason;
                    workInProgressSuspendedReason = 0;
                    workInProgressThrownValue = null;
                    throwAndUnwindWorkLoop(root2, unitOfWork, thrownValue, reason);
                    if (shouldYieldForPrerendering && workInProgressRootIsPrerendering) {
                      exitStatus = 0;
                      break a;
                    }
                    break;
                  default:
                    reason = workInProgressSuspendedReason, workInProgressSuspendedReason = 0, workInProgressThrownValue = null, throwAndUnwindWorkLoop(root2, unitOfWork, thrownValue, reason);
                }
              }
              workLoopSync();
              exitStatus = workInProgressRootExitStatus;
              break;
            } catch (thrownValue$169) {
              handleThrow(root2, thrownValue$169);
            }
          while (1);
          lanes && root2.shellSuspendCounter++;
          lastContextDependency = currentlyRenderingFiber$1 = null;
          executionContext = prevExecutionContext;
          ReactSharedInternals.H = prevDispatcher;
          ReactSharedInternals.A = prevAsyncDispatcher;
          null === workInProgress && (workInProgressRoot = null, workInProgressRootRenderLanes = 0, finishQueueingConcurrentUpdates());
          return exitStatus;
        }
        function workLoopSync() {
          for (; null !== workInProgress; ) performUnitOfWork(workInProgress);
        }
        function renderRootConcurrent(root2, lanes) {
          var prevExecutionContext = executionContext;
          executionContext |= 2;
          var prevDispatcher = pushDispatcher(), prevAsyncDispatcher = pushAsyncDispatcher();
          workInProgressRoot !== root2 || workInProgressRootRenderLanes !== lanes ? (workInProgressTransitions = null, workInProgressRootRenderTargetTime = now2() + 500, prepareFreshStack(root2, lanes)) : workInProgressRootIsPrerendering = checkIfRootIsPrerendering(
            root2,
            lanes
          );
          a: do
            try {
              if (0 !== workInProgressSuspendedReason && null !== workInProgress) {
                lanes = workInProgress;
                var thrownValue = workInProgressThrownValue;
                b: switch (workInProgressSuspendedReason) {
                  case 1:
                    workInProgressSuspendedReason = 0;
                    workInProgressThrownValue = null;
                    throwAndUnwindWorkLoop(root2, lanes, thrownValue, 1);
                    break;
                  case 2:
                  case 9:
                    if (isThenableResolved(thrownValue)) {
                      workInProgressSuspendedReason = 0;
                      workInProgressThrownValue = null;
                      replaySuspendedUnitOfWork(lanes);
                      break;
                    }
                    lanes = function() {
                      2 !== workInProgressSuspendedReason && 9 !== workInProgressSuspendedReason || workInProgressRoot !== root2 || (workInProgressSuspendedReason = 7);
                      ensureRootIsScheduled(root2);
                    };
                    thrownValue.then(lanes, lanes);
                    break a;
                  case 3:
                    workInProgressSuspendedReason = 7;
                    break a;
                  case 4:
                    workInProgressSuspendedReason = 5;
                    break a;
                  case 7:
                    isThenableResolved(thrownValue) ? (workInProgressSuspendedReason = 0, workInProgressThrownValue = null, replaySuspendedUnitOfWork(lanes)) : (workInProgressSuspendedReason = 0, workInProgressThrownValue = null, throwAndUnwindWorkLoop(root2, lanes, thrownValue, 7));
                    break;
                  case 5:
                    var resource = null;
                    switch (workInProgress.tag) {
                      case 26:
                        resource = workInProgress.memoizedState;
                      case 5:
                      case 27:
                        var hostFiber = workInProgress, type = hostFiber.type, props = hostFiber.pendingProps;
                        if (resource ? preloadResource(resource) : preloadInstance(hostFiber.stateNode, type, props)) {
                          workInProgressSuspendedReason = 0;
                          workInProgressThrownValue = null;
                          var sibling = hostFiber.sibling;
                          if (null !== sibling) workInProgress = sibling;
                          else {
                            var returnFiber = hostFiber.return;
                            null !== returnFiber ? (workInProgress = returnFiber, completeUnitOfWork(returnFiber)) : workInProgress = null;
                          }
                          break b;
                        }
                    }
                    workInProgressSuspendedReason = 0;
                    workInProgressThrownValue = null;
                    throwAndUnwindWorkLoop(root2, lanes, thrownValue, 5);
                    break;
                  case 6:
                    workInProgressSuspendedReason = 0;
                    workInProgressThrownValue = null;
                    throwAndUnwindWorkLoop(root2, lanes, thrownValue, 6);
                    break;
                  case 8:
                    resetWorkInProgressStack();
                    workInProgressRootExitStatus = 6;
                    break a;
                  default:
                    throw Error(formatProdErrorMessage(462));
                }
              }
              workLoopConcurrentByScheduler();
              break;
            } catch (thrownValue$171) {
              handleThrow(root2, thrownValue$171);
            }
          while (1);
          lastContextDependency = currentlyRenderingFiber$1 = null;
          ReactSharedInternals.H = prevDispatcher;
          ReactSharedInternals.A = prevAsyncDispatcher;
          executionContext = prevExecutionContext;
          if (null !== workInProgress) return 0;
          workInProgressRoot = null;
          workInProgressRootRenderLanes = 0;
          finishQueueingConcurrentUpdates();
          return workInProgressRootExitStatus;
        }
        function workLoopConcurrentByScheduler() {
          for (; null !== workInProgress && !shouldYield(); )
            performUnitOfWork(workInProgress);
        }
        function performUnitOfWork(unitOfWork) {
          var next = beginWork(
            unitOfWork.alternate,
            unitOfWork,
            entangledRenderLanes
          );
          unitOfWork.memoizedProps = unitOfWork.pendingProps;
          null === next ? completeUnitOfWork(unitOfWork) : workInProgress = next;
        }
        function replaySuspendedUnitOfWork(unitOfWork) {
          var next = unitOfWork;
          var current = next.alternate;
          switch (next.tag) {
            case 15:
            case 0:
              next = replayFunctionComponent(
                current,
                next,
                next.pendingProps,
                next.type,
                void 0,
                workInProgressRootRenderLanes
              );
              break;
            case 11:
              next = replayFunctionComponent(
                current,
                next,
                next.pendingProps,
                next.type.render,
                next.ref,
                workInProgressRootRenderLanes
              );
              break;
            case 5:
              resetHooksOnUnwind(next);
              var fiber = next;
              supportsHydration && fiber === hydrationParentFiber && (isHydrating ? (popToNextHostParent(fiber), 5 === fiber.tag && null != fiber.stateNode && (nextHydratableInstance = fiber.stateNode)) : (popToNextHostParent(fiber), isHydrating = true));
            default:
              unwindInterruptedWork(current, next), next = workInProgress = resetWorkInProgress(next, entangledRenderLanes), next = beginWork(current, next, entangledRenderLanes);
          }
          unitOfWork.memoizedProps = unitOfWork.pendingProps;
          null === next ? completeUnitOfWork(unitOfWork) : workInProgress = next;
        }
        function throwAndUnwindWorkLoop(root2, unitOfWork, thrownValue, suspendedReason) {
          lastContextDependency = currentlyRenderingFiber$1 = null;
          resetHooksOnUnwind(unitOfWork);
          thenableState$1 = null;
          thenableIndexCounter$1 = 0;
          var returnFiber = unitOfWork.return;
          try {
            if (throwException(
              root2,
              returnFiber,
              unitOfWork,
              thrownValue,
              workInProgressRootRenderLanes
            )) {
              workInProgressRootExitStatus = 1;
              logUncaughtError(
                root2,
                createCapturedValueAtFiber(thrownValue, root2.current)
              );
              workInProgress = null;
              return;
            }
          } catch (error) {
            if (null !== returnFiber) throw workInProgress = returnFiber, error;
            workInProgressRootExitStatus = 1;
            logUncaughtError(
              root2,
              createCapturedValueAtFiber(thrownValue, root2.current)
            );
            workInProgress = null;
            return;
          }
          if (unitOfWork.flags & 32768) {
            if (isHydrating || 1 === suspendedReason) root2 = true;
            else if (workInProgressRootIsPrerendering || 0 !== (workInProgressRootRenderLanes & 536870912))
              root2 = false;
            else if (workInProgressRootDidSkipSuspendedSiblings = root2 = true, 2 === suspendedReason || 9 === suspendedReason || 3 === suspendedReason || 6 === suspendedReason)
              suspendedReason = suspenseHandlerStackCursor.current, null !== suspendedReason && 13 === suspendedReason.tag && (suspendedReason.flags |= 16384);
            unwindUnitOfWork(unitOfWork, root2);
          } else completeUnitOfWork(unitOfWork);
        }
        function completeUnitOfWork(unitOfWork) {
          var completedWork = unitOfWork;
          do {
            if (0 !== (completedWork.flags & 32768)) {
              unwindUnitOfWork(
                completedWork,
                workInProgressRootDidSkipSuspendedSiblings
              );
              return;
            }
            unitOfWork = completedWork.return;
            var next = completeWork(
              completedWork.alternate,
              completedWork,
              entangledRenderLanes
            );
            if (null !== next) {
              workInProgress = next;
              return;
            }
            completedWork = completedWork.sibling;
            if (null !== completedWork) {
              workInProgress = completedWork;
              return;
            }
            workInProgress = completedWork = unitOfWork;
          } while (null !== completedWork);
          0 === workInProgressRootExitStatus && (workInProgressRootExitStatus = 5);
        }
        function unwindUnitOfWork(unitOfWork, skipSiblings) {
          do {
            var next = unwindWork(unitOfWork.alternate, unitOfWork);
            if (null !== next) {
              next.flags &= 32767;
              workInProgress = next;
              return;
            }
            next = unitOfWork.return;
            null !== next && (next.flags |= 32768, next.subtreeFlags = 0, next.deletions = null);
            if (!skipSiblings && (unitOfWork = unitOfWork.sibling, null !== unitOfWork)) {
              workInProgress = unitOfWork;
              return;
            }
            workInProgress = unitOfWork = next;
          } while (null !== unitOfWork);
          workInProgressRootExitStatus = 6;
          workInProgress = null;
        }
        function completeRoot(root2, finishedWork, lanes, recoverableErrors, transitions, didIncludeRenderPhaseUpdate, spawnedLane, updatedLanes, suspendedRetryLanes, didSkipSuspendedSiblings, exitStatus, suspendedState) {
          root2.cancelPendingCommit = null;
          do
            flushPendingEffects();
          while (0 !== pendingEffectsStatus);
          if (0 !== (executionContext & 6)) throw Error(formatProdErrorMessage(327));
          if (null !== finishedWork) {
            if (finishedWork === root2.current)
              throw Error(formatProdErrorMessage(177));
            root2 === workInProgressRoot && (workInProgress = workInProgressRoot = null, workInProgressRootRenderLanes = 0);
            pendingFinishedWork = finishedWork;
            pendingEffectsRoot = root2;
            pendingEffectsLanes = lanes;
            pendingPassiveTransitions = transitions;
            pendingRecoverableErrors = recoverableErrors;
            commitRoot(
              root2,
              finishedWork,
              lanes,
              spawnedLane,
              updatedLanes,
              suspendedRetryLanes,
              suspendedState
            );
          }
        }
        function commitRoot(root2, finishedWork, lanes, spawnedLane, updatedLanes, suspendedRetryLanes, suspendedState) {
          var remainingLanes = finishedWork.lanes | finishedWork.childLanes;
          pendingEffectsRemainingLanes = remainingLanes;
          remainingLanes |= concurrentlyUpdatedLanes;
          markRootFinished(
            root2,
            lanes,
            remainingLanes,
            spawnedLane,
            updatedLanes,
            suspendedRetryLanes
          );
          pendingViewTransitionEvents = null;
          (lanes & 335544064) === lanes ? (pendingTransitionTypes = claimQueuedTransitionTypes(root2), spawnedLane = 10262) : (pendingTransitionTypes = null, spawnedLane = 10256);
          0 !== (finishedWork.subtreeFlags & spawnedLane) || 0 !== (finishedWork.flags & spawnedLane) ? (root2.callbackNode = null, root2.callbackPriority = 0, scheduleCallback(NormalPriority$1, function() {
            flushPassiveEffects();
            return null;
          })) : (root2.callbackNode = null, root2.callbackPriority = 0);
          shouldStartViewTransition = false;
          spawnedLane = 0 !== (finishedWork.flags & 13878);
          if (0 !== (finishedWork.subtreeFlags & 13878) || spawnedLane) {
            spawnedLane = ReactSharedInternals.T;
            ReactSharedInternals.T = null;
            updatedLanes = getCurrentUpdatePriority();
            setCurrentUpdatePriority(2);
            suspendedRetryLanes = executionContext;
            executionContext |= 4;
            try {
              commitBeforeMutationEffects(root2, finishedWork, lanes);
            } finally {
              executionContext = suspendedRetryLanes, setCurrentUpdatePriority(updatedLanes), ReactSharedInternals.T = spawnedLane;
            }
          }
          pendingEffectsStatus = 1;
          shouldStartViewTransition ? pendingViewTransition = startViewTransition(
            suspendedState,
            root2.containerInfo,
            pendingTransitionTypes,
            flushMutationEffects,
            flushLayoutEffects,
            flushAfterMutationEffects,
            flushSpawnedWork,
            flushPassiveEffects,
            reportViewTransitionError,
            null,
            null
          ) : (flushMutationEffects(), flushLayoutEffects(), flushSpawnedWork());
        }
        function reportViewTransitionError(error) {
          if (0 !== pendingEffectsStatus) {
            var onRecoverableError = pendingEffectsRoot.onRecoverableError;
            onRecoverableError(error, { componentStack: null });
          }
        }
        function flushAfterMutationEffects() {
          3 === pendingEffectsStatus && (pendingEffectsStatus = 0, commitAfterMutationEffectsOnFiber(
            pendingFinishedWork,
            pendingEffectsRoot
          ), pendingEffectsStatus = 4);
        }
        function flushMutationEffects() {
          if (1 === pendingEffectsStatus) {
            pendingEffectsStatus = 0;
            var root2 = pendingEffectsRoot, finishedWork = pendingFinishedWork, lanes = pendingEffectsLanes, rootMutationHasEffect = 0 !== (finishedWork.flags & 13878);
            if (0 !== (finishedWork.subtreeFlags & 13878) || rootMutationHasEffect) {
              rootMutationHasEffect = ReactSharedInternals.T;
              ReactSharedInternals.T = null;
              var previousPriority = getCurrentUpdatePriority();
              setCurrentUpdatePriority(2);
              var prevExecutionContext = executionContext;
              executionContext |= 4;
              try {
                inUpdateViewTransition = rootViewTransitionAffected = false, commitMutationEffectsOnFiber(finishedWork, root2, lanes), resetAfterCommit(root2.containerInfo);
              } finally {
                executionContext = prevExecutionContext, setCurrentUpdatePriority(previousPriority), ReactSharedInternals.T = rootMutationHasEffect;
              }
            }
            root2.current = finishedWork;
            pendingEffectsStatus = 2;
          }
        }
        function flushLayoutEffects() {
          if (2 === pendingEffectsStatus) {
            pendingEffectsStatus = 0;
            var root2 = pendingEffectsRoot, finishedWork = pendingFinishedWork, rootHasLayoutEffect = 0 !== (finishedWork.flags & 8772);
            if (0 !== (finishedWork.subtreeFlags & 8772) || rootHasLayoutEffect) {
              rootHasLayoutEffect = ReactSharedInternals.T;
              ReactSharedInternals.T = null;
              var previousPriority = getCurrentUpdatePriority();
              setCurrentUpdatePriority(2);
              var prevExecutionContext = executionContext;
              executionContext |= 4;
              try {
                commitLayoutEffectOnFiber(root2, finishedWork.alternate, finishedWork);
              } finally {
                executionContext = prevExecutionContext, setCurrentUpdatePriority(previousPriority), ReactSharedInternals.T = rootHasLayoutEffect;
              }
            }
            pendingEffectsStatus = 3;
          }
        }
        function flushSpawnedWork() {
          if (4 === pendingEffectsStatus || 3 === pendingEffectsStatus) {
            pendingEffectsStatus = 0;
            var committedViewTransition = pendingViewTransition;
            pendingViewTransition = null;
            requestPaint();
            var root2 = pendingEffectsRoot, finishedWork = pendingFinishedWork, lanes = pendingEffectsLanes, recoverableErrors = pendingRecoverableErrors, passiveSubtreeMask = (lanes & 335544064) === lanes ? 10262 : 10256;
            0 !== (finishedWork.subtreeFlags & passiveSubtreeMask) || 0 !== (finishedWork.flags & passiveSubtreeMask) ? pendingEffectsStatus = 5 : (pendingEffectsStatus = 0, pendingFinishedWork = pendingEffectsRoot = null, releaseRootPooledCache(root2, root2.pendingLanes));
            passiveSubtreeMask = root2.pendingLanes;
            0 === passiveSubtreeMask && (legacyErrorBoundariesThatAlreadyFailed = null);
            lanesToEventPriority(lanes);
            finishedWork = finishedWork.stateNode;
            if (injectedHook && "function" === typeof injectedHook.onCommitFiberRoot)
              try {
                injectedHook.onCommitFiberRoot(
                  rendererID,
                  finishedWork,
                  void 0,
                  128 === (finishedWork.current.flags & 128)
                );
              } catch (err) {
              }
            if (null !== recoverableErrors) {
              finishedWork = ReactSharedInternals.T;
              passiveSubtreeMask = getCurrentUpdatePriority();
              setCurrentUpdatePriority(2);
              ReactSharedInternals.T = null;
              try {
                for (var onRecoverableError = root2.onRecoverableError, i = 0; i < recoverableErrors.length; i++) {
                  var recoverableError = recoverableErrors[i];
                  onRecoverableError(recoverableError.value, {
                    componentStack: recoverableError.stack
                  });
                }
              } finally {
                ReactSharedInternals.T = finishedWork, setCurrentUpdatePriority(passiveSubtreeMask);
              }
            }
            recoverableErrors = pendingViewTransitionEvents;
            onRecoverableError = pendingTransitionTypes;
            pendingTransitionTypes = null;
            if (null !== recoverableErrors && (pendingViewTransitionEvents = null, null === onRecoverableError && (onRecoverableError = []), null !== committedViewTransition))
              for (recoverableError = 0; recoverableError < recoverableErrors.length; recoverableError++)
                finishedWork = (0, recoverableErrors[recoverableError])(
                  onRecoverableError
                ), void 0 !== finishedWork && addViewTransitionFinishedListener(
                  committedViewTransition,
                  finishedWork
                );
            0 !== (pendingEffectsLanes & 3) && flushPendingEffects();
            ensureRootIsScheduled(root2);
            passiveSubtreeMask = root2.pendingLanes;
            0 !== (lanes & 261930) && 0 !== (passiveSubtreeMask & 42) ? root2 === rootWithNestedUpdates ? nestedUpdateCount++ : (nestedUpdateCount = 0, rootWithNestedUpdates = root2) : (nestedUpdateCount = 0, rootWithNestedUpdates = null);
            supportsHydration && flushHydrationEvents();
            flushSyncWorkAcrossRoots_impl(0, false);
          }
        }
        function releaseRootPooledCache(root2, remainingLanes) {
          0 === (root2.pooledCacheLanes &= remainingLanes) && (remainingLanes = root2.pooledCache, null != remainingLanes && (root2.pooledCache = null, releaseCache(remainingLanes)));
        }
        function flushPendingEffects() {
          null !== pendingViewTransition && (stopViewTransition(pendingViewTransition), pendingViewTransition = null);
          flushMutationEffects();
          flushLayoutEffects();
          flushSpawnedWork();
          return flushPassiveEffects();
        }
        function flushPassiveEffects() {
          if (5 !== pendingEffectsStatus) return false;
          var root2 = pendingEffectsRoot, remainingLanes = pendingEffectsRemainingLanes;
          pendingEffectsRemainingLanes = 0;
          var renderPriority = lanesToEventPriority(pendingEffectsLanes), priority = 32 > renderPriority ? 32 : renderPriority;
          renderPriority = ReactSharedInternals.T;
          var previousPriority = getCurrentUpdatePriority();
          try {
            setCurrentUpdatePriority(priority);
            ReactSharedInternals.T = null;
            priority = pendingPassiveTransitions;
            pendingPassiveTransitions = null;
            var root$jscomp$0 = pendingEffectsRoot, lanes = pendingEffectsLanes;
            pendingEffectsStatus = 0;
            pendingFinishedWork = pendingEffectsRoot = null;
            pendingEffectsLanes = 0;
            if (0 !== (executionContext & 6))
              throw Error(formatProdErrorMessage(331));
            var prevExecutionContext = executionContext;
            executionContext |= 4;
            commitPassiveUnmountOnFiber(root$jscomp$0.current);
            commitPassiveMountOnFiber(
              root$jscomp$0,
              root$jscomp$0.current,
              lanes,
              priority
            );
            executionContext = prevExecutionContext;
            flushSyncWorkAcrossRoots_impl(0, false);
            if (injectedHook && "function" === typeof injectedHook.onPostCommitFiberRoot)
              try {
                injectedHook.onPostCommitFiberRoot(rendererID, root$jscomp$0);
              } catch (err) {
              }
            return true;
          } finally {
            setCurrentUpdatePriority(previousPriority), ReactSharedInternals.T = renderPriority, releaseRootPooledCache(root2, remainingLanes);
          }
        }
        function captureCommitPhaseErrorOnRoot(rootFiber, sourceFiber, error) {
          sourceFiber = createCapturedValueAtFiber(error, sourceFiber);
          sourceFiber = createRootErrorUpdate(rootFiber.stateNode, sourceFiber, 2);
          rootFiber = enqueueUpdate(rootFiber, sourceFiber, 2);
          null !== rootFiber && (markRootUpdated$1(rootFiber, 2), ensureRootIsScheduled(rootFiber));
        }
        function captureCommitPhaseError(sourceFiber, nearestMountedAncestor, error) {
          if (3 === sourceFiber.tag)
            captureCommitPhaseErrorOnRoot(sourceFiber, sourceFiber, error);
          else
            for (; null !== nearestMountedAncestor; ) {
              if (3 === nearestMountedAncestor.tag) {
                captureCommitPhaseErrorOnRoot(
                  nearestMountedAncestor,
                  sourceFiber,
                  error
                );
                break;
              } else if (1 === nearestMountedAncestor.tag) {
                var instance = nearestMountedAncestor.stateNode;
                if ("function" === typeof nearestMountedAncestor.type.getDerivedStateFromError || "function" === typeof instance.componentDidCatch && (null === legacyErrorBoundariesThatAlreadyFailed || !legacyErrorBoundariesThatAlreadyFailed.has(instance))) {
                  sourceFiber = createCapturedValueAtFiber(error, sourceFiber);
                  error = createClassErrorUpdate(2);
                  instance = enqueueUpdate(nearestMountedAncestor, error, 2);
                  null !== instance && (initializeClassErrorUpdate(
                    error,
                    instance,
                    nearestMountedAncestor,
                    sourceFiber
                  ), markRootUpdated$1(instance, 2), ensureRootIsScheduled(instance));
                  break;
                }
              }
              nearestMountedAncestor = nearestMountedAncestor.return;
            }
        }
        function attachPingListener(root2, wakeable, lanes) {
          var pingCache = root2.pingCache;
          if (null === pingCache) {
            pingCache = root2.pingCache = new PossiblyWeakMap();
            var threadIDs = /* @__PURE__ */ new Set();
            pingCache.set(wakeable, threadIDs);
          } else
            threadIDs = pingCache.get(wakeable), void 0 === threadIDs && (threadIDs = /* @__PURE__ */ new Set(), pingCache.set(wakeable, threadIDs));
          threadIDs.has(lanes) || (workInProgressRootDidAttachPingListener = true, threadIDs.add(lanes), root2 = pingSuspendedRoot.bind(null, root2, wakeable, lanes), wakeable.then(root2, root2));
        }
        function pingSuspendedRoot(root2, wakeable, pingedLanes) {
          var pingCache = root2.pingCache;
          null !== pingCache && pingCache.delete(wakeable);
          root2.pingedLanes |= root2.suspendedLanes & pingedLanes;
          root2.warmLanes &= ~pingedLanes;
          workInProgressRoot === root2 && (workInProgressRootRenderLanes & pingedLanes) === pingedLanes && (4 === workInProgressRootExitStatus || 3 === workInProgressRootExitStatus && (workInProgressRootRenderLanes & 62914560) === workInProgressRootRenderLanes && 300 > now2() - globalMostRecentFallbackTime ? 0 === (executionContext & 2) ? prepareFreshStack(root2, 0) : workInProgressRootPingedLanes |= pingedLanes : workInProgressRootPingedLanes |= pingedLanes, workInProgressSuspendedRetryLanes === workInProgressRootRenderLanes && (workInProgressSuspendedRetryLanes = 0));
          ensureRootIsScheduled(root2);
        }
        function retryTimedOutBoundary(boundaryFiber, retryLane) {
          0 === retryLane && (retryLane = claimNextRetryLane());
          boundaryFiber = enqueueConcurrentRenderForLane(boundaryFiber, retryLane);
          null !== boundaryFiber && (markRootUpdated$1(boundaryFiber, retryLane), ensureRootIsScheduled(boundaryFiber));
        }
        function retryDehydratedSuspenseBoundary(boundaryFiber) {
          var suspenseState = boundaryFiber.memoizedState, retryLane = 0;
          null !== suspenseState && (retryLane = suspenseState.retryLane);
          retryTimedOutBoundary(boundaryFiber, retryLane);
        }
        function resolveRetryWakeable(boundaryFiber, wakeable) {
          var retryLane = 0;
          switch (boundaryFiber.tag) {
            case 31:
            case 13:
              var retryCache = boundaryFiber.stateNode;
              var suspenseState = boundaryFiber.memoizedState;
              null !== suspenseState && (retryLane = suspenseState.retryLane);
              break;
            case 19:
              retryCache = boundaryFiber.stateNode;
              break;
            case 22:
              retryCache = boundaryFiber.stateNode._retryCache;
              break;
            default:
              throw Error(formatProdErrorMessage(314));
          }
          null !== retryCache && retryCache.delete(wakeable);
          retryTimedOutBoundary(boundaryFiber, retryLane);
        }
        function scheduleCallback(priorityLevel, callback) {
          return scheduleCallback$3(priorityLevel, callback);
        }
        function FiberNode(tag, pendingProps, key, mode) {
          this.tag = tag;
          this.key = key;
          this.sibling = this.child = this.return = this.stateNode = this.type = this.elementType = null;
          this.index = 0;
          this.refCleanup = this.ref = null;
          this.pendingProps = pendingProps;
          this.dependencies = this.memoizedState = this.updateQueue = this.memoizedProps = null;
          this.mode = mode;
          this.subtreeFlags = this.flags = 0;
          this.deletions = null;
          this.childLanes = this.lanes = 0;
          this.alternate = null;
        }
        function shouldConstruct(Component) {
          Component = Component.prototype;
          return !(!Component || !Component.isReactComponent);
        }
        function createWorkInProgress(current, pendingProps) {
          var workInProgress2 = current.alternate;
          null === workInProgress2 ? (workInProgress2 = createFiber(
            current.tag,
            pendingProps,
            current.key,
            current.mode
          ), workInProgress2.elementType = current.elementType, workInProgress2.type = current.type, workInProgress2.stateNode = current.stateNode, workInProgress2.alternate = current, current.alternate = workInProgress2) : (workInProgress2.pendingProps = pendingProps, workInProgress2.type = current.type, workInProgress2.flags = 0, workInProgress2.subtreeFlags = 0, workInProgress2.deletions = null);
          workInProgress2.flags = current.flags & 1206910976;
          workInProgress2.childLanes = current.childLanes;
          workInProgress2.lanes = current.lanes;
          workInProgress2.child = current.child;
          workInProgress2.memoizedProps = current.memoizedProps;
          workInProgress2.memoizedState = current.memoizedState;
          workInProgress2.updateQueue = current.updateQueue;
          pendingProps = current.dependencies;
          workInProgress2.dependencies = null === pendingProps ? null : {
            lanes: pendingProps.lanes,
            firstContext: pendingProps.firstContext
          };
          workInProgress2.sibling = current.sibling;
          workInProgress2.index = current.index;
          workInProgress2.ref = current.ref;
          workInProgress2.refCleanup = current.refCleanup;
          return workInProgress2;
        }
        function resetWorkInProgress(workInProgress2, renderLanes2) {
          workInProgress2.flags &= 1206910978;
          var current = workInProgress2.alternate;
          null === current ? (workInProgress2.childLanes = 0, workInProgress2.lanes = renderLanes2, workInProgress2.child = null, workInProgress2.subtreeFlags = 0, workInProgress2.memoizedProps = null, workInProgress2.memoizedState = null, workInProgress2.updateQueue = null, workInProgress2.dependencies = null, workInProgress2.stateNode = null) : (workInProgress2.childLanes = current.childLanes, workInProgress2.lanes = current.lanes, workInProgress2.child = current.child, workInProgress2.subtreeFlags = 0, workInProgress2.deletions = null, workInProgress2.memoizedProps = current.memoizedProps, workInProgress2.memoizedState = current.memoizedState, workInProgress2.updateQueue = current.updateQueue, workInProgress2.type = current.type, renderLanes2 = current.dependencies, workInProgress2.dependencies = null === renderLanes2 ? null : {
            lanes: renderLanes2.lanes,
            firstContext: renderLanes2.firstContext
          });
          return workInProgress2;
        }
        function createFiberFromTypeAndProps(type, key, pendingProps, owner, mode, lanes) {
          var fiberTag = 0;
          owner = type;
          if ("function" === typeof owner) shouldConstruct(owner) && (fiberTag = 1);
          else if ("string" === typeof owner)
            fiberTag = supportsResources && supportsSingletons ? isHostHoistableType(type, pendingProps, contextStackCursor.current) ? 26 : isHostSingletonType(type) ? 27 : 5 : supportsResources ? isHostHoistableType(
              type,
              pendingProps,
              contextStackCursor.current
            ) ? 26 : 5 : supportsSingletons ? isHostSingletonType(type) ? 27 : 5 : 5;
          else
            a: switch (owner) {
              case REACT_ACTIVITY_TYPE:
                return type = createFiber(31, pendingProps, key, mode), type.elementType = REACT_ACTIVITY_TYPE, type.lanes = lanes, type;
              case REACT_FRAGMENT_TYPE:
                return createFiberFromFragment(
                  pendingProps.children,
                  mode,
                  lanes,
                  key
                );
              case REACT_STRICT_MODE_TYPE:
                fiberTag = 8;
                mode |= 24;
                break;
              case REACT_PROFILER_TYPE:
                return type = createFiber(12, pendingProps, key, mode | 2), type.elementType = REACT_PROFILER_TYPE, type.lanes = lanes, type;
              case REACT_SUSPENSE_TYPE:
                return type = createFiber(13, pendingProps, key, mode), type.elementType = REACT_SUSPENSE_TYPE, type.lanes = lanes, type;
              case REACT_SUSPENSE_LIST_TYPE:
                return type = createFiber(19, pendingProps, key, mode), type.elementType = REACT_SUSPENSE_LIST_TYPE, type.lanes = lanes, type;
              case REACT_LEGACY_HIDDEN_TYPE:
              case REACT_VIEW_TRANSITION_TYPE:
                return type = mode | 32, type = createFiber(30, pendingProps, key, type), type.elementType = REACT_VIEW_TRANSITION_TYPE, type.lanes = lanes, type.stateNode = {
                  autoName: null,
                  paired: null,
                  clones: null,
                  ref: null
                }, type;
              default:
                if ("object" === typeof owner && null !== owner)
                  switch (owner.$$typeof) {
                    case REACT_CONTEXT_TYPE:
                      fiberTag = 10;
                      break a;
                    case REACT_CONSUMER_TYPE:
                      fiberTag = 9;
                      break a;
                    case REACT_FORWARD_REF_TYPE:
                      fiberTag = 11;
                      break a;
                    case REACT_MEMO_TYPE:
                      fiberTag = 14;
                      break a;
                    case REACT_LAZY_TYPE:
                      fiberTag = 16;
                      owner = null;
                      break a;
                  }
                fiberTag = 29;
                pendingProps = Error(
                  formatProdErrorMessage(
                    130,
                    null === type ? "null" : typeof type,
                    ""
                  )
                );
                owner = null;
            }
          key = createFiber(fiberTag, pendingProps, key, mode);
          key.elementType = type;
          key.type = owner;
          key.lanes = lanes;
          return key;
        }
        function createFiberFromFragment(elements, mode, lanes, key) {
          elements = createFiber(7, elements, key, mode);
          elements.lanes = lanes;
          return elements;
        }
        function createFiberFromText(content, mode, lanes) {
          content = createFiber(6, content, null, mode);
          content.lanes = lanes;
          return content;
        }
        function createFiberFromDehydratedFragment(dehydratedNode) {
          var fiber = createFiber(18, null, null, 0);
          fiber.stateNode = dehydratedNode;
          return fiber;
        }
        function createFiberFromPortal(portal, mode, lanes) {
          mode = createFiber(
            4,
            null !== portal.children ? portal.children : [],
            portal.key,
            mode
          );
          mode.lanes = lanes;
          mode.stateNode = {
            containerInfo: portal.containerInfo,
            pendingChildren: null,
            implementation: portal.implementation
          };
          return mode;
        }
        function FiberRootNode(containerInfo, tag, hydrate, identifierPrefix, onUncaughtError, onCaughtError, onRecoverableError, onDefaultTransitionIndicator, formState) {
          this.tag = 1;
          this.containerInfo = containerInfo;
          this.pingCache = this.current = this.pendingChildren = null;
          this.timeoutHandle = noTimeout;
          this.callbackNode = this.next = this.pendingContext = this.context = this.cancelPendingCommit = null;
          this.callbackPriority = 0;
          this.expirationTimes = createLaneMap(-1);
          this.entangledLanes = this.shellSuspendCounter = this.errorRecoveryDisabledLanes = this.expiredLanes = this.warmLanes = this.pingedLanes = this.suspendedLanes = this.pendingLanes = 0;
          this.entanglements = createLaneMap(0);
          this.hiddenUpdates = createLaneMap(null);
          this.identifierPrefix = identifierPrefix;
          this.onUncaughtError = onUncaughtError;
          this.onCaughtError = onCaughtError;
          this.onRecoverableError = onRecoverableError;
          this.pooledCache = null;
          this.pooledCacheLanes = 0;
          this.formState = formState;
          this.transitionTypes = null;
          this.incompleteTransitions = /* @__PURE__ */ new Map();
        }
        function createFiberRoot(containerInfo, tag, hydrate, initialChildren, hydrationCallbacks, isStrictMode, identifierPrefix, formState, onUncaughtError, onCaughtError, onRecoverableError, onDefaultTransitionIndicator) {
          containerInfo = new FiberRootNode(
            containerInfo,
            tag,
            hydrate,
            identifierPrefix,
            onUncaughtError,
            onCaughtError,
            onRecoverableError,
            onDefaultTransitionIndicator,
            formState
          );
          tag = 1;
          true === isStrictMode && (tag |= 24);
          isStrictMode = createFiber(3, null, null, tag);
          containerInfo.current = isStrictMode;
          isStrictMode.stateNode = containerInfo;
          tag = createCache();
          tag.refCount++;
          containerInfo.pooledCache = tag;
          tag.refCount++;
          isStrictMode.memoizedState = {
            element: initialChildren,
            isDehydrated: hydrate,
            cache: tag
          };
          initializeUpdateQueue(isStrictMode);
          return containerInfo;
        }
        function getContextForSubtree(parentComponent) {
          if (!parentComponent) return emptyContextObject;
          parentComponent = emptyContextObject;
          return parentComponent;
        }
        function findHostInstance(component) {
          var fiber = component._reactInternals;
          if (void 0 === fiber) {
            if ("function" === typeof component.render)
              throw Error(formatProdErrorMessage(188));
            component = Object.keys(component).join(",");
            throw Error(formatProdErrorMessage(268, component));
          }
          component = findCurrentFiberUsingSlowPath(fiber);
          component = null !== component ? findCurrentHostFiberImpl(component) : null;
          return null === component ? null : getPublicInstance(component.stateNode);
        }
        function updateContainerImpl(rootFiber, lane, element, container2, parentComponent, callback) {
          parentComponent = getContextForSubtree(parentComponent);
          null === container2.context ? container2.context = parentComponent : container2.pendingContext = parentComponent;
          container2 = createUpdate(lane);
          container2.payload = { element };
          callback = void 0 === callback ? null : callback;
          null !== callback && (container2.callback = callback);
          element = enqueueUpdate(rootFiber, container2, lane);
          null !== element && (scheduleUpdateOnFiber(element, rootFiber, lane), entangleTransitions(element, rootFiber, lane));
        }
        function markRetryLaneImpl(fiber, retryLane) {
          fiber = fiber.memoizedState;
          if (null !== fiber && null !== fiber.dehydrated) {
            var a = fiber.retryLane;
            fiber.retryLane = 0 !== a && a < retryLane ? a : retryLane;
          }
        }
        function markRetryLaneIfNotHydrated(fiber, retryLane) {
          markRetryLaneImpl(fiber, retryLane);
          (fiber = fiber.alternate) && markRetryLaneImpl(fiber, retryLane);
        }
        var exports2 = {};
        "use strict";
        var React2 = require_react(), Scheduler = require_scheduler(), assign = Object.assign, REACT_LEGACY_ELEMENT_TYPE = /* @__PURE__ */ Symbol.for("react.element"), REACT_ELEMENT_TYPE = /* @__PURE__ */ Symbol.for("react.transitional.element"), REACT_PORTAL_TYPE = /* @__PURE__ */ Symbol.for("react.portal"), REACT_FRAGMENT_TYPE = /* @__PURE__ */ Symbol.for("react.fragment"), REACT_STRICT_MODE_TYPE = /* @__PURE__ */ Symbol.for("react.strict_mode"), REACT_PROFILER_TYPE = /* @__PURE__ */ Symbol.for("react.profiler"), REACT_CONSUMER_TYPE = /* @__PURE__ */ Symbol.for("react.consumer"), REACT_CONTEXT_TYPE = /* @__PURE__ */ Symbol.for("react.context"), REACT_FORWARD_REF_TYPE = /* @__PURE__ */ Symbol.for("react.forward_ref"), REACT_SUSPENSE_TYPE = /* @__PURE__ */ Symbol.for("react.suspense"), REACT_SUSPENSE_LIST_TYPE = /* @__PURE__ */ Symbol.for("react.suspense_list"), REACT_MEMO_TYPE = /* @__PURE__ */ Symbol.for("react.memo"), REACT_LAZY_TYPE = /* @__PURE__ */ Symbol.for("react.lazy");
        /* @__PURE__ */ Symbol.for("react.scope");
        var REACT_ACTIVITY_TYPE = /* @__PURE__ */ Symbol.for("react.activity"), REACT_LEGACY_HIDDEN_TYPE = /* @__PURE__ */ Symbol.for("react.legacy_hidden");
        /* @__PURE__ */ Symbol.for("react.tracing_marker");
        var REACT_MEMO_CACHE_SENTINEL = /* @__PURE__ */ Symbol.for("react.memo_cache_sentinel"), REACT_VIEW_TRANSITION_TYPE = /* @__PURE__ */ Symbol.for("react.view_transition"), REACT_RECOVERABLE_TYPE = /* @__PURE__ */ Symbol.for("react.recoverable"), MAYBE_ITERATOR_SYMBOL = Symbol.iterator, REACT_OPTIMISTIC_KEY = /* @__PURE__ */ Symbol.for("react.optimistic_key"), REACT_CLIENT_REFERENCE = /* @__PURE__ */ Symbol.for("react.client.reference"), isArrayImpl = Array.isArray, ReactSharedInternals = React2.__CLIENT_INTERNALS_DO_NOT_USE_OR_WARN_USERS_THEY_CANNOT_UPGRADE, rendererVersion = $$$config.rendererVersion, rendererPackageName = $$$config.rendererPackageName, extraDevToolsConfig = $$$config.extraDevToolsConfig, getPublicInstance = $$$config.getPublicInstance, getRootHostContext = $$$config.getRootHostContext, getChildHostContext = $$$config.getChildHostContext, prepareForCommit = $$$config.prepareForCommit, resetAfterCommit = $$$config.resetAfterCommit, createInstance = $$$config.createInstance;
        $$$config.cloneMutableInstance;
        var appendInitialChild = $$$config.appendInitialChild, finalizeInitialChildren = $$$config.finalizeInitialChildren, shouldSetTextContent = $$$config.shouldSetTextContent, createTextInstance = $$$config.createTextInstance;
        $$$config.cloneMutableTextInstance;
        var scheduleTimeout = $$$config.scheduleTimeout, cancelTimeout = $$$config.cancelTimeout, noTimeout = $$$config.noTimeout, isPrimaryRenderer = $$$config.isPrimaryRenderer;
        $$$config.warnsIfNotActing;
        var supportsMutation = $$$config.supportsMutation, supportsPersistence = $$$config.supportsPersistence, supportsHydration = $$$config.supportsHydration, getInstanceFromNode = $$$config.getInstanceFromNode;
        $$$config.beforeActiveInstanceBlur;
        var preparePortalMount = $$$config.preparePortalMount;
        $$$config.prepareScopeUpdate;
        $$$config.getInstanceFromScope;
        var setCurrentUpdatePriority = $$$config.setCurrentUpdatePriority, getCurrentUpdatePriority = $$$config.getCurrentUpdatePriority, resolveUpdatePriority = $$$config.resolveUpdatePriority;
        $$$config.trackSchedulerEvent;
        $$$config.resolveEventType;
        $$$config.resolveEventTimeStamp;
        var shouldAttemptEagerTransition = $$$config.shouldAttemptEagerTransition, detachDeletedInstance = $$$config.detachDeletedInstance;
        $$$config.requestPostPaintCallback;
        var maySuspendCommit = $$$config.maySuspendCommit, maySuspendCommitOnUpdate = $$$config.maySuspendCommitOnUpdate, maySuspendCommitInSyncRender = $$$config.maySuspendCommitInSyncRender, preloadInstance = $$$config.preloadInstance, startSuspendingCommit = $$$config.startSuspendingCommit, suspendInstance = $$$config.suspendInstance, suspendOnActiveViewTransition = $$$config.suspendOnActiveViewTransition, waitForCommitToBeReady = $$$config.waitForCommitToBeReady;
        $$$config.getSuspendedCommitReason;
        var NotPendingTransition = $$$config.NotPendingTransition, HostTransitionContext = $$$config.HostTransitionContext, resetFormInstance = $$$config.resetFormInstance;
        $$$config.bindToConsole;
        var supportsMicrotasks = $$$config.supportsMicrotasks, scheduleMicrotask = $$$config.scheduleMicrotask, supportsTestSelectors = $$$config.supportsTestSelectors, findFiberRoot = $$$config.findFiberRoot, getBoundingRect = $$$config.getBoundingRect, getTextContent = $$$config.getTextContent, isHiddenSubtree = $$$config.isHiddenSubtree, matchAccessibilityRole = $$$config.matchAccessibilityRole, setFocusIfFocusable = $$$config.setFocusIfFocusable, setupIntersectionObserver = $$$config.setupIntersectionObserver, appendChild = $$$config.appendChild, appendChildToContainer = $$$config.appendChildToContainer, commitTextUpdate = $$$config.commitTextUpdate, commitMount = $$$config.commitMount, commitUpdate = $$$config.commitUpdate, insertBefore = $$$config.insertBefore, insertInContainerBefore = $$$config.insertInContainerBefore, removeChild = $$$config.removeChild, removeChildFromContainer = $$$config.removeChildFromContainer, resetTextContent = $$$config.resetTextContent, hideInstance = $$$config.hideInstance, hideTextInstance = $$$config.hideTextInstance, unhideInstance = $$$config.unhideInstance, unhideTextInstance = $$$config.unhideTextInstance, applyViewTransitionName = $$$config.applyViewTransitionName, restoreViewTransitionName = $$$config.restoreViewTransitionName, cancelViewTransitionName = $$$config.cancelViewTransitionName, cancelRootViewTransitionName = $$$config.cancelRootViewTransitionName, restoreRootViewTransitionName = $$$config.restoreRootViewTransitionName;
        $$$config.cloneRootViewTransitionContainer;
        $$$config.removeRootViewTransitionClone;
        var measureInstance = $$$config.measureInstance, measureClonedInstance = $$$config.measureClonedInstance, wasInstanceInViewport = $$$config.wasInstanceInViewport, hasInstanceChanged = $$$config.hasInstanceChanged, hasInstanceAffectedParent = $$$config.hasInstanceAffectedParent, startViewTransition = $$$config.startViewTransition;
        $$$config.startGestureTransition;
        var stopViewTransition = $$$config.stopViewTransition, addViewTransitionFinishedListener = $$$config.addViewTransitionFinishedListener;
        $$$config.getCurrentGestureOffset;
        var createViewTransitionInstance = $$$config.createViewTransitionInstance, clearContainer = $$$config.clearContainer, createFragmentInstance = $$$config.createFragmentInstance, updateFragmentInstanceFiber = $$$config.updateFragmentInstanceFiber, commitNewChildToFragmentInstance = $$$config.commitNewChildToFragmentInstance, deleteChildFromFragmentInstance = $$$config.deleteChildFromFragmentInstance, cloneInstance = $$$config.cloneInstance, createContainerChildSet = $$$config.createContainerChildSet, appendChildToContainerChildSet = $$$config.appendChildToContainerChildSet, finalizeContainerChildren = $$$config.finalizeContainerChildren, replaceContainerChildren = $$$config.replaceContainerChildren, cloneHiddenInstance = $$$config.cloneHiddenInstance, cloneHiddenTextInstance = $$$config.cloneHiddenTextInstance, isSuspenseInstancePending = $$$config.isSuspenseInstancePending, isSuspenseInstanceFallback = $$$config.isSuspenseInstanceFallback, getSuspenseInstanceFallbackErrorDetails = $$$config.getSuspenseInstanceFallbackErrorDetails, registerSuspenseInstanceRetry = $$$config.registerSuspenseInstanceRetry, canHydrateFormStateMarker = $$$config.canHydrateFormStateMarker, isFormStateMarkerMatching = $$$config.isFormStateMarkerMatching, getNextHydratableSibling = $$$config.getNextHydratableSibling, getNextHydratableSiblingAfterSingleton = $$$config.getNextHydratableSiblingAfterSingleton, getFirstHydratableChild = $$$config.getFirstHydratableChild, getFirstHydratableChildWithinContainer = $$$config.getFirstHydratableChildWithinContainer, getFirstHydratableChildWithinActivityInstance = $$$config.getFirstHydratableChildWithinActivityInstance, getFirstHydratableChildWithinSuspenseInstance = $$$config.getFirstHydratableChildWithinSuspenseInstance, getFirstHydratableChildWithinSingleton = $$$config.getFirstHydratableChildWithinSingleton, canHydrateInstance = $$$config.canHydrateInstance, canHydrateTextInstance = $$$config.canHydrateTextInstance, canHydrateActivityInstance = $$$config.canHydrateActivityInstance, canHydrateSuspenseInstance = $$$config.canHydrateSuspenseInstance, hydrateInstance = $$$config.hydrateInstance, hydrateTextInstance = $$$config.hydrateTextInstance, hydrateActivityInstance = $$$config.hydrateActivityInstance, hydrateSuspenseInstance = $$$config.hydrateSuspenseInstance, getNextHydratableInstanceAfterActivityInstance = $$$config.getNextHydratableInstanceAfterActivityInstance, getNextHydratableInstanceAfterSuspenseInstance = $$$config.getNextHydratableInstanceAfterSuspenseInstance, commitHydratedInstance = $$$config.commitHydratedInstance, commitHydratedContainer = $$$config.commitHydratedContainer, commitHydratedActivityInstance = $$$config.commitHydratedActivityInstance, commitHydratedSuspenseInstance = $$$config.commitHydratedSuspenseInstance, finalizeHydratedChildren = $$$config.finalizeHydratedChildren, flushHydrationEvents = $$$config.flushHydrationEvents;
        $$$config.clearActivityBoundary;
        var clearSuspenseBoundary = $$$config.clearSuspenseBoundary;
        $$$config.clearActivityBoundaryFromContainer;
        var clearSuspenseBoundaryFromContainer = $$$config.clearSuspenseBoundaryFromContainer, hideDehydratedBoundary = $$$config.hideDehydratedBoundary, unhideDehydratedBoundary = $$$config.unhideDehydratedBoundary, shouldDeleteUnhydratedTailInstances = $$$config.shouldDeleteUnhydratedTailInstances;
        $$$config.diffHydratedPropsForDevWarnings;
        $$$config.diffHydratedTextForDevWarnings;
        $$$config.describeHydratableInstanceForDevWarnings;
        var validateHydratableInstance = $$$config.validateHydratableInstance, validateHydratableTextInstance = $$$config.validateHydratableTextInstance, supportsResources = $$$config.supportsResources, isHostHoistableType = $$$config.isHostHoistableType, getHoistableRoot = $$$config.getHoistableRoot, getResource = $$$config.getResource, acquireResource = $$$config.acquireResource, releaseResource = $$$config.releaseResource, hydrateHoistable = $$$config.hydrateHoistable, mountHoistable = $$$config.mountHoistable, unmountHoistable = $$$config.unmountHoistable, createHoistableInstance = $$$config.createHoistableInstance, prepareToCommitHoistables = $$$config.prepareToCommitHoistables, mayResourceSuspendCommit = $$$config.mayResourceSuspendCommit, preloadResource = $$$config.preloadResource, suspendResource = $$$config.suspendResource, supportsSingletons = $$$config.supportsSingletons, resolveSingletonInstance = $$$config.resolveSingletonInstance, acquireSingletonInstance = $$$config.acquireSingletonInstance, releaseSingletonInstance = $$$config.releaseSingletonInstance, isHostSingletonType = $$$config.isHostSingletonType, isSingletonScope = $$$config.isSingletonScope, valueStack = [], index$jscomp$0 = -1, emptyContextObject = {}, clz32 = Math.clz32 ? Math.clz32 : clz32Fallback, log$1 = Math.log, LN2 = Math.LN2, nextTransitionUpdateLane = 256, nextTransitionDeferredLane = 262144, nextRetryLane = 4194304, scheduleCallback$3 = Scheduler.unstable_scheduleCallback, cancelCallback$1 = Scheduler.unstable_cancelCallback, shouldYield = Scheduler.unstable_shouldYield, requestPaint = Scheduler.unstable_requestPaint, now2 = Scheduler.unstable_now, ImmediatePriority = Scheduler.unstable_ImmediatePriority, UserBlockingPriority = Scheduler.unstable_UserBlockingPriority, NormalPriority$1 = Scheduler.unstable_NormalPriority, IdlePriority = Scheduler.unstable_IdlePriority, log2 = Scheduler.log, unstable_setDisableYieldValue = Scheduler.unstable_setDisableYieldValue, rendererID = null, injectedHook = null, globalClientIdCounter$1 = 0, objectIs = "function" === typeof Object.is ? Object.is : is, reportGlobalError = "function" === typeof reportError ? reportError : function(error) {
          if ("object" === typeof window && "function" === typeof window.ErrorEvent) {
            var event = new window.ErrorEvent("error", {
              bubbles: true,
              cancelable: true,
              message: "object" === typeof error && null !== error && "string" === typeof error.message ? String(error.message) : String(error),
              error
            });
            if (!window.dispatchEvent(event)) return;
          } else if ("object" === typeof process && "function" === typeof process.emit) {
            process.emit("uncaughtException", error);
            return;
          }
          console.error(error);
        }, hasOwnProperty = Object.prototype.hasOwnProperty, prefix, suffix, reentry = false, CapturedStacks = /* @__PURE__ */ new WeakMap(), forkStack = [], forkStackIndex = 0, treeForkProvider = null, treeForkCount = 0, idStack = [], idStackIndex = 0, treeContextProvider = null, treeContextId = 1, treeContextOverflow = "", contextStackCursor = createCursor(null), contextFiberStackCursor = createCursor(null), rootInstanceStackCursor = createCursor(null), hostTransitionProviderCursor = createCursor(null), hydrationParentFiber = null, nextHydratableInstance = null, isHydrating = false, hydrationErrors = null, rootOrSingletonContext = false, HydrationMismatchException = Error(formatProdErrorMessage(519)), valueCursor = createCursor(null), currentlyRenderingFiber$1 = null, lastContextDependency = null, AbortControllerLocal = "undefined" !== typeof AbortController ? AbortController : function() {
          var listeners = [], signal = this.signal = {
            aborted: false,
            addEventListener: function(type, listener) {
              listeners.push(listener);
            }
          };
          this.abort = function() {
            signal.aborted = true;
            listeners.forEach(function(listener) {
              return listener();
            });
          };
        }, scheduleCallback$2 = Scheduler.unstable_scheduleCallback, NormalPriority = Scheduler.unstable_NormalPriority, CacheContext = {
          $$typeof: REACT_CONTEXT_TYPE,
          Consumer: null,
          Provider: null,
          _currentValue: null,
          _currentValue2: null,
          _threadCount: 0
        }, entangledTransitionTypes = null, firstScheduledRoot = null, lastScheduledRoot = null, didScheduleMicrotask = false, mightHavePendingSyncWork = false, isFlushingWork = false, currentEventTransitionLane = 0, currentEntangledListeners = null, currentEntangledPendingCount = 0, currentEntangledLane = 0, currentEntangledActionThenable = null, prevOnStartTransitionFinish = ReactSharedInternals.S;
        ReactSharedInternals.S = function(transition, returnValue) {
          globalMostRecentTransitionTime = now2();
          "object" === typeof returnValue && null !== returnValue && "function" === typeof returnValue.then && entangleAsyncAction(transition, returnValue);
          if (null !== entangledTransitionTypes)
            for (var root2 = firstScheduledRoot; null !== root2; )
              queueTransitionTypes(root2, entangledTransitionTypes), root2 = root2.next;
          root2 = transition.types;
          if (null !== root2) {
            for (var root$19 = firstScheduledRoot; null !== root$19; )
              queueTransitionTypes(root$19, root2), root$19 = root$19.next;
            if (0 !== currentEntangledLane) {
              root$19 = entangledTransitionTypes;
              null === root$19 && (root$19 = entangledTransitionTypes = []);
              for (var i = 0; i < root2.length; i++) {
                var transitionType = root2[i];
                -1 === root$19.indexOf(transitionType) && root$19.push(transitionType);
              }
            }
          }
          null !== prevOnStartTransitionFinish && prevOnStartTransitionFinish(transition, returnValue);
        };
        var resumedCache = createCursor(null), SuspenseException = Error(formatProdErrorMessage(460)), SuspenseyCommitException = Error(formatProdErrorMessage(474)), SuspenseActionException = Error(formatProdErrorMessage(542)), noopSuspenseyCommitThenable = { then: function() {
        } }, suspendedThenable = null, thenableState$1 = null, thenableIndexCounter$1 = 0, reconcileChildFibers = createChildReconciler(true), mountChildFibers = createChildReconciler(false), concurrentQueues = [], concurrentQueuesIndex = 0, concurrentlyUpdatedLanes = 0, hasForceUpdate = false, didReadFromEntangledAsyncAction = false, currentTreeHiddenStackCursor = createCursor(null), prevEntangledRenderLanesCursor = createCursor(0), suspenseHandlerStackCursor = createCursor(null), shellBoundary = null, suspenseStackCursor = createCursor(0), renderLanes = 0, currentlyRenderingFiber = null, currentHook = null, workInProgressHook = null, didScheduleRenderPhaseUpdate = false, didScheduleRenderPhaseUpdateDuringThisPass = false, shouldDoubleInvokeUserFnsInHooksDEV = false, localIdCounter = 0, thenableIndexCounter = 0, thenableState = null, globalClientIdCounter = 0, ContextOnlyDispatcher = {
          readContext,
          use,
          useCallback: throwInvalidHookError,
          useContext: throwInvalidHookError,
          useEffect: throwInvalidHookError,
          useImperativeHandle: throwInvalidHookError,
          useLayoutEffect: throwInvalidHookError,
          useInsertionEffect: throwInvalidHookError,
          useMemo: throwInvalidHookError,
          useReducer: throwInvalidHookError,
          useRef: throwInvalidHookError,
          useState: throwInvalidHookError,
          useDebugValue: throwInvalidHookError,
          useDeferredValue: throwInvalidHookError,
          useTransition: throwInvalidHookError,
          useSyncExternalStore: throwInvalidHookError,
          useId: throwInvalidHookError,
          useHostTransitionStatus: throwInvalidHookError,
          useFormState: throwInvalidHookError,
          useActionState: throwInvalidHookError,
          useOptimistic: throwInvalidHookError,
          useMemoCache: throwInvalidHookError,
          useCacheRefresh: throwInvalidHookError,
          useEffectEvent: throwInvalidHookError
        }, HooksDispatcherOnMount = {
          readContext,
          use,
          useCallback: function(callback, deps) {
            mountWorkInProgressHook().memoizedState = [
              callback,
              void 0 === deps ? null : deps
            ];
            return callback;
          },
          useContext: readContext,
          useEffect: mountEffect,
          useImperativeHandle: function(ref, create, deps) {
            deps = null !== deps && void 0 !== deps ? deps.concat([ref]) : null;
            mountEffectImpl(
              4194308,
              4,
              imperativeHandleEffect.bind(null, create, ref),
              deps
            );
          },
          useLayoutEffect: function(create, deps) {
            return mountEffectImpl(4194308, 4, create, deps);
          },
          useInsertionEffect: function(create, deps) {
            mountEffectImpl(4, 2, create, deps);
          },
          useMemo: function(nextCreate, deps) {
            var hook = mountWorkInProgressHook();
            deps = void 0 === deps ? null : deps;
            var nextValue = nextCreate();
            if (shouldDoubleInvokeUserFnsInHooksDEV) {
              setIsStrictModeForDevtools(true);
              try {
                nextCreate();
              } finally {
                setIsStrictModeForDevtools(false);
              }
            }
            hook.memoizedState = [nextValue, deps];
            return nextValue;
          },
          useReducer: function(reducer, initialArg, init) {
            var hook = mountWorkInProgressHook();
            if (void 0 !== init) {
              var initialState = init(initialArg);
              if (shouldDoubleInvokeUserFnsInHooksDEV) {
                setIsStrictModeForDevtools(true);
                try {
                  init(initialArg);
                } finally {
                  setIsStrictModeForDevtools(false);
                }
              }
            } else initialState = initialArg;
            hook.memoizedState = hook.baseState = initialState;
            reducer = {
              pending: null,
              lanes: 0,
              dispatch: null,
              lastRenderedReducer: reducer,
              lastRenderedState: initialState
            };
            hook.queue = reducer;
            reducer = reducer.dispatch = dispatchReducerAction.bind(
              null,
              currentlyRenderingFiber,
              reducer
            );
            return [hook.memoizedState, reducer];
          },
          useRef: function(initialValue) {
            var hook = mountWorkInProgressHook();
            initialValue = { current: initialValue };
            return hook.memoizedState = initialValue;
          },
          useState: function(initialState) {
            initialState = mountStateImpl(initialState);
            var queue = initialState.queue, dispatch = dispatchSetState.bind(
              null,
              currentlyRenderingFiber,
              queue
            );
            queue.dispatch = dispatch;
            return [initialState.memoizedState, dispatch];
          },
          useDebugValue: mountDebugValue,
          useDeferredValue: function(value, initialValue) {
            var hook = mountWorkInProgressHook();
            return mountDeferredValueImpl(hook, value, initialValue);
          },
          useTransition: function() {
            var stateHook = mountStateImpl(false);
            stateHook = startTransition.bind(
              null,
              currentlyRenderingFiber,
              stateHook.queue,
              true,
              false
            );
            mountWorkInProgressHook().memoizedState = stateHook;
            return [false, stateHook];
          },
          useSyncExternalStore: function(subscribe, getSnapshot, getServerSnapshot) {
            var fiber = currentlyRenderingFiber, hook = mountWorkInProgressHook();
            if (isHydrating) {
              if (void 0 === getServerSnapshot)
                throw Error(formatProdErrorMessage(407));
              getServerSnapshot = getServerSnapshot();
            } else {
              getServerSnapshot = getSnapshot();
              if (null === workInProgressRoot)
                throw Error(formatProdErrorMessage(349));
              0 !== (workInProgressRootRenderLanes & 127) || pushStoreConsistencyCheck(fiber, getSnapshot, getServerSnapshot);
            }
            hook.memoizedState = getServerSnapshot;
            var inst = { value: getServerSnapshot, getSnapshot };
            hook.queue = inst;
            mountEffect(subscribeToStore.bind(null, fiber, inst, subscribe), [
              subscribe
            ]);
            fiber.flags |= 2048;
            pushSimpleEffect(
              9,
              { destroy: void 0 },
              updateStoreInstance.bind(
                null,
                fiber,
                inst,
                getServerSnapshot,
                getSnapshot
              ),
              null
            );
            return getServerSnapshot;
          },
          useId: function() {
            var hook = mountWorkInProgressHook(), identifierPrefix = workInProgressRoot.identifierPrefix;
            if (isHydrating) {
              var JSCompiler_inline_result = treeContextOverflow;
              var idWithLeadingBit = treeContextId;
              JSCompiler_inline_result = (idWithLeadingBit & ~(1 << 32 - clz32(idWithLeadingBit) - 1)).toString(32) + JSCompiler_inline_result;
              identifierPrefix = "_" + identifierPrefix + "R_" + JSCompiler_inline_result;
              JSCompiler_inline_result = localIdCounter++;
              0 < JSCompiler_inline_result && (identifierPrefix += "H" + JSCompiler_inline_result.toString(32));
              identifierPrefix += "_";
            } else
              JSCompiler_inline_result = globalClientIdCounter++, identifierPrefix = "_" + identifierPrefix + "r_" + JSCompiler_inline_result.toString(32) + "_";
            return hook.memoizedState = identifierPrefix;
          },
          useHostTransitionStatus,
          useFormState: mountActionState,
          useActionState: mountActionState,
          useOptimistic: function(passthrough) {
            var hook = mountWorkInProgressHook();
            hook.memoizedState = hook.baseState = passthrough;
            var queue = {
              pending: null,
              lanes: 0,
              dispatch: null,
              lastRenderedReducer: null,
              lastRenderedState: null
            };
            hook.queue = queue;
            hook = dispatchOptimisticSetState.bind(
              null,
              currentlyRenderingFiber,
              true,
              queue
            );
            queue.dispatch = hook;
            return [passthrough, hook];
          },
          useMemoCache,
          useCacheRefresh: function() {
            return mountWorkInProgressHook().memoizedState = refreshCache.bind(
              null,
              currentlyRenderingFiber
            );
          },
          useEffectEvent: function(callback) {
            var hook = mountWorkInProgressHook(), ref = { impl: callback };
            hook.memoizedState = ref;
            return function() {
              if (0 !== (executionContext & 2))
                throw Error(formatProdErrorMessage(440));
              return ref.impl.apply(void 0, arguments);
            };
          }
        }, HooksDispatcherOnUpdate = {
          readContext,
          use,
          useCallback: updateCallback,
          useContext: readContext,
          useEffect: updateEffect,
          useImperativeHandle: updateImperativeHandle,
          useInsertionEffect: updateInsertionEffect,
          useLayoutEffect: updateLayoutEffect,
          useMemo: updateMemo,
          useReducer: updateReducer,
          useRef: updateRef,
          useState: function() {
            return updateReducer(basicStateReducer);
          },
          useDebugValue: mountDebugValue,
          useDeferredValue: function(value, initialValue) {
            var hook = updateWorkInProgressHook();
            return updateDeferredValueImpl(
              hook,
              currentHook.memoizedState,
              value,
              initialValue
            );
          },
          useTransition: function() {
            var booleanOrThenable = updateReducer(basicStateReducer)[0], start = updateWorkInProgressHook().memoizedState;
            return [
              "boolean" === typeof booleanOrThenable ? booleanOrThenable : useThenable(booleanOrThenable),
              start
            ];
          },
          useSyncExternalStore: updateSyncExternalStore,
          useId: updateId,
          useHostTransitionStatus,
          useFormState: updateActionState,
          useActionState: updateActionState,
          useOptimistic: function(passthrough, reducer) {
            var hook = updateWorkInProgressHook();
            return updateOptimisticImpl(hook, currentHook, passthrough, reducer);
          },
          useMemoCache,
          useCacheRefresh: updateRefresh,
          useEffectEvent: updateEvent
        }, HooksDispatcherOnRerender = {
          readContext,
          use,
          useCallback: updateCallback,
          useContext: readContext,
          useEffect: updateEffect,
          useImperativeHandle: updateImperativeHandle,
          useInsertionEffect: updateInsertionEffect,
          useLayoutEffect: updateLayoutEffect,
          useMemo: updateMemo,
          useReducer: rerenderReducer,
          useRef: updateRef,
          useState: function() {
            return rerenderReducer(basicStateReducer);
          },
          useDebugValue: mountDebugValue,
          useDeferredValue: function(value, initialValue) {
            var hook = updateWorkInProgressHook();
            return null === currentHook ? mountDeferredValueImpl(hook, value, initialValue) : updateDeferredValueImpl(
              hook,
              currentHook.memoizedState,
              value,
              initialValue
            );
          },
          useTransition: function() {
            var booleanOrThenable = rerenderReducer(basicStateReducer)[0], start = updateWorkInProgressHook().memoizedState;
            return [
              "boolean" === typeof booleanOrThenable ? booleanOrThenable : useThenable(booleanOrThenable),
              start
            ];
          },
          useSyncExternalStore: updateSyncExternalStore,
          useId: updateId,
          useHostTransitionStatus,
          useFormState: rerenderActionState,
          useActionState: rerenderActionState,
          useOptimistic: function(passthrough, reducer) {
            var hook = updateWorkInProgressHook();
            if (null !== currentHook)
              return updateOptimisticImpl(hook, currentHook, passthrough, reducer);
            hook.baseState = passthrough;
            return [passthrough, hook.queue.dispatch];
          },
          useMemoCache,
          useCacheRefresh: updateRefresh,
          useEffectEvent: updateEvent
        }, classComponentUpdater = {
          enqueueSetState: function(inst, payload, callback) {
            inst = inst._reactInternals;
            var lane = requestUpdateLane(), update = createUpdate(lane);
            update.payload = payload;
            void 0 !== callback && null !== callback && (update.callback = callback);
            payload = enqueueUpdate(inst, update, lane);
            null !== payload && (scheduleUpdateOnFiber(payload, inst, lane), entangleTransitions(payload, inst, lane));
          },
          enqueueReplaceState: function(inst, payload, callback) {
            inst = inst._reactInternals;
            var lane = requestUpdateLane(), update = createUpdate(lane);
            update.tag = 1;
            update.payload = payload;
            void 0 !== callback && null !== callback && (update.callback = callback);
            payload = enqueueUpdate(inst, update, lane);
            null !== payload && (scheduleUpdateOnFiber(payload, inst, lane), entangleTransitions(payload, inst, lane));
          },
          enqueueForceUpdate: function(inst, callback) {
            inst = inst._reactInternals;
            var lane = requestUpdateLane(), update = createUpdate(lane);
            update.tag = 2;
            void 0 !== callback && null !== callback && (update.callback = callback);
            callback = enqueueUpdate(inst, update, lane);
            null !== callback && (scheduleUpdateOnFiber(callback, inst, lane), entangleTransitions(callback, inst, lane));
          }
        }, SelectiveHydrationException = Error(formatProdErrorMessage(461)), didReceiveUpdate = false, SUSPENDED_MARKER = {
          dehydrated: null,
          treeContext: null,
          retryLane: 0,
          hydrationErrors: null
        }, viewTransitionMutationContext = false, shouldStartViewTransition = false, appearingViewTransitions = null, viewTransitionCancelableChildren = null, viewTransitionHostInstanceIdx = 0, offscreenSubtreeIsHidden = false, offscreenSubtreeWasHidden = false, offscreenDirectParentIsHidden = false, needsFormReset = false, PossiblyWeakSet = "function" === typeof WeakSet ? WeakSet : Set, nextEffect = null, viewTransitionContextChanged = false, inUpdateViewTransition = false, rootViewTransitionAffected = false, rootViewTransitionNameCanceled = false, hostParent = null, hostParentIsContainer = false, currentHoistableRoot = null, suspenseyCommitFlag = 8192, DefaultAsyncDispatcher = {
          getCacheForType: function(resourceType) {
            var cache = readContext(CacheContext), cacheForType = cache.data.get(resourceType);
            void 0 === cacheForType && (cacheForType = resourceType(), cache.data.set(resourceType, cacheForType));
            return cacheForType;
          },
          cacheSignal: function() {
            return readContext(CacheContext).controller.signal;
          }
        }, COMPONENT_TYPE = 0, HAS_PSEUDO_CLASS_TYPE = 1, ROLE_TYPE = 2, TEST_NAME_TYPE = 3, TEXT_TYPE = 4;
        if ("function" === typeof Symbol && Symbol.for) {
          var symbolFor = Symbol.for;
          COMPONENT_TYPE = symbolFor("selector.component");
          HAS_PSEUDO_CLASS_TYPE = symbolFor("selector.has_pseudo_class");
          ROLE_TYPE = symbolFor("selector.role");
          TEST_NAME_TYPE = symbolFor("selector.test_id");
          TEXT_TYPE = symbolFor("selector.text");
        }
        var PossiblyWeakMap = "function" === typeof WeakMap ? WeakMap : Map, executionContext = 0, workInProgressRoot = null, workInProgress = null, workInProgressRootRenderLanes = 0, workInProgressSuspendedReason = 0, workInProgressThrownValue = null, workInProgressRootDidSkipSuspendedSiblings = false, workInProgressRootIsPrerendering = false, workInProgressRootDidAttachPingListener = false, entangledRenderLanes = 0, workInProgressRootExitStatus = 0, workInProgressRootSkippedLanes = 0, workInProgressRootInterleavedUpdatedLanes = 0, workInProgressRootPingedLanes = 0, workInProgressDeferredLane = 0, workInProgressSuspendedRetryLanes = 0, workInProgressRootConcurrentErrors = null, workInProgressRootRecoverableErrors = null, workInProgressRootDidIncludeRecursiveRenderUpdate = false, globalMostRecentFallbackTime = 0, globalMostRecentTransitionTime = 0, workInProgressRootRenderTargetTime = Infinity, workInProgressTransitions = null, legacyErrorBoundariesThatAlreadyFailed = null, pendingEffectsStatus = 0, pendingEffectsRoot = null, pendingFinishedWork = null, pendingEffectsLanes = 0, pendingEffectsRemainingLanes = 0, pendingPassiveTransitions = null, pendingRecoverableErrors = null, pendingViewTransition = null, pendingViewTransitionEvents = null, pendingTransitionTypes = null, nestedUpdateCount = 0, rootWithNestedUpdates = null;
        exports2.attemptContinuousHydration = function(fiber) {
          if (13 === fiber.tag || 31 === fiber.tag) {
            var root2 = enqueueConcurrentRenderForLane(fiber, 67108864);
            null !== root2 && scheduleUpdateOnFiber(root2, fiber, 67108864);
            markRetryLaneIfNotHydrated(fiber, 67108864);
          }
        };
        exports2.attemptHydrationAtCurrentPriority = function(fiber) {
          if (13 === fiber.tag || 31 === fiber.tag) {
            var lane = requestUpdateLane();
            lane = getBumpedLaneForHydrationByLane(lane);
            var root2 = enqueueConcurrentRenderForLane(fiber, lane);
            null !== root2 && scheduleUpdateOnFiber(root2, fiber, lane);
            markRetryLaneIfNotHydrated(fiber, lane);
          }
        };
        exports2.attemptSynchronousHydration = function(fiber) {
          switch (fiber.tag) {
            case 3:
              fiber = fiber.stateNode;
              if (fiber.current.memoizedState.isDehydrated) {
                var lanes = getHighestPriorityLanes(fiber.pendingLanes);
                if (0 !== lanes) {
                  fiber.pendingLanes |= 2;
                  for (fiber.entangledLanes |= 2; lanes; ) {
                    var lane = 1 << 31 - clz32(lanes);
                    fiber.entanglements[1] |= lane;
                    lanes &= ~lane;
                  }
                  ensureRootIsScheduled(fiber);
                  0 === (executionContext & 6) && (workInProgressRootRenderTargetTime = now2() + 500, flushSyncWorkAcrossRoots_impl(0, false));
                }
              }
              break;
            case 31:
            case 13:
              lanes = enqueueConcurrentRenderForLane(fiber, 2), null !== lanes && scheduleUpdateOnFiber(lanes, fiber, 2), flushSyncWork(), markRetryLaneIfNotHydrated(fiber, 2);
          }
        };
        exports2.batchedUpdates = function(fn, a) {
          return fn(a);
        };
        exports2.createComponentSelector = function(component) {
          return { $$typeof: COMPONENT_TYPE, value: component };
        };
        exports2.createContainer = function(containerInfo, tag, hydrationCallbacks, isStrictMode, concurrentUpdatesByDefaultOverride, identifierPrefix, onUncaughtError, onCaughtError, onRecoverableError, onDefaultTransitionIndicator) {
          return createFiberRoot(
            containerInfo,
            tag,
            false,
            null,
            hydrationCallbacks,
            isStrictMode,
            identifierPrefix,
            null,
            onUncaughtError,
            onCaughtError,
            onRecoverableError,
            onDefaultTransitionIndicator
          );
        };
        exports2.createHasPseudoClassSelector = function(selectors) {
          return { $$typeof: HAS_PSEUDO_CLASS_TYPE, value: selectors };
        };
        exports2.createHydrationContainer = function(initialChildren, callback, containerInfo, tag, hydrationCallbacks, isStrictMode, concurrentUpdatesByDefaultOverride, identifierPrefix, onUncaughtError, onCaughtError, onRecoverableError, onDefaultTransitionIndicator, transitionCallbacks, formState) {
          initialChildren = createFiberRoot(
            containerInfo,
            tag,
            true,
            initialChildren,
            hydrationCallbacks,
            isStrictMode,
            identifierPrefix,
            formState,
            onUncaughtError,
            onCaughtError,
            onRecoverableError,
            onDefaultTransitionIndicator
          );
          initialChildren.context = getContextForSubtree(null);
          containerInfo = initialChildren.current;
          tag = requestUpdateLane();
          tag = getBumpedLaneForHydrationByLane(tag);
          hydrationCallbacks = createUpdate(tag);
          hydrationCallbacks.callback = void 0 !== callback && null !== callback ? callback : null;
          enqueueUpdate(containerInfo, hydrationCallbacks, tag);
          callback = tag;
          initialChildren.current.lanes = callback;
          markRootUpdated$1(initialChildren, callback);
          ensureRootIsScheduled(initialChildren);
          return initialChildren;
        };
        exports2.createPortal = function(children, containerInfo, implementation) {
          var key = 3 < arguments.length && void 0 !== arguments[3] ? arguments[3] : null;
          return {
            $$typeof: REACT_PORTAL_TYPE,
            key: null == key ? null : key === REACT_OPTIMISTIC_KEY ? REACT_OPTIMISTIC_KEY : "" + key,
            children,
            containerInfo,
            implementation
          };
        };
        exports2.createRoleSelector = function(role) {
          return { $$typeof: ROLE_TYPE, value: role };
        };
        exports2.createTestNameSelector = function(id) {
          return { $$typeof: TEST_NAME_TYPE, value: id };
        };
        exports2.createTextSelector = function(text) {
          return { $$typeof: TEXT_TYPE, value: text };
        };
        exports2.defaultOnCaughtError = function(error) {
          console.error(error);
        };
        exports2.defaultOnRecoverableError = function(error) {
          reportGlobalError(error);
        };
        exports2.defaultOnUncaughtError = function(error) {
          reportGlobalError(error);
        };
        exports2.deferredUpdates = function(fn) {
          var prevTransition = ReactSharedInternals.T, previousPriority = getCurrentUpdatePriority();
          try {
            return setCurrentUpdatePriority(32), ReactSharedInternals.T = null, fn();
          } finally {
            setCurrentUpdatePriority(previousPriority), ReactSharedInternals.T = prevTransition;
          }
        };
        exports2.discreteUpdates = function(fn, a, b, c, d) {
          var prevTransition = ReactSharedInternals.T, previousPriority = getCurrentUpdatePriority();
          try {
            return setCurrentUpdatePriority(2), ReactSharedInternals.T = null, fn(a, b, c, d);
          } finally {
            setCurrentUpdatePriority(previousPriority), ReactSharedInternals.T = prevTransition, 0 === executionContext && (workInProgressRootRenderTargetTime = now2() + 500);
          }
        };
        exports2.findAllNodes = findAllNodes;
        exports2.findBoundingRects = function(hostRoot, selectors) {
          if (!supportsTestSelectors) throw Error(formatProdErrorMessage(363));
          selectors = findAllNodes(hostRoot, selectors);
          hostRoot = [];
          for (var i = 0; i < selectors.length; i++)
            hostRoot.push(getBoundingRect(selectors[i]));
          for (selectors = hostRoot.length - 1; 0 < selectors; selectors--) {
            i = hostRoot[selectors];
            for (var targetLeft = i.x, targetRight = targetLeft + i.width, targetTop = i.y, targetBottom = targetTop + i.height, j = selectors - 1; 0 <= j; j--)
              if (selectors !== j) {
                var otherRect = hostRoot[j], otherLeft = otherRect.x, otherRight = otherLeft + otherRect.width, otherTop = otherRect.y, otherBottom = otherTop + otherRect.height;
                if (targetLeft >= otherLeft && targetTop >= otherTop && targetRight <= otherRight && targetBottom <= otherBottom) {
                  hostRoot.splice(selectors, 1);
                  break;
                } else if (!(targetLeft !== otherLeft || i.width !== otherRect.width || otherBottom < targetTop || otherTop > targetBottom)) {
                  otherTop > targetTop && (otherRect.height += otherTop - targetTop, otherRect.y = targetTop);
                  otherBottom < targetBottom && (otherRect.height = targetBottom - otherTop);
                  hostRoot.splice(selectors, 1);
                  break;
                } else if (!(targetTop !== otherTop || i.height !== otherRect.height || otherRight < targetLeft || otherLeft > targetRight)) {
                  otherLeft > targetLeft && (otherRect.width += otherLeft - targetLeft, otherRect.x = targetLeft);
                  otherRight < targetRight && (otherRect.width = targetRight - otherLeft);
                  hostRoot.splice(selectors, 1);
                  break;
                }
              }
          }
          return hostRoot;
        };
        exports2.findHostInstance = findHostInstance;
        exports2.findHostInstanceWithNoPortals = function(fiber) {
          fiber = findCurrentFiberUsingSlowPath(fiber);
          fiber = null !== fiber ? findCurrentHostFiberWithNoPortalsImpl(fiber) : null;
          return null === fiber ? null : getPublicInstance(fiber.stateNode);
        };
        exports2.findHostInstanceWithWarning = function(component) {
          return findHostInstance(component);
        };
        exports2.flushPassiveEffects = flushPendingEffects;
        exports2.flushSyncFromReconciler = function(fn) {
          var prevExecutionContext = executionContext;
          executionContext |= 1;
          var prevTransition = ReactSharedInternals.T, previousPriority = getCurrentUpdatePriority();
          try {
            if (setCurrentUpdatePriority(2), ReactSharedInternals.T = null, fn)
              return fn();
          } finally {
            setCurrentUpdatePriority(previousPriority), ReactSharedInternals.T = prevTransition, executionContext = prevExecutionContext, 0 === (executionContext & 6) && flushSyncWorkAcrossRoots_impl(0, false);
          }
        };
        exports2.flushSyncWork = flushSyncWork;
        exports2.focusWithin = function(hostRoot, selectors) {
          if (!supportsTestSelectors) throw Error(formatProdErrorMessage(363));
          hostRoot = findFiberRootForHostRoot(hostRoot);
          selectors = findPaths(hostRoot, selectors);
          selectors = Array.from(selectors);
          for (hostRoot = 0; hostRoot < selectors.length; ) {
            var fiber = selectors[hostRoot++], tag = fiber.tag;
            if (!isHiddenSubtree(fiber)) {
              if ((5 === tag || 26 === tag || 27 === tag) && setFocusIfFocusable(fiber.stateNode))
                return true;
              for (fiber = fiber.child; null !== fiber; )
                selectors.push(fiber), fiber = fiber.sibling;
            }
          }
          return false;
        };
        exports2.getFindAllNodesFailureDescription = function(hostRoot, selectors) {
          if (!supportsTestSelectors) throw Error(formatProdErrorMessage(363));
          var maxSelectorIndex = 0, matchedNames = [];
          hostRoot = [findFiberRootForHostRoot(hostRoot), 0];
          for (var index = 0; index < hostRoot.length; ) {
            var fiber = hostRoot[index++], tag = fiber.tag, selectorIndex = hostRoot[index++], selector = selectors[selectorIndex];
            if (5 !== tag && 26 !== tag && 27 !== tag || !isHiddenSubtree(fiber)) {
              if (matchSelector(fiber, selector) && (matchedNames.push(selectorToString(selector)), selectorIndex++, selectorIndex > maxSelectorIndex && (maxSelectorIndex = selectorIndex)), selectorIndex < selectors.length)
                for (fiber = fiber.child; null !== fiber; )
                  hostRoot.push(fiber, selectorIndex), fiber = fiber.sibling;
            }
          }
          if (maxSelectorIndex < selectors.length) {
            for (hostRoot = []; maxSelectorIndex < selectors.length; maxSelectorIndex++)
              hostRoot.push(selectorToString(selectors[maxSelectorIndex]));
            return "findAllNodes was able to match part of the selector:\n  " + (matchedNames.join(" > ") + "\n\nNo matching component was found for:\n  ") + hostRoot.join(" > ");
          }
          return null;
        };
        exports2.getPublicRootInstance = function(container2) {
          container2 = container2.current;
          if (!container2.child) return null;
          switch (container2.child.tag) {
            case 27:
            case 5:
              return getPublicInstance(container2.child.stateNode);
            default:
              return container2.child.stateNode;
          }
        };
        exports2.injectIntoDevTools = function() {
          var internals = {
            bundleType: 0,
            version: rendererVersion,
            rendererPackageName,
            currentDispatcherRef: ReactSharedInternals,
            reconcilerVersion: "19.3.0"
          };
          null !== extraDevToolsConfig && (internals.rendererConfig = extraDevToolsConfig);
          if ("undefined" === typeof __REACT_DEVTOOLS_GLOBAL_HOOK__) internals = false;
          else {
            var hook = __REACT_DEVTOOLS_GLOBAL_HOOK__;
            if (hook.isDisabled || !hook.supportsFiber) internals = true;
            else {
              try {
                rendererID = hook.inject(internals), injectedHook = hook;
              } catch (err) {
              }
              internals = hook.checkDCE ? true : false;
            }
          }
          return internals;
        };
        exports2.isAlreadyRendering = function() {
          return 0 !== (executionContext & 6);
        };
        exports2.observeVisibleRects = function(hostRoot, selectors, callback, options) {
          if (!supportsTestSelectors) throw Error(formatProdErrorMessage(363));
          hostRoot = findAllNodes(hostRoot, selectors);
          var disconnect = setupIntersectionObserver(
            hostRoot,
            callback,
            options
          ).disconnect;
          return {
            disconnect: function() {
              disconnect();
            }
          };
        };
        exports2.shouldError = function() {
          return null;
        };
        exports2.shouldSuspend = function() {
          return false;
        };
        exports2.startHostTransition = function(formFiber, pendingState, action, formData) {
          if (5 !== formFiber.tag) throw Error(formatProdErrorMessage(476));
          var queue = ensureFormComponentIsStateful(formFiber).queue;
          startTransition(
            formFiber,
            queue,
            pendingState,
            NotPendingTransition,
            null === action ? noop : function() {
              var stateHook = ensureFormComponentIsStateful(formFiber);
              null === stateHook.next && (stateHook = formFiber.alternate.memoizedState);
              dispatchSetStateInternal(
                formFiber,
                stateHook.next.queue,
                {},
                requestUpdateLane()
              );
              return action(formData);
            }
          );
        };
        exports2.updateContainer = function(element, container2, parentComponent, callback) {
          var current = container2.current, lane = requestUpdateLane();
          updateContainerImpl(
            current,
            lane,
            element,
            container2,
            parentComponent,
            callback
          );
          return lane;
        };
        exports2.updateContainerSync = function(element, container2, parentComponent, callback) {
          updateContainerImpl(
            container2.current,
            2,
            element,
            container2,
            parentComponent,
            callback
          );
          return 2;
        };
        return exports2;
      };
      module.exports.default = module.exports;
      Object.defineProperty(module.exports, "__esModule", { value: true });
    }
  });

  // node_modules/react-reconciler/index.js
  var require_react_reconciler = __commonJS({
    "node_modules/react-reconciler/index.js"(exports, module) {
      "use strict";
      if (true) {
        module.exports = require_react_reconciler_production();
      } else {
        module.exports = null;
      }
    }
  });

  // node_modules/react/cjs/react-jsx-runtime.production.js
  var require_react_jsx_runtime_production = __commonJS({
    "node_modules/react/cjs/react-jsx-runtime.production.js"(exports) {
      "use strict";
      var REACT_ELEMENT_TYPE = /* @__PURE__ */ Symbol.for("react.transitional.element");
      var REACT_FRAGMENT_TYPE = /* @__PURE__ */ Symbol.for("react.fragment");
      function jsxProd(type, config, maybeKey) {
        var key = null;
        void 0 !== maybeKey && (key = "" + maybeKey);
        void 0 !== config.key && (key = "" + config.key);
        if ("key" in config) {
          maybeKey = {};
          for (var propName in config)
            "key" !== propName && (maybeKey[propName] = config[propName]);
        } else maybeKey = config;
        config = maybeKey.ref;
        return {
          $$typeof: REACT_ELEMENT_TYPE,
          type,
          key,
          ref: void 0 !== config ? config : null,
          props: maybeKey
        };
      }
      exports.Fragment = REACT_FRAGMENT_TYPE;
      exports.jsx = jsxProd;
      exports.jsxs = jsxProd;
    }
  });

  // node_modules/react/jsx-runtime.js
  var require_jsx_runtime = __commonJS({
    "node_modules/react/jsx-runtime.js"(exports, module) {
      "use strict";
      if (true) {
        module.exports = require_react_jsx_runtime_production();
      } else {
        module.exports = null;
      }
    }
  });

  // bench/engines/react-commit.jsx
  var import_react = __toESM(require_react());
  var import_react_reconciler = __toESM(require_react_reconciler());
  var import_jsx_runtime = __toESM(require_jsx_runtime());
  var log = typeof print === "function" ? print : (s) => console.log(s);
  var now = typeof performance !== "undefined" && performance.now ? () => performance.now() : typeof preciseTime === "function" ? () => preciseTime() * 1e3 : () => Date.now();
  var nodes = 0;
  var updates = 0;
  var prio = 32;
  var hostConfig = {
    supportsMutation: true,
    isPrimaryRenderer: true,
    noTimeout: -1,
    createInstance(type, props) {
      nodes++;
      return { type, props, children: [] };
    },
    createTextInstance(text) {
      return { text };
    },
    appendInitialChild(p, c) {
      p.children.push(c);
    },
    appendChild(p, c) {
      p.children.push(c);
    },
    appendChildToContainer(p, c) {
      p.children.push(c);
    },
    insertBefore(p, c, b) {
      p.children.splice(p.children.indexOf(b), 0, c);
    },
    insertInContainerBefore(p, c, b) {
      p.children.splice(p.children.indexOf(b), 0, c);
    },
    removeChild(p, c) {
      p.children.splice(p.children.indexOf(c), 1);
    },
    removeChildFromContainer(p, c) {
      p.children.splice(p.children.indexOf(c), 1);
    },
    clearContainer(c) {
      c.children.length = 0;
    },
    finalizeInitialChildren() {
      return false;
    },
    commitUpdate(inst, type, oldProps, newProps) {
      updates++;
      inst.props = newProps;
    },
    commitTextUpdate(t, o, n) {
      t.text = n;
    },
    shouldSetTextContent() {
      return false;
    },
    getRootHostContext() {
      return {};
    },
    getChildHostContext(c) {
      return c;
    },
    getPublicInstance(i) {
      return i;
    },
    prepareForCommit() {
      return null;
    },
    resetAfterCommit() {
    },
    preparePortalMount() {
    },
    scheduleTimeout: (fn, ms) => typeof setTimeout === "function" ? setTimeout(fn, ms) : 0,
    cancelTimeout: (id) => typeof clearTimeout === "function" ? clearTimeout(id) : void 0,
    getCurrentUpdatePriority() {
      return prio;
    },
    setCurrentUpdatePriority(p) {
      prio = p;
    },
    resolveUpdatePriority() {
      return prio;
    },
    getInstanceFromNode() {
      return null;
    },
    beforeActiveInstanceBlur() {
    },
    afterActiveInstanceBlur() {
    },
    prepareScopeUpdate() {
    },
    getInstanceFromScope() {
      return null;
    },
    detachDeletedInstance() {
    },
    maySuspendCommit() {
      return false;
    },
    preloadInstance() {
      return true;
    },
    startSuspendingCommit() {
    },
    suspendInstance() {
    },
    waitForCommitToBeReady() {
      return null;
    },
    resetFormInstance() {
    },
    shouldAttemptEagerTransition() {
      return false;
    },
    trackSchedulerEvent() {
    },
    resolveEventType() {
      return null;
    },
    resolveEventTimeStamp() {
      return -1.1;
    },
    requestPostPaintCallback() {
    },
    NotPendingTransition: null,
    HostTransitionContext: import_react.default.createContext(null)
  };
  var t0 = now();
  var R = (0, import_react_reconciler.default)(hostConfig);
  var container = { children: [] };
  var root = R.createContainer(container, 0, null, false, null, "", (e) => log("err " + e), (e) => log("err " + e), (e) => log("err " + e), null);
  var N = 500;
  var setTick;
  var cellStyle = { width: 30, height: 20, margin: 1 };
  function Cell({ hue }) {
    return /* @__PURE__ */ (0, import_jsx_runtime.jsx)("view", { style: [cellStyle, { backgroundColor: `hsl(${hue}, 60%, 50%)` }] });
  }
  var MemoCell = import_react.default.memo(Cell);
  function App() {
    const [state, set] = (0, import_react.useState)({ tick: 0, mode: "none", memo: false });
    setTick = set;
    const cells = [];
    const C = state.memo ? MemoCell : Cell;
    for (let i = 0; i < N; i++) {
      const changed = state.mode === "all" || state.mode === "tenth" && i % 10 === 0;
      const hue = changed ? (i * 7 + state.tick * 13) % 360 : i * 7 % 360;
      cells.push(/* @__PURE__ */ (0, import_jsx_runtime.jsx)(C, { hue }, i));
    }
    return /* @__PURE__ */ (0, import_jsx_runtime.jsx)("view", { children: cells });
  }
  var render = (el) => {
    R.updateContainerSync(el, root, null, null);
    R.flushSyncWork();
  };
  log(`[bench] init (React + reconciler setup): ${(now() - t0).toFixed(3)} ms`);
  function time(label, it, fn) {
    for (let k = 0; k < 3; k++) fn(k);
    const s = now();
    const u0 = updates;
    for (let k = 0; k < it; k++) fn(k);
    log(`[bench] ${label}: ${((now() - s) / it).toFixed(3)} ms/commit, ${(updates - u0) / it} host updates/commit`);
  }
  time("mount 500 views", 20, () => {
    render(null);
    render(/* @__PURE__ */ (0, import_jsx_runtime.jsx)(App, {}));
  });
  var tick = 1;
  for (const memo of [false, true]) {
    R.flushSyncFromReconciler(() => setTick({ tick: tick++, mode: "none", memo }));
    for (const mode of ["none", "tenth", "all"]) {
      time(`update ${memo ? "memo" : "plain"} (${mode})`, 100, () => R.flushSyncFromReconciler(() => setTick({ tick: tick++, mode, memo })));
    }
  }
  log(`[bench] total: ${(now() - t0).toFixed(1)} ms, host nodes created ${nodes}`);
})();
/*! Bundled license information:

react/cjs/react.production.js:
  (**
   * @license React
   * react.production.js
   *
   * Copyright (c) Meta Platforms, Inc. and affiliates.
   *
   * This source code is licensed under the MIT license found in the
   * LICENSE file in the root directory of this source tree.
   *)

scheduler/cjs/scheduler.production.js:
  (**
   * @license React
   * scheduler.production.js
   *
   * Copyright (c) Meta Platforms, Inc. and affiliates.
   *
   * This source code is licensed under the MIT license found in the
   * LICENSE file in the root directory of this source tree.
   *)

react-reconciler/cjs/react-reconciler.production.js:
  (**
   * @license React
   * react-reconciler.production.js
   *
   * Copyright (c) Meta Platforms, Inc. and affiliates.
   *
   * This source code is licensed under the MIT license found in the
   * LICENSE file in the root directory of this source tree.
   *)

react/cjs/react-jsx-runtime.production.js:
  (**
   * @license React
   * react-jsx-runtime.production.js
   *
   * Copyright (c) Meta Platforms, Inc. and affiliates.
   *
   * This source code is licensed under the MIT license found in the
   * LICENSE file in the root directory of this source tree.
   *)
*/
