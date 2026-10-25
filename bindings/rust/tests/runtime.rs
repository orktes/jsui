// Round trips through libjsui without a window: shared state, the example
// UI's messages, a call into the UI and a CPU frame. Needs libjsui (see
// build.rs) and the example bundle:
//   npx jsui build bindings/ui/app.jsx -o bindings/ui/dist/bundle.js
use std::cell::RefCell;
use std::rc::Rc;

use jsui::{Config, JsThread, Runtime};

fn runtime() -> Runtime {
    Runtime::new(Config { width: 640, height: 480, audio_disabled: true, ..Default::default() }).unwrap()
}

// One test: the runtimes of a process are meant to live on one thread, and
// the test harness would run separate tests on separate threads.
#[test]
fn runtime_round_trips() {
    state_round_trip();
    example_ui_runs();
    js_thread_runs();
}

fn state_round_trip() {
    let rt = runtime();
    assert!(rt.set_state("player", r#"{"gold":10}"#));
    assert!(!rt.set_state("player", r#"{ "gold": 10 }"#), "an equal value is not a change");
    assert!(rt.patch_state("player", r#"{"level":2}"#));
    assert_eq!(rt.get_state("player").as_deref(), Some(r#"{"gold":10,"level":2}"#));
    assert!(rt.remove_state("player"));
    assert_eq!(rt.get_state("player"), None);
}

fn example_ui_runs() {
    let rt = runtime();
    let messages = Rc::new(RefCell::new(Vec::<String>::new()));
    {
        let messages = messages.clone();
        rt.on_message(move |_, name, _| messages.borrow_mut().push(name.to_owned()));
    }
    rt.set_state("quests", r#"{"items":[{"id":1,"title":"Test quest","done":false}]}"#);
    rt.load_bundle_file(concat!(env!("CARGO_MANIFEST_DIR"), "/../ui/dist/bundle.js")).unwrap();

    // No Host.handle('nope') in the UI: the call fails, inside a later tick.
    let answer = Rc::new(RefCell::new(None::<Result<String, String>>));
    {
        let answer = answer.clone();
        rt.call_ui("nope", "{}", move |r| *answer.borrow_mut() = Some(r.map(str::to_owned).map_err(str::to_owned)));
    }
    for i in 0..20 {
        rt.tick(i as f64 * 16.0);
    }
    assert!(matches!(*answer.borrow(), Some(Err(_))), "{:?}", answer.borrow());
    assert!(messages.borrow().iter().any(|m| m == "ui:anchors"), "{:?}", messages.borrow());

    let mut px = vec![0u8; 640 * 480 * 4];
    rt.render_rgba(&mut px, 640, 480);
    assert!(px.chunks(4).any(|p| p[3] == 255), "the frame is painted");
    let png = rt.render_png().unwrap();
    assert_eq!(&png[1..4], b"PNG");
}

// The bundle's JS on a thread of its own: a JS stall holds no tick, and a
// sync brings in what JS did.
fn js_thread_runs() {
    let config =
        Config { width: 640, height: 480, audio_disabled: true, js_thread: JsThread::On, ..Default::default() };
    let rt = Runtime::new(config).unwrap();
    assert_eq!(rt.js_thread(), JsThread::On);
    let messages = Rc::new(RefCell::new(Vec::<String>::new()));
    {
        let messages = messages.clone();
        rt.on_message(move |_, name, _| messages.borrow_mut().push(name.to_owned()));
    }
    rt.set_state("quests", r#"{"items":[{"id":1,"title":"Test quest","done":false}]}"#);
    rt.load_bundle_file(concat!(env!("CARGO_MANIFEST_DIR"), "/../ui/dist/bundle.js")).unwrap();
    rt.tick_sync(0.0);
    assert!(rt.sync(2000.0));

    rt.debug_stall_js(300.0);
    let start = std::time::Instant::now();
    for i in 1..=5 {
        rt.tick(i as f64 * 16.0);
    }
    assert!(start.elapsed().as_millis() < 150, "the ticks waited for JS: {:?}", start.elapsed());
    assert!(rt.sync(2000.0));
    assert!(messages.borrow().iter().any(|m| m == "ui:anchors"), "{:?}", messages.borrow());
}
