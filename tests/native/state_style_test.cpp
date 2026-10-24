// core/state_style.h: style value interpolation, flattening, weights.
#include <cmath>

#include "core/state_style.h"
#include "test.h"

using jsui::Value;

TEST(state_style_mix_values) {
  using jsui::mixStyleValue;
  CHECK_NEAR(mixStyleValue("opacity", Value(0.2), Value(1), 0.5f).asNumber(), 0.6, 1e-6);
  // Missing base: the key's resting value (opacity 1, transform identity).
  CHECK_NEAR(mixStyleValue("opacity", Value(), Value(0), 0.25f).asNumber(), 0.75, 1e-6);
  Value tf = Value::parseJson(R"([{"scale":0.5},{"rotate":"90deg"},{"translateX":10}])");
  Value m = mixStyleValue("transform", Value(), tf, 0.5f);
  CHECK_NEAR(m.asArray()[0]["scale"].asNumber(), 0.75, 1e-6);
  CHECK_EQ(m.asArray()[1]["rotate"].asString(), std::string("45deg"));
  CHECK_NEAR(m.asArray()[2]["translateX"].asNumber(), 5, 1e-6);
  // Angles in other units mix in degrees.
  Value r = mixStyleValue("transform", Value::parseJson(R"([{"rotate":"0.5turn"}])"),
                          Value::parseJson(R"([{"rotate":"0deg"}])"), 0.5f);
  CHECK_EQ(r.asArray()[0]["rotate"].asString(), std::string("90deg"));
  // Different transform shapes switch halfway.
  Value a = Value::parseJson(R"([{"scale":2}])");
  CHECK(mixStyleValue("transform", a, tf, 0.4f).equals(a));
  CHECK(mixStyleValue("transform", a, tf, 0.6f).equals(tf));
  // Colors: a missing base fades in the target's hue (not through black).
  CHECK_EQ(mixStyleValue("backgroundColor", Value(), Value("#ff0000"), 0.5f).asString(),
           std::string("rgba(255, 0, 0, 0.5)"));
  CHECK_EQ(mixStyleValue("color", Value("#000000"), Value("#ffffff"), 0.5f).asString(),
           std::string("rgba(127.5, 127.5, 127.5, 1)"));
  // Unknown resting values switch halfway; null base stays null below it.
  CHECK(mixStyleValue("width", Value(), Value(100), 0.4f).isNull());
  CHECK_NEAR(mixStyleValue("width", Value(), Value(100), 0.6f).asNumber(), 100, 1e-9);
  CHECK_NEAR(mixStyleValue("width", Value(50), Value(100), 0.5f).asNumber(), 75, 1e-9);
  CHECK_NEAR(mixStyleValue("borderWidth", Value(), Value(4), 0.5f).asNumber(), 2, 1e-9);
  Value so = mixStyleValue("shadowOffset", Value(), Value::parseJson(R"({"width":4,"height":8})"), 0.5f);
  CHECK_NEAR(so["height"].asNumber(), 4, 1e-9);
}

TEST(state_style_blend_and_weights) {
  jsui::StateStyles s;
  jsui::parseStateStyles(Value::parseJson(R"({
    "style": [{"opacity": 1, "backgroundColor": "#000000"}, null, false, [{"width": 10}]],
    "hoveredStyle": {"backgroundColor": "#ffffff"},
    "pressedStyle": [{"opacity": 0.5}],
    "disabledStyle": {"opacity": 0.2},
    "transition": {"duration": 100, "easing": "linear", "pressed": 0}
  })"), &s);
  CHECK_NEAR(s.base["width"].asNumber(), 10, 1e-9);
  CHECK_EQ(s.keys.size(), size_t(2));
  CHECK_NEAR(s.duration[jsui::kHovered], 100, 1e-9);
  CHECK_NEAR(s.duration[jsui::kPressed], 0, 1e-9);
  bool t[jsui::kStateSlots] = {};
  bool moving = false;
  jsui::stepStateWeights(s, t, 0, &moving);  // mount: no states
  t[jsui::kHovered] = t[jsui::kPressed] = true;
  moving = false;
  CHECK(jsui::stepStateWeights(s, t, 16, &moving));
  CHECK(moving);
  CHECK_NEAR(s.weight[jsui::kHovered], 0.16, 1e-6);  // started a frame back
  CHECK_NEAR(s.weight[jsui::kPressed], 1, 1e-9);     // instant
  Value st = jsui::blendStateStyle(s);
  CHECK_NEAR(st["opacity"].asNumber(), 0.5, 1e-9);
  CHECK_NEAR(st["width"].asNumber(), 10, 1e-9);
  for (double ms = 32; ms <= 160; ms += 16) jsui::stepStateWeights(s, t, ms, &moving);
  CHECK_NEAR(s.weight[jsui::kHovered], 1, 1e-9);
  // Disabled wins (last in blend order).
  t[jsui::kDisabled] = true;
  jsui::stepStateWeights(s, t, 176, &moving);
  CHECK_NEAR(jsui::blendStateStyle(s)["opacity"].asNumber(), 0.2 + (0.5 - 0.2) * (1 - 0.16), 1e-6);
  CHECK(!jsui::wantsStateStyles(Value::parseJson(R"({"style": {}, "disabled": true})")));
  CHECK(jsui::wantsStateStyles(Value::parseJson(R"({"group": "card"})")));
}
