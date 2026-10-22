// Text layout: a paragraph-root Text node (with nested Text spans and RawText
// leaves) becomes one skparagraph Paragraph. The paragraph is built once per
// content/style change and cached in Node::textCache; Yoga measures it through
// a measure function and the painter draws it at the content box.
#pragma once

#include <yoga/Yoga.h>

class SkCanvas;
namespace skia::textlayout {
class Paragraph;
}

namespace jsui {

class Node;

namespace text {

// Installs the Yoga measure function on Text / TextInput nodes.
void installMeasure(Node* node);

// The paragraph of a paragraph-root Text (or TextInput) node laid out at
// `width` (the content box width). nullptr when there is no text.
skia::textlayout::Paragraph* layoutParagraph(Node* node, float width);

// Paints a paragraph-root Text node whose content box starts at (x, y).
void paintText(Node* node, SkCanvas* canvas, float x, float y, float width, float height);
// Paints a TextInput's value (or placeholder) inside its content box.
void paintTextInput(Node* node, SkCanvas* canvas, float x, float y, float width, float height);

// Drops every cached paragraph under `root` and re-dirties Text measurement
// (after fonts were registered).
void invalidateAll(Node* root);

}  // namespace text
}  // namespace jsui
