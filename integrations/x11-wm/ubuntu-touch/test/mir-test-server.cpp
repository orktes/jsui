// Test-only Mir 1.8 server for jsui-wl --mir (see test-mir.sh): runs on Mir's
// stub graphics and input platforms, so it needs no GPU or screen. It stands
// in for Ubuntu Touch's lomiri-system-compositor: jsui-wl connects as a
// mirclient session and puts its fullscreen window up, and touches written
// to /tmp/touch.cmd ("down X Y" / "move X Y" / "up X Y") reach it as Mir
// touch events from a fake touchscreen. The stub platform has no GL, so the
// compositor only marks frames rendered (SIGUSR1 logs the biggest surface).
#include <miral/runner.h>
#include <miral/minimal_window_manager.h>
#include <miral/set_window_management_policy.h>
#include <mir/server.h>
#include <mir/compositor/display_buffer_compositor.h>
#include <mir/compositor/display_buffer_compositor_factory.h>
#include <mir/compositor/scene_element.h>
#include <mir/graphics/renderable.h>
#include <mir/graphics/buffer.h>
#include <miral/add_init_callback.h>
#include <mir_test_framework/stub_server_platform_factory.h>
#include <mir_test_framework/fake_input_device.h>
#include <mir/test/event_factory.h>
#include <mir/input/input_device_info.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <vector>
namespace mc = mir::compositor;
namespace mg = mir::graphics;
static std::atomic<bool> g_dump{false};
static std::atomic<int> g_frames{0};
struct DumpCompositor : mc::DisplayBufferCompositor {
  void composite(mc::SceneElementSequence&& seq) override {
    for (auto& e : seq) e->rendered();
    if (!seq.empty()) g_frames++;
    if (!g_dump.exchange(false) || seq.empty()) return;
    auto r = seq.front()->renderable();
    for (auto& e : seq) {
      auto s = e->renderable()->buffer()->size();
      auto bs = r->buffer()->size();
      if (s.width.as_int() * s.height.as_int() > bs.width.as_int() * bs.height.as_int()) r = e->renderable();
    }
    auto b = r->buffer();
    std::fprintf(stderr, "surface at %d,%d %dx%d (%zu elements)\n", r->screen_position().top_left.x.as_int(),
                 r->screen_position().top_left.y.as_int(), b->size().width.as_int(), b->size().height.as_int(),
                 seq.size());
  }
};
struct DumpFactory : mc::DisplayBufferCompositorFactory {
  std::unique_ptr<mc::DisplayBufferCompositor> create_compositor_for(mg::DisplayBuffer&) override {
    return std::make_unique<DumpCompositor>();
  }
};
namespace mis = mir::input::synthesis;
static mir::UniqueModulePtr<mir_test_framework::FakeInputDevice> g_touch;
// Touch input from a file: lines "down X Y" / "move X Y" / "up X Y" in /tmp/touch.cmd.
static void touchThread() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    FILE* f = std::fopen("/tmp/touch.cmd", "r");
    if (!f) continue;
    char cmd[16];
    int x, y;
    while (std::fscanf(f, "%15s %d %d", cmd, &x, &y) == 3) {
      auto a = std::string(cmd) == "down" ? mis::TouchParameters::Action::Tap
             : std::string(cmd) == "move" ? mis::TouchParameters::Action::Move : mis::TouchParameters::Action::Release;
      g_touch->emit_event(mis::a_touch_event().with_action(a).at_position({x, y}));
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::fclose(f);
    unlink("/tmp/touch.cmd");
  }
}
int main(int argc, char const* argv[]) {
  std::signal(SIGUSR1, [](int) { g_dump = true; });
  std::signal(SIGUSR2, [](int) { std::fprintf(stderr, "frames: %d\n", g_frames.load()); });
  miral::MirRunner runner{argc, argv};
  return runner.run_with({
      [](mir::Server& s) { s.override_the_display_buffer_compositor_factory([] { return std::make_shared<DumpFactory>(); }); },
      miral::set_window_management_policy<miral::MinimalWindowManager>(),
      miral::AddInitCallback{[] {
        g_touch = mir_test_framework::add_fake_input_device(
            mir::input::InputDeviceInfo{"touchscreen", "touch-1", mir::input::DeviceCapability::touchscreen | mir::input::DeviceCapability::multitouch});
        std::thread(touchThread).detach();
      }}});
}
