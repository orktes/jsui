// VectorView (NodeType::Vector): one native node that paints a tree of
// vector drawing commands, the backend of react-native-svg's <Svg> and
// react-native-skia's <Canvas> (packages/react/src/vector/).
//
// The drawing elements (<Circle>, <Path>, <G>, <LinearGradient>...) are not
// views: @orktes/jsui-react keeps them as JS-side host instances and, when a
// commit changed any of them, serializes the owner's subtree into the
// `commands` prop of this node (one object tree, see the format below). The
// tree is parsed once per change (paths from SVG strings, kept per element);
// painting walks it with SkPath / SkPaint into the view's content box,
// scaled by the viewBox (`vb`, `align`, `slice`: preserveAspectRatio). Only
// core Skia is used (no SVG module), so every renderer draws it the same.
//
// Animated drawing props: an element with Animated values gets a key `k`
// and its values travel as paint-only style keys `draw:<k>.<prop>` on this
// node (kGroupDraw), which Animated writes with patchStyle (JS driver) or
// natively (useNativeDriver), with no React render and no re-serialization:
// painting reads them over the element's own props.
//
// Format (all fields optional unless noted):
//   commands: {mode: 'svg' | 'skia', vb: [x, y, w, h], align: 'xMidYMid' |
//              'none' | 'x{Min,Mid,Max}Y{Min,Mid,Max}', slice: bool,
//              color: currentColor, c: [element]}
//   element:  {t (required): 'g' | 'path' | 'rect' | 'circle' | 'ellipse' |
//              'oval' | 'line' | 'poly' | 'points' | 'text' | 'image' |
//              'fill', k: anim key, c: [element], geometry props, paint props}
// See docs/ARCHITECTURE.md "Vector drawing (Svg, Canvas)" for the props.
#pragma once

#include "core/node.h"
#include "core/value.h"

class SkCanvas;
struct SkRect;
class SkRRect;

namespace jsui {
namespace vector {

// Node::setProps: re-parses `commands` when it changed.
void propsChanged(Node* node, const Value& old);
// Paints the commands into `box` (the content box), clipped to `clip`.
void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip);
// Number of elements in the parsed tree (tests).
size_t elementCount(Node* node);

}  // namespace vector
}  // namespace jsui
