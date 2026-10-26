// Text layout: a paragraph-root Text node (with nested Text spans and RawText
// leaves) becomes one skparagraph Paragraph. The paragraph is built once per
// content/style change and cached in Node::textCache; Yoga measures it through
// a measure function and the painter draws it at the content box.
#pragma once

#include <yoga/Yoga.h>

#include <vector>

#include "include/core/SkRect.h"

class SkCanvas;
class SkMatrix;
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

// Shapes the paragraph of a Text node ahead of its first measure (the same
// build and unconstrained first layout the measure would do). Returns false
// when there was nothing to do (already shaped, or no font manager).
bool prepareText(Node* node);

// Paints a paragraph-root Text node whose content box starts at (x, y).
// `layoutSpace` is given when a transform applies to the canvas: it maps the
// canvas coordinates to device pixels without the transforms (the space
// boxes pixel-snap in), and the baseline then snaps there, not on the device.
void paintText(Node* node, SkCanvas* canvas, float x, float y, float width, float height,
               const SkMatrix* layoutSpace = nullptr);
// The local rect that paintText at (x, y) paints inside (ink and text
// shadows included); false without a paragraph or with textReveal.
bool paintBounds(Node* node, float x, float y, float width, SkRect* out);
// The number of reveal units (ComputedStyle::textRevealMode: graphemes or
// words) in a paragraph-root Text's text; 0 without text.
int revealUnitCount(Node* node);
// Paints a TextInput's value (or placeholder) inside its content box.
void paintTextInput(Node* node, SkCanvas* canvas, float x, float y, float width, float height,
                    const SkMatrix* layoutSpace = nullptr);

// --- TextInput editing geometry (see text/text_edit.h). Coordinates are
// the node's border-box local px (after its last layout); indices are value
// indices (UTF-16 units). The content scroll of the edit state is applied.

// Caret rectangle for a caret before `index`.
SkRect inputCaretRect(Node* n, int index);
// Index nearest to a point (click to place the caret).
int inputIndexAt(Node* n, float x, float y);
// Selection highlight boxes for [start, end).
void inputSelectionRects(Node* n, int start, int end, std::vector<SkRect>* out);
// Up (dir < 0) / Down (dir > 0) one visual line. `goalX` keeps the column
// across repeated moves (in: used when hasGoal; out: the column).
int inputVerticalMove(Node* n, int index, int dir, float* goalX, bool hasGoal);
// Start / end of the visual line containing `index` (single line: text).
int inputLineEdge(Node* n, int index, bool end);
// Base direction of the input's text is right-to-left.
bool inputRtl(Node* n);
// Scrolls the content so the caret is visible; true if the offset changed.
bool inputScrollToCaret(Node* n);
// Focused, caret in its "on" blink phase, collapsed selection, editable and
// not caretHidden.
bool inputCaretVisible(Node* n);
// Incremental repaint: compares the caret (visibility + rect) with what was
// last reported. When it changed, returns true with the old and new rects
// (border-box coords, empty when hidden) and records the new state.
bool inputCaretDamage(Node* n, SkRect* before, SkRect* after);

// Whether the paragraph of Text root `n` (already built) can follow a change
// of its own `color` alone without being rebuilt: no span sets a color of its
// own, so every run was built with the root's. The painter then draws it in
// the current color (a hover / state color transition reshapes nothing).
bool recolorable(Node* n);

// Drops every cached paragraph under `root` and re-dirties Text measurement
// (after fonts were registered).
void invalidateAll(Node* root);

}  // namespace text
}  // namespace jsui
