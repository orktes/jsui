// Owns all nodes of one runtime, the root container and the layout pass.
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "core/node.h"

namespace jsui {

class FontManager;
class ImageCache;

class Document {
 public:
  Document();
  ~Document();

  // The root container (id 1). The reconciler appends its top level here.
  Node* root() const { return root_; }

  Node* createNode(NodeType type);
  Node* get(uint32_t id) const;
  // Destroys a detached node and its subtree.
  void destroyNode(Node* node);
  // Destroys everything except the root (used on reload).
  void clear();

  void setViewport(float width, float height, float pixelRatio);
  float width() const { return width_; }
  float height() const { return height_; }
  float pixelRatio() const { return pixelRatio_; }

  // Runs Yoga if anything is dirty. Returns true if layout changed.
  bool layout();
  void markLayoutDirty() { layoutDirty_ = true; paintDirty_ = true; }
  void markPaintDirty() { paintDirty_ = true; }
  bool paintDirty() const { return paintDirty_; }
  void clearPaintDirty() { paintDirty_ = false; }

  // Yoga configuration shared by this document's nodes (one per document:
  // no global state).
  YGConfigRef yogaConfig() const { return yogaConfig_; }

  // Services used by text measurement and painting. Set by the Runtime.
  FontManager* fonts = nullptr;
  ImageCache* images = nullptr;

  // Called after layout for nodes whose props contain `__events` with
  // "layout" when their layoutRect changed.
  std::function<void(Node*, const Rect&)> onLayout;
  // Called for every node right before it is destroyed.
  std::function<void(Node*)> onNodeDestroyed;

 private:
  void destroyRecursive(Node* node);
  void reportLayout(Node* node);

  std::unordered_map<uint32_t, std::unique_ptr<Node>> nodes_;
  std::unordered_map<uint32_t, Rect> reportedLayout_;
  Node* root_ = nullptr;
  uint32_t nextId_ = 1;
  float width_ = 0, height_ = 0, pixelRatio_ = 1;
  bool layoutDirty_ = true, paintDirty_ = true;
  uint32_t fontGeneration_ = 0;  // FontManager::generation the text caches were built with
  YGConfigRef yogaConfig_ = nullptr;
};

}  // namespace jsui
