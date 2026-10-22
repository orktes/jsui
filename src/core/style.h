// Computed (parsed) style for a node. Layout properties are written straight
// into the node's Yoga node by applyStyle(); everything the painter, text
// layout and hit testing need is kept here.
//
// Style input follows React Native (camelCase keys, numbers are logical px)
// plus the web-style extensions RN >= 0.76 accepts: `boxShadow`,
// `experimental_backgroundImage` / `backgroundImage` gradients, `filter`,
// CSS transform strings, `cursor`.
#pragma once

#include <yoga/Yoga.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/value.h"

namespace jsui {

// Non-premultiplied RGBA, 0..1.
struct Color {
  float r = 0, g = 0, b = 0, a = 0;
  bool transparent() const { return a <= 0; }
  static Color rgba(int r, int g, int b, float a = 1) { return {r / 255.f, g / 255.f, b / 255.f, a}; }
};

// Parses CSS colors: named colors, #rgb, #rgba, #rrggbb, #rrggbbaa,
// rgb()/rgba(), hsl()/hsla(), "transparent". Returns false if unparseable.
bool parseColor(const std::string& s, Color* out);
bool parseColor(const Value& v, Color* out);

// A length that may be a percentage (of the reference box) or px.
struct Length {
  float value = 0;
  bool percent = false;
  float resolve(float ref) const { return percent ? value * ref / 100.f : value; }
};

struct Shadow {
  float offsetX = 0, offsetY = 0;
  float blur = 0;    // CSS blur radius (sigma = blur / 2)
  float spread = 0;
  Color color;
  bool inset = false;
};

struct GradientStop {
  Color color;
  std::optional<float> position;  // 0..1, resolved by the painter when absent
  bool px = false;                // position is in px along the gradient line
};

struct Gradient {
  enum class Kind { Linear, Radial } kind = Kind::Linear;
  // Linear: CSS angle in degrees (0 = to top, 90 = to right). Default 180.
  float angle = 180;
  // Radial: shape and size keyword, center in percent of the box.
  bool circle = false;
  enum class Extent { FarthestCorner, ClosestSide, ClosestCorner, FarthestSide } extent = Extent::FarthestCorner;
  Length centerX{50, true}, centerY{50, true};
  bool repeating = false;
  std::vector<GradientStop> stops;
  // Linear "to <corner>" keyword: the angle depends on the box aspect ratio.
  // 0 none, 1 top right, 2 bottom right, 3 bottom left, 4 top left.
  uint8_t corner = 0;
  // Radial: explicit size ("circle 40px", "ellipse 50% 30%") overrides extent.
  bool explicitSize = false;
  Length sizeX, sizeY;
};

// One parsed transform function, applied about transformOrigin.
struct TransformOp {
  enum class Kind { Translate, Scale, Rotate, SkewX, SkewY, Matrix } kind;
  float a = 0, b = 0;      // translate x/y (px), scale x/y, rotate/skew degrees in a
  bool percentA = false, percentB = false;  // translate percentages of own size
  float m[6] = {1, 0, 0, 1, 0, 0};  // Matrix: a b c d e f (CSS matrix())
  // rotateX/rotateY are flattened to a Scale (no perspective); this records
  // that the rotation turned the element's back face toward the viewer.
  bool flipsBackface = false;
};

enum class BorderStyle : uint8_t { Solid, Dashed, Dotted };
enum class Overflow : uint8_t { Visible, Hidden, Scroll };
enum class PointerEvents : uint8_t { Auto, None, BoxNone, BoxOnly };
enum class ResizeMode : uint8_t { Cover, Contain, Stretch, Center, Repeat };

enum class TextAlign : uint8_t { Auto, Left, Right, Center, Justify, End };
enum class TextTransform : uint8_t { None, Uppercase, Lowercase, Capitalize };
enum TextDecoration : uint8_t { DecorNone = 0, DecorUnderline = 1, DecorLineThrough = 2 };

// Inherited text properties. Unset optionals inherit from the parent Text.
struct TextStyle {
  std::optional<Color> color;
  std::optional<std::string> fontFamily;  // CSS-like list: "Inter, sans-serif"
  std::optional<float> fontSize;
  std::optional<int> fontWeight;          // 100..900 ("bold" = 700, "normal" = 400)
  std::optional<bool> italic;
  std::optional<float> lineHeight;        // px (RN semantics)
  std::optional<float> letterSpacing;
  std::optional<TextAlign> textAlign;
  std::optional<TextTransform> textTransform;
  std::optional<uint8_t> decoration;      // TextDecoration bits
  std::optional<Color> decorationColor;
  std::vector<Shadow> textShadows;
  bool hasTextShadow = false;
  // Added by the render area:
  std::optional<uint8_t> decorationStyle;  // 0 solid, 1 double, 2 dotted, 3 dashed, 4 wavy
  std::optional<std::vector<std::string>> fontVariant;  // RN fontVariant ("tabular-nums", ...)
  std::optional<int8_t> writingDirection;  // 0 auto, 1 ltr, 2 rtl
  bool lineHeightRelative = false;         // lineHeight is a multiple of fontSize ("1.5")
};

// One CSS filter function, applied in order.
struct FilterOp {
  enum class Kind : uint8_t { Blur, Brightness, Contrast, Saturate, Grayscale, Sepia, Invert, Opacity, HueRotate, DropShadow };
  Kind kind = Kind::Blur;
  float amount = 0;  // blur: std deviation px; hue-rotate: degrees; others: factor
  Shadow shadow;     // drop-shadow
};

struct ComputedStyle {
  // Paint
  Color backgroundColor;
  std::vector<Gradient> backgroundImages;  // painted on top of backgroundColor, first on top
  float opacity = 1;

  // Borders (top, right, bottom, left). Widths also go to Yoga.
  float borderWidth[4] = {0, 0, 0, 0};
  Color borderColor[4];
  BorderStyle borderStyle = BorderStyle::Solid;
  // Corner radii (topLeft, topRight, bottomRight, bottomLeft), px or %.
  Length borderRadius[4];

  std::vector<Shadow> boxShadows;  // outer and inset

  Overflow overflow = Overflow::Visible;
  std::vector<TransformOp> transform;
  Length transformOrigin[2] = {{50, true}, {50, true}};

  // Filters (CSS filter subset)
  float blur = 0;
  float brightness = 1, contrast = 1, saturate = 1, grayscale = 0;

  int zIndex = 0;
  bool hasZIndex = false;
  PointerEvents pointerEvents = PointerEvents::Auto;
  std::string cursor;
  bool display = true;  // display: 'none' -> false

  // Image
  ResizeMode resizeMode = ResizeMode::Cover;
  std::optional<Color> tintColor;

  TextStyle text;

  // Added by the render area:
  std::vector<FilterOp> filters;  // full ordered filter list (blur/brightness/... above mirror it)
  bool hasResizeMode = false;     // resizeMode/objectFit given in style (overrides the prop)
  bool backfaceHidden = false;    // backfaceVisibility: 'hidden'
  uint8_t blendMode = 0;          // mixBlendMode, index into kBlendModeNames (0 normal)
  // outline (RN 0.77): drawn outside the border box, does not affect layout.
  float outlineWidth = 0, outlineOffset = 0;
  Color outlineColor = {0, 0, 0, 1};
  BorderStyle outlineStyle = BorderStyle::Solid;
};

// CSS mix-blend-mode names in the order used by ComputedStyle::blendMode.
extern const char* const kBlendModeNames[];
extern const int kBlendModeCount;

// Parses a flattened style object into `out` and writes layout properties to
// `yoga`. Unknown keys are ignored. Resets everything not present first.
void applyStyle(const Value& style, ComputedStyle* out, YGNodeRef yoga);

// Parses a CSS box-shadow list ("0 4px 12px rgba(0,0,0,.3), inset 0 1px #fff")
// or RN array-of-objects form.
bool parseBoxShadow(const Value& v, std::vector<Shadow>* out);
// Parses "linear-gradient(...)", "radial-gradient(...)" lists (and RN object form).
bool parseBackgroundImage(const Value& v, std::vector<Gradient>* out);
// Parses RN transform arrays ([{rotate: '45deg'}, ...]) and CSS transform strings.
bool parseTransform(const Value& v, std::vector<TransformOp>* out);
// Parses CSS filter strings ("blur(4px) brightness(1.2)") and RN arrays.
bool parseFilter(const Value& v, std::vector<FilterOp>* out);
// Parses a CSS / RN length: number (px), "12px", "50%". "auto" is not a Length.
bool parseLength(const Value& v, Length* out);

// True when the two text styles produce the same text metrics (so a change
// between them only needs a repaint, not a re-measure).
bool textMetricsEqual(const TextStyle& a, const TextStyle& b);
bool operator==(const Color& a, const Color& b);
bool operator==(const Shadow& a, const Shadow& b);
bool operator==(const TextStyle& a, const TextStyle& b);

}  // namespace jsui
