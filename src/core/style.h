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
};

// One parsed transform function, applied about transformOrigin.
struct TransformOp {
  enum class Kind { Translate, Scale, Rotate, SkewX, SkewY, Matrix } kind;
  float a = 0, b = 0;      // translate x/y (px), scale x/y, rotate/skew degrees in a
  bool percentA = false, percentB = false;  // translate percentages of own size
  float m[6] = {1, 0, 0, 1, 0, 0};  // Matrix: a b c d e f (CSS matrix())
};

enum class BorderStyle : uint8_t { Solid, Dashed, Dotted };
enum class Overflow : uint8_t { Visible, Hidden, Scroll };
enum class PointerEvents : uint8_t { Auto, None, BoxNone, BoxOnly };
enum class ResizeMode : uint8_t { Cover, Contain, Stretch, Center, Repeat };

enum class TextAlign : uint8_t { Auto, Left, Right, Center, Justify };
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
};

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

}  // namespace jsui
