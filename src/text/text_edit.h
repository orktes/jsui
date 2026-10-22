// Editing state of a TextInput: the native copy of its text, the selection,
// scroll offset, caret blink phase and undo history.
//
// The runtime owns the text the user edits (like a native text field in
// React Native): keys, text input and pointer presses edit it directly and
// the change is reported to JS ('change' / 'selectionchange' events). JS
// controls it through props:
//   value                 adopted when it differs from the native text and
//   mostRecentEventCount  is >= the native change count (JS has seen every
//                         native edit; older renders are stale and ignored).
//                         Without a count, `value` is adopted when it changed.
//   defaultValue          initial text when `value` is absent
// and through the bridge (setTextInputSelection). Indices are UTF-16 code
// units, like JS strings.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/node.h"
#include "include/core/SkRect.h"

namespace jsui::edit {

struct UndoEntry {
  std::u16string value;
  int anchor = 0, focus = 0;
};

struct EditState : Node::Cache {
  std::u16string value;
  int anchor = 0;  // selection anchor (fixed end while extending)
  int focus = 0;   // caret (moving end)
  int start() const { return anchor < focus ? anchor : focus; }
  int end() const { return anchor < focus ? focus : anchor; }
  bool collapsed() const { return anchor == focus; }

  int eventCount = 0;  // native edits so far (RN's eventCount)
  std::string valueProp;  // props.value at the last sync
  bool focused = false;
  // Caret blink: on for 500 ms, off for 500 ms, restarting at blinkBase.
  bool caretOn = true;
  double blinkBase = 0;
  // Content scroll (single line: x, multiline: y), content-box px.
  float scrollX = 0, scrollY = 0;
  // Goal x (content px) kept across repeated Up / Down.
  float goalX = -1;
  bool hasGoalX = false;

  // Caret as last reported to the incremental painter (border-box coords).
  bool recordedVisible = false;
  SkRect recordedCaret = SkRect::MakeEmpty();

  std::vector<UndoEntry> undo, redo;
  bool coalesceTyping = false;  // the last undo entry may absorb more typed text
};

// UTF conversion (invalid UTF-8 decodes as U+FFFD).
std::u16string toUtf16(const std::string& s);
std::string toUtf8(const std::u16string& s);

// The node's edit state, created from its props on first use.
EditState* state(Node* n);
// Existing state or nullptr.
inline EditState* peek(Node* n) { return n ? static_cast<EditState*>(n->editState.get()) : nullptr; }
// Called by Node::setProps for TextInput nodes: adopts `value` (see above),
// clamps the selection. Marks the node dirty when the text changed.
void syncProps(Node* n);

// Replaces the native text (keeps the selection clamped) and marks the
// node's text dirty.
void setValue(Node* n, EditState* s, std::u16string value);
// Re-dirties text layout after the value changed.
void contentChanged(Node* n);

// Cluster boundaries (code points, plus combining marks / ZWJ sequences /
// variation selectors / emoji modifiers kept with their base).
int nextBoundary(const std::u16string& s, int i);
int prevBoundary(const std::u16string& s, int i);
// Word boundaries (letters, digits, '_' and non-ASCII letters form words).
int nextWordEnd(const std::u16string& s, int i);
int prevWordStart(const std::u16string& s, int i);
// The word (or run of spaces / punctuation) around index i (double click).
void wordAt(const std::u16string& s, int i, int* start, int* end);

}  // namespace jsui::edit
