// Image node behaviour: source resolution, intrinsic size measurement and
// painting with resizeMode / tintColor.
#pragma once

#include <string>

#include "core/node.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"

class SkCanvas;
struct SkRect;
class SkRRect;

namespace jsui {
namespace image {

// Per-node state kept in Node::imageCache.
struct NodeCache : Node::Cache {
  std::string url;           // resolved URL ("" = no source)
  float sourceWidth = 0;     // source={{uri, width, height}} (react-native-web sizes the box from these)
  float sourceHeight = 0;
  float scale = 1;           // source density (scale: 2 => @2x asset)
  bool measuredLoaded = false;  // the last measure saw a decoded image
};

// Installs the intrinsic-size measure function on Image nodes.
void installMeasure(Node* node);
// Resolves props.source into the node's NodeCache (cheap when unchanged).
NodeCache* ensure(Node* node);
// Starts loading the node's image now, so it is ready (and sized) by the
// first paint instead of popping in a frame later.
void request(Node* node);
// True when the node's image has finished loading (decoded or failed), or
// it has no source; never starts a load.
bool settled(Node* node);
// Appends to `key` what paint() draws besides the box and clip: the decoded
// image's identity, resize mode, density and tint. False when paint() would
// draw nothing (no source, not loaded yet).
bool paintKey(Node* node, std::string* key);
// Paints the image into `box` (the padding box), clipped to `clip`.
void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip);

}  // namespace image
}  // namespace jsui
