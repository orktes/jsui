// SKELETON painter: solid backgrounds only.
#include "render/painter.h"

#include "core/document.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

namespace jsui {

namespace {
void paintNode(Node* n, SkCanvas* c) {
  if (!n->yoga()) return;
  const auto& s = n->style();
  Rect r = n->layoutRect();
  c->save();
  c->translate(r.x, r.y);
  if (!s.backgroundColor.transparent()) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor4f({s.backgroundColor.r, s.backgroundColor.g, s.backgroundColor.b, s.backgroundColor.a});
    c->drawRect(SkRect::MakeWH(r.width, r.height), p);
  }
  for (Node* ch : n->children()) paintNode(ch, c);
  c->restore();
}
}  // namespace

void paintDocument(Document& doc, SkCanvas* canvas) {
  canvas->save();
  canvas->scale(doc.pixelRatio(), doc.pixelRatio());
  paintNode(doc.root(), canvas);
  canvas->restore();
}

}  // namespace jsui
