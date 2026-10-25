// Delivering native -> JS messages (js/inbound.h) to the engine.
#include "js/inbound.h"

#include <limits>
#include <utility>

#include "js/engine.h"
#include "js/js_thread.h"
#include "runtime.h"

namespace jsui {

uint32_t Inbound::event(Value&& ev, bool reply) {
  Message m{Message::Kind::Event};
  m.value = std::move(ev);
  if (reply) {
    m.seq = nextSeq();
    replies_.push_back({m.seq, false, 0, 0});
  }
  uint32_t seq = m.seq;
  post(std::move(m));
  return seq;
}

void Inbound::hostMessage(std::string name, std::string json) {
  Message m{Message::Kind::HostMessage};
  m.name = std::move(name);
  m.text = std::move(json);
  post(std::move(m));
}

void Inbound::layout(uint32_t id, float x, float y, float w, float h) {
  Message m{Message::Kind::Layout};
  m.id = id;
  m.rect[0] = x;
  m.rect[1] = y;
  m.rect[2] = w;
  m.rect[3] = h;
  post(std::move(m));
}

void Inbound::call(const char* fn, Value::Array&& args, Missing missing, uint64_t id) {
  Message m{Message::Kind::Call};
  m.name = fn;
  m.value = Value(std::move(args));
  m.missing = missing;
  m.id = id;
  post(std::move(m));
}

void Inbound::keysChanged(Message::Kind kind, std::vector<std::string>&& keys) {
  Message m{kind};
  m.keys = std::move(keys);
  post(std::move(m));
}

void Inbound::settleCall(uint64_t id, bool ok, std::string payload) {
  Message m{Message::Kind::SettleCall};
  m.id = id;
  m.ok = ok;
  m.text = std::move(payload);
  post(std::move(m));
}

void Inbound::frame(double t) {
  Message m{Message::Kind::Frame};
  m.t = t;
  post(std::move(m));
}

uint32_t Inbound::eval(std::string source, std::string name) {
  Message m{Message::Kind::Eval};
  m.text = std::move(source);
  m.name = std::move(name);
  m.seq = nextSeq();
  replies_.push_back({m.seq, false, 0, 0});
  uint32_t seq = m.seq;
  post(std::move(m));
  return seq;
}

void Inbound::task(std::function<void()> fn) {
  Message m{Message::Kind::Task};
  m.task = std::move(fn);
  post(std::move(m));
}

void Inbound::beginBatch() { post(Message{Message::Kind::BeginBatch}); }

void Inbound::endBatch() { post(Message{Message::Kind::EndBatch}); }

// Single-threaded: delivered at once. JS thread: queued for it; in
// lockstep mode the owner waits for JS to handle it (and applies what JS
// did), as single-threaded, except inside a batch (the batch is waited for
// as a whole).
void Inbound::post(Message&& m) {
  JsEngine* js = rt_.js.get();
  if (!js) {
    if (m.seq) forget(m.seq);  // no answer will come
    return;
  }
  if (JsThread* t = rt_.jsThread()) {
    // A batch goes out with its first message: one with none (a layout
    // without onLayout events) is dropped, so JS stays idle (syncJs).
    if (m.kind == Message::Kind::BeginBatch) {
      batchDepth_++;
      heldBegins_++;
      return;
    }
    if (m.kind == Message::Kind::EndBatch) {
      if (batchDepth_ > 0) batchDepth_--;
      if (heldBegins_ > 0) {
        heldBegins_--;
        return;
      }
    }
    for (; heldBegins_ > 0; heldBegins_--) {
      Message begin{Message::Kind::BeginBatch};
      begin.now = rt_.now;
      t->post(std::move(begin));
    }
    m.now = rt_.now;
    t->post(std::move(m));
    if (t->lockstep() && batchDepth_ == 0) rt_.syncJs(-1, false);
    return;
  }
  deliver(m, *js);
}

// On a JS thread, an answer goes out with the batch of the task.
static void answer(Inbound& in, uint32_t seq, int flags, bool event) {
  if (JsThread* t = JsThread::current()) t->reply(seq, flags, event);
  else in.reply(seq, flags, event);
}

void Inbound::deliver(Message& m, JsEngine& engine) {
  JsEngine* js = &engine;
  using Kind = Message::Kind;
  switch (m.kind) {
    case Kind::Event: {
      int flags = js->dispatchEvent(m.value);
      if (m.seq) answer(*this, m.seq, flags, true);
      break;
    }
    case Kind::HostMessage: js->hostMessage(m.name, m.text); break;
    case Kind::Layout: js->layoutEvent(uint32_t(m.id), m.rect[0], m.rect[1], m.rect[2], m.rect[3]); break;
    case Kind::Call: {
      const Value::Array& args = m.value.asArray();
      if (!js->callGlobal(m.name.c_str(), args.data(), int(args.size()))) noHandler(m);
      break;
    }
    case Kind::StateChanged: js->stateChanged(m.keys); break;
    case Kind::BuffersChanged: js->buffersChanged(m.keys); break;
    case Kind::TexturesChanged: js->texturesChanged(m.keys); break;
    case Kind::SettleCall: js->settleCall(m.id, m.ok, m.text); break;
    case Kind::Frame:
      js->runTimers(m.t);
      js->runAnimationFrame(m.t);
      js->runJobs();
      break;
    case Kind::Eval: {
      bool ok = js->eval(m.text, m.name);
      answer(*this, m.seq, ok ? 1 : 0, false);
      break;
    }
    case Kind::Task:
      if (m.task) m.task();
      break;
    case Kind::BeginBatch: js->beginBatch(); break;
    case Kind::EndBatch: js->endBatch(); break;
  }
}

// What a "no handler" reply does (single-threaded: right after the call; JS
// thread: on the owner thread, with the task's batch).
void Inbound::noHandler(const Message& m) {
  if (JsThread::current() && m.missing != Missing::Ignore) {
    Runtime* rt = &rt_;
    Message copy{m.kind};
    copy.missing = m.missing;
    copy.id = m.id;
    copy.value = m.value;
    rt->toHost([rt, copy = std::move(copy)] { rt->inbound.noHandler(copy); });
    return;
  }
  switch (m.missing) {
    case Missing::Ignore: break;
    case Missing::WarnNoAppRegistry:
      rt_.log(JSUI_LOG_WARN, "surfaces: the bundle has no AppRegistry (import AppRegistry from 'react-native')");
      break;
    case Missing::FailUiCall: {
      const Value::Array& args = m.value.asArray();
      std::string name = args.size() > 1 ? args[1].asString() : std::string();
      rt_.uiCallSettled(m.id, false,
                        "no UI handler for \"" + name + "\" (the bundle has not registered Host.handle)");
      break;
    }
    case Missing::Reload: rt_.reload(); break;  // deferred while JS (or its batch) runs
  }
}

void Inbound::reply(uint32_t seq, int flags, bool event) {
  for (Reply& r : replies_) {
    if (r.seq != seq) continue;
    r.arrived = true;
    r.flags = flags;
    r.dueAt = event && replyDelayMs > 0 ? rt_.now + replyDelayMs : -std::numeric_limits<double>::infinity();
    return;
  }
}

bool Inbound::take(uint32_t seq, int* flags) {
  for (size_t i = 0; i < replies_.size(); i++) {
    Reply& r = replies_[i];
    if (r.seq != seq) continue;
    if (!r.arrived || r.dueAt > rt_.now) return false;
    *flags = r.flags;
    replies_.erase(replies_.begin() + std::ptrdiff_t(i));
    return true;
  }
  return false;
}

void Inbound::forget(uint32_t seq) {
  for (size_t i = 0; i < replies_.size(); i++) {
    if (replies_[i].seq != seq) continue;
    replies_.erase(replies_.begin() + std::ptrdiff_t(i));
    return;
  }
}

}  // namespace jsui
