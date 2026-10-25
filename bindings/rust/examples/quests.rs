//! Quest log: a desktop app whose UI is the React bundle in bindings/ui and
//! whose data and rules live here, in Rust. A GLFW window with an OpenGL
//! context; Skia draws the UI on the GPU straight into the window.
//!
//!   npx jsui build bindings/ui/app.jsx -o bindings/ui/dist/bundle.js
//!   cargo run --release --example quests            # [bundle.js | --dev URL]
//!   cargo run --example quests -- --selftest --screenshot quests.png
//!
//! What crosses the boundary (see bindings/ui/app.jsx):
//!   Rust -> UI  shared state `host`, `quests`, `player`, `clock`; message `toast`
//!   UI -> Rust  calls quests.add / quests.toggle / quests.remove; messages
//!               ui:anchors (for --selftest) and app:quit
use std::cell::RefCell;
use std::ffi::{c_void, CStr, CString};
use std::rc::Rc;

use glfw::{Action, Context as _, Key, StandardCursor, WindowEvent};
use jsui::{Call, Config, Context, GpuContext, JsThread, MouseButton, Runtime};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

#[derive(Serialize, Clone)]
struct Quest {
    id: u64,
    title: String,
    done: bool,
}

/// Everything the app knows. The UI only ever sees it through shared state.
struct App {
    quests: Vec<Quest>,
    next_id: u64,
    xp: u32,
    level: u32,
    anchors: Value,
    quit: bool,
}

impl App {
    fn new() -> App {
        let mut app = App { quests: vec![], next_id: 1, xp: 0, level: 1, anchors: Value::Null, quit: false };
        for t in ["Slay the ember drake", "Map the sunken library", "Forge a starsteel blade"] {
            app.add(t);
        }
        app
    }
    fn add(&mut self, title: &str) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        self.quests.push(Quest { id, title: title.to_owned(), done: false });
        id
    }
    fn next_level_xp(&self) -> u32 {
        100 * self.level
    }
    /// Pushes the app's data to the UI. Unchanged values cost nothing.
    fn publish(&self, cx: &Context) {
        cx.set_state("quests", &json!({ "items": self.quests }).to_string());
        cx.set_state(
            "player",
            &json!({ "level": self.level, "xp": self.xp, "next": self.next_level_xp() }).to_string(),
        );
    }
    /// `Host.call(name, args)` from the UI: the app's rules.
    fn handle(&mut self, cx: &Context, call: Call) -> Result<Value, String> {
        #[derive(Deserialize)]
        struct Args {
            id: Option<u64>,
            title: Option<String>,
        }
        let args: Args = serde_json::from_str(call.args).map_err(|e| e.to_string())?;
        let result = match call.name {
            "quests.add" => {
                let title = args.title.as_deref().map(str::trim).filter(|t| !t.is_empty()).ok_or("empty title")?;
                json!({ "id": self.add(title) })
            }
            "quests.toggle" => {
                let q = self.quests.iter_mut().find(|q| Some(q.id) == args.id).ok_or("no such quest")?;
                q.done = !q.done;
                let (done, title) = (q.done, q.title.clone());
                if done {
                    self.xp += 50;
                    let mut text = format!("Quest complete: {title} (+50 XP)");
                    while self.xp >= self.next_level_xp() {
                        self.xp -= self.next_level_xp();
                        self.level += 1;
                        text = format!("Level up! You are now level {}", self.level);
                    }
                    let tone = if text.starts_with("Level") { "level" } else { "quest" };
                    cx.post_message("toast", &json!({ "text": text, "tone": tone }).to_string());
                }
                json!({ "done": done })
            }
            "quests.remove" => {
                self.quests.retain(|q| Some(q.id) != args.id);
                json!({})
            }
            other => return Err(format!("unknown call {other}")),
        };
        self.publish(cx);
        Ok(result)
    }
}

struct Options {
    bundle: String,
    dev: bool,
    selftest: bool,
    screenshot: Option<String>,
    frames: u32,
    width: u32,
    height: u32,
    js_thread: JsThread,
}

fn parse_args() -> Options {
    let mut o = Options {
        bundle: concat!(env!("CARGO_MANIFEST_DIR"), "/../ui/dist/bundle.js").to_owned(),
        dev: false,
        selftest: false,
        screenshot: None,
        frames: 0,
        width: 900,
        height: 760,
        js_thread: JsThread::On,
    };
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut num =
            |a: &str| args.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| panic!("{a} needs a number"));
        match a.as_str() {
            "--dev" => o.dev = true,
            "--selftest" => o.selftest = true,
            "--screenshot" => o.screenshot = args.next(),
            "--frames" => o.frames = num(&a),
            "--width" => o.width = num(&a),
            "--height" => o.height = num(&a),
            "--js-thread" => {
                o.js_thread = match args.next().as_deref() {
                    Some("off") => JsThread::Off,
                    Some("lockstep") => JsThread::Lockstep,
                    _ => JsThread::On,
                }
            }
            _ if a.starts_with("--") => panic!("unknown option {a}"),
            _ => o.bundle = a,
        }
    }
    o
}

fn main() {
    let opt = parse_args();

    // --- window and GL context
    let mut glfw = glfw::init(glfw::fail_on_errors).expect("glfw init");
    glfw.window_hint(glfw::WindowHint::ContextVersion(3, 3));
    glfw.window_hint(glfw::WindowHint::OpenGlProfile(glfw::OpenGlProfileHint::Core));
    glfw.window_hint(glfw::WindowHint::OpenGlForwardCompat(true));
    glfw.window_hint(glfw::WindowHint::ScaleToMonitor(true));
    let (mut window, events) = glfw
        .create_window(opt.width, opt.height, "JSUI · Rust", glfw::WindowMode::Windowed)
        .expect("could not create a window with an OpenGL 3.3 context");
    window.make_current();
    window.set_all_polling(true);
    glfw.set_swap_interval(if opt.selftest { glfw::SwapInterval::None } else { glfw::SwapInterval::Sync(1) });

    // Skia on this GL context, with GLFW's loader.
    let gpu = GpuContext::gl_with_loader(|name| {
        let name = CString::new(name).unwrap();
        unsafe { glfw::ffi::glfwGetProcAddress(name.as_ptr()).map_or(std::ptr::null(), |f| f as *const c_void) }
    })
    .expect("Skia could not use this OpenGL context");
    let gl = Gl::load();
    let gpu_name = gl.renderer();
    println!("GPU: Skia Ganesh GL on {gpu_name}");

    // --- runtime
    let (w, h) = window.get_size();
    let (fw, _) = window.get_framebuffer_size();
    let rt = Runtime::new(Config {
        width: w,
        height: h,
        pixel_ratio: fw as f32 / w.max(1) as f32,
        dev_mode: opt.dev,
        // JS on its own thread (React Native's model); it wakes the idle
        // wait below when it has something for the next tick.
        js_thread: opt.js_thread,
        on_js_ready: Some(|| unsafe { glfw::ffi::glfwPostEmptyEvent() }),
        ..Default::default()
    })
    .expect("jsui runtime");
    println!("JS thread: {}", rt.js_thread());

    let app = Rc::new(RefCell::new(App::new()));
    {
        let app = app.clone();
        rt.on_call(move |cx, call| match app.borrow_mut().handle(cx, call) {
            Ok(v) => {
                cx.resolve_call(call.id, &v.to_string());
            }
            Err(e) => {
                cx.reject_call(call.id, &e);
            }
        });
    }
    {
        let app = app.clone();
        rt.on_message(move |_, name, json| {
            let mut app = app.borrow_mut();
            match name {
                "ui:anchors" => app.anchors = serde_json::from_str(json).unwrap_or(Value::Null),
                "app:quit" => app.quit = true,
                _ => {}
            }
        });
    }
    rt.set_state(
        "host",
        &json!({
            "language": format!("Rust {}", env!("JSUI_RUSTC_VERSION")),
            "renderer": "Skia Ganesh · OpenGL",
            "gpu": gpu_name,
            "os": format!("{} {}", std::env::consts::OS, std::env::consts::ARCH),
        })
        .to_string(),
    );
    app.borrow().publish(&rt);

    let loaded = if opt.dev || opt.bundle.starts_with("http") {
        rt.load_bundle_url(&opt.bundle)
    } else {
        rt.load_bundle_file(&opt.bundle)
    };
    if let Err(e) = loaded {
        eprintln!(
            "could not load {}: {e}\n(build it: npx jsui build bindings/ui/app.jsx -o bindings/ui/dist/bundle.js)",
            opt.bundle
        );
        std::process::exit(1);
    }

    // --- frame loop
    let mut mouse = (0.0f32, 0.0f32);
    let mut cursor = String::from("default");
    let mut clock = String::new();
    let mut frame = 0u32;
    let mut selftest = SelfTest::default();
    let mut dirty = true;
    loop {
        if window.should_close() || app.borrow().quit {
            break;
        }
        // Sleep while nothing moves; the UI's animations ask for frames.
        if opt.selftest || opt.frames > 0 || rt.wants_frame() || dirty {
            glfw.poll_events();
        } else {
            glfw.wait_events_timeout(0.25);
        }
        for (_, event) in glfw::flush_messages(&events) {
            match event {
                WindowEvent::Size(..) | WindowEvent::FramebufferSize(..) | WindowEvent::ContentScale(..) => {
                    let (w, h) = window.get_size();
                    let (fw, _) = window.get_framebuffer_size();
                    rt.set_viewport(w, h, fw as f32 / w.max(1) as f32);
                    dirty = true;
                }
                WindowEvent::Refresh => dirty = true,
                WindowEvent::CursorPos(x, y) => {
                    mouse = (x as f32, y as f32);
                    rt.send_mouse_move(mouse.0, mouse.1);
                }
                WindowEvent::CursorEnter(false) => {
                    rt.send_mouse_leave();
                }
                WindowEvent::MouseButton(b, action, m) => {
                    let button = match b {
                        glfw::MouseButtonRight => MouseButton::Right,
                        glfw::MouseButtonMiddle => MouseButton::Middle,
                        _ => MouseButton::Left,
                    };
                    let consumed = rt.send_mouse_button(mouse.0, mouse.1, button, action == Action::Press, mods(m));
                    // Input the UI did not consume is the app's (a game would orbit its camera here).
                    if !consumed && action == Action::Press {
                        println!("click at {:.0},{:.0} passed through to the app", mouse.0, mouse.1);
                    }
                }
                WindowEvent::Scroll(dx, dy) => {
                    rt.send_mouse_wheel(mouse.0, mouse.1, -dx as f32 * 40.0, -dy as f32 * 40.0);
                }
                WindowEvent::Key(key, _, action, m) => {
                    let (k, code) = dom_key(key);
                    rt.send_key(k, code, action != Action::Release, action == Action::Repeat, mods(m));
                }
                WindowEvent::Char(c) => {
                    rt.send_text(c.encode_utf8(&mut [0; 4]));
                }
                _ => {}
            }
        }

        let now = chrono::Local::now().format("%H:%M:%S").to_string();
        if now != clock {
            clock = now;
            rt.set_state("clock", &json!(clock).to_string());
        }

        rt.tick(jsui::now_ms());

        let want = rt.cursor();
        if want != cursor {
            let shape = match want.as_str() {
                "pointer" => StandardCursor::Hand,
                "text" => StandardCursor::IBeam,
                "crosshair" => StandardCursor::Crosshair,
                _ => StandardCursor::Arrow,
            };
            window.set_cursor(Some(glfw::Cursor::standard(shape)));
            cursor = want;
        }

        let (fw, fh) = window.get_framebuffer_size();
        if rt.needs_redraw() || dirty {
            rt.render_gl(&gpu, 0, fw, fh, 0);
            window.swap_buffers();
            dirty = false;
        }
        frame += 1;

        if opt.selftest {
            match selftest.step(&rt, &app, frame) {
                Some(Ok(())) => {
                    println!("selftest: ok");
                    break;
                }
                Some(Err(e)) => {
                    eprintln!("selftest: FAILED: {e}");
                    std::process::exit(1);
                }
                None => {}
            }
        } else if opt.frames > 0 && frame >= opt.frames {
            break;
        }
    }

    if let Some(path) = &opt.screenshot {
        // Settle animations, then read back what the GPU drew.
        for _ in 0..30 {
            rt.tick_sync(jsui::now_ms() + 1000.0);
        }
        let (fw, fh) = window.get_framebuffer_size();
        rt.render_gl(&gpu, 0, fw, fh, 0);
        let pixels = gl.read_pixels(fw, fh);
        write_png(path, &pixels, fw as u32, fh as u32);
        println!("wrote {path} ({fw}x{fh})");
    }
    drop(gpu); // GL context still current
}

fn mods(m: glfw::Modifiers) -> i32 {
    let mut out = 0;
    if m.contains(glfw::Modifiers::Shift) {
        out |= jsui::mods::SHIFT;
    }
    if m.contains(glfw::Modifiers::Control) {
        out |= jsui::mods::CTRL;
    }
    if m.contains(glfw::Modifiers::Alt) {
        out |= jsui::mods::ALT;
    }
    if m.contains(glfw::Modifiers::Super) {
        out |= jsui::mods::META;
    }
    out
}

const LETTERS: &str = "abcdefghijklmnopqrstuvwxyz";
const LETTER_CODES: [&str; 26] = [
    "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN",
    "KeyO", "KeyP", "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ",
];
const DIGITS: &str = "0123456789";
const DIGIT_CODES: [&str; 10] =
    ["Digit0", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9"];

/// GLFW key -> DOM `key` and `code` (the keys a UI needs; text comes from Char events).
fn dom_key(key: Key) -> (Option<&'static str>, Option<&'static str>) {
    let both = |s: &'static str| (Some(s), Some(s));
    match key {
        Key::Enter => both("Enter"),
        Key::KpEnter => (Some("Enter"), Some("NumpadEnter")),
        Key::Escape => both("Escape"),
        Key::Tab => both("Tab"),
        Key::Backspace => both("Backspace"),
        Key::Delete => both("Delete"),
        Key::Left => both("ArrowLeft"),
        Key::Right => both("ArrowRight"),
        Key::Up => both("ArrowUp"),
        Key::Down => both("ArrowDown"),
        Key::Home => both("Home"),
        Key::End => both("End"),
        Key::PageUp => both("PageUp"),
        Key::PageDown => both("PageDown"),
        Key::Space => (Some(" "), Some("Space")),
        Key::LeftShift => (Some("Shift"), Some("ShiftLeft")),
        Key::RightShift => (Some("Shift"), Some("ShiftRight")),
        Key::LeftControl => (Some("Control"), Some("ControlLeft")),
        Key::RightControl => (Some("Control"), Some("ControlRight")),
        Key::LeftAlt => (Some("Alt"), Some("AltLeft")),
        Key::RightAlt => (Some("Alt"), Some("AltRight")),
        Key::LeftSuper => (Some("Meta"), Some("MetaLeft")),
        Key::RightSuper => (Some("Meta"), Some("MetaRight")),
        // Letters and digits: shortcuts (Ctrl+A / C / V / X / Z) need the key; typing comes from Char.
        k if (Key::A as i32..=Key::Z as i32).contains(&(k as i32)) => {
            let i = (k as i32 - Key::A as i32) as usize;
            (Some(&LETTERS[i..i + 1]), Some(LETTER_CODES[i]))
        }
        k if (Key::Num0 as i32..=Key::Num9 as i32).contains(&(k as i32)) => {
            let i = (k as i32 - Key::Num0 as i32) as usize;
            (Some(&DIGITS[i..i + 1]), Some(DIGIT_CODES[i]))
        }
        _ => (None, None),
    }
}

/// Scripted check of the whole round trip, driven by where the UI says its
/// controls are (`ui:anchors`): a click on empty space passes through, Add
/// creates a quest in Rust, ticking the first quest gives 50 XP and the UI
/// shows both.
#[derive(Default)]
struct SelfTest {
    stage: u32,
    count: u64,
}

impl SelfTest {
    fn step(&mut self, rt: &Context, app: &RefCell<App>, frame: u32) -> Option<Result<(), String>> {
        // A click calls back into the app (on_call borrows it mutably), so no
        // borrow is held across one.
        let a = app.borrow().anchors.clone();
        let quests = app.borrow().quests.len() as u64;
        if frame > 900 {
            return Some(Err(format!("timed out in stage {} (anchors: {a})", self.stage)));
        }
        let center = |name: &str| -> Option<(f32, f32)> {
            let r = a.get(name)?;
            let f = |k: &str| r.get(k).and_then(Value::as_f64).unwrap_or(0.0) as f32;
            Some((f("x") + f("width") / 2.0, f("y") + f("height") / 2.0))
        };
        let click = |(x, y): (f32, f32)| {
            rt.send_mouse_move(x, y);
            let down = rt.send_mouse_button(x, y, MouseButton::Left, true, 0);
            let up = rt.send_mouse_button(x, y, MouseButton::Left, false, 0);
            down && up
        };
        let count = a.get("count").and_then(Value::as_u64).unwrap_or(0);
        let done = a.get("done").and_then(Value::as_u64).unwrap_or(0);
        match self.stage {
            0 if a.get("add").is_some() && count > 0 => {
                let empty = center("empty")?;
                if rt.hit_test(empty.0, empty.1) != 0 || click(empty) {
                    return Some(Err("a click on empty space was consumed".into()));
                }
                if !click(center("add")?) {
                    return Some(Err("the Add button did not consume its click".into()));
                }
                self.count = count;
                self.stage = 1;
            }
            1 if count == self.count + 1 && quests == count => {
                if done != 0 || !click(center("toggle")?) {
                    return Some(Err("could not tick the first quest".into()));
                }
                self.stage = 2;
            }
            2 if done == 1 => {
                let app = app.borrow();
                if app.xp != 50 || !app.quests[0].done {
                    return Some(Err(format!("expected 50 XP, have {}", app.xp)));
                }
                let player = rt.get_state("player").unwrap_or_default();
                if !player.contains("\"xp\":50") {
                    return Some(Err(format!("player state not published: {player}")));
                }
                return Some(Ok(()));
            }
            _ => {}
        }
        None
    }
}

/// The few GL entry points the example calls itself (Skia loads its own).
struct Gl {
    get_string: unsafe extern "C" fn(u32) -> *const u8,
    read_pixels: unsafe extern "C" fn(i32, i32, i32, i32, u32, u32, *mut c_void),
}

impl Gl {
    fn load() -> Gl {
        unsafe fn get(name: &str) -> unsafe extern "C" fn() {
            let c = CString::new(name).unwrap();
            glfw::ffi::glfwGetProcAddress(c.as_ptr()).unwrap_or_else(|| panic!("no {name}"))
        }
        type GetString = unsafe extern "C" fn(u32) -> *const u8;
        type ReadPixels = unsafe extern "C" fn(i32, i32, i32, i32, u32, u32, *mut c_void);
        unsafe {
            Gl {
                get_string: std::mem::transmute::<unsafe extern "C" fn(), GetString>(get("glGetString")),
                read_pixels: std::mem::transmute::<unsafe extern "C" fn(), ReadPixels>(get("glReadPixels")),
            }
        }
    }
    fn renderer(&self) -> String {
        const GL_RENDERER: u32 = 0x1F01;
        unsafe {
            let p = (self.get_string)(GL_RENDERER);
            if p.is_null() {
                "unknown GPU".into()
            } else {
                CStr::from_ptr(p.cast()).to_string_lossy().into_owned()
            }
        }
    }
    /// RGBA rows, top row first.
    fn read_pixels(&self, w: i32, h: i32) -> Vec<u8> {
        const GL_RGBA: u32 = 0x1908;
        const GL_UNSIGNED_BYTE: u32 = 0x1401;
        let mut px = vec![0u8; (w * h * 4) as usize];
        unsafe { (self.read_pixels)(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.as_mut_ptr().cast()) };
        let row = (w * 4) as usize;
        let mut out = Vec::with_capacity(px.len());
        for y in (0..h as usize).rev() {
            out.extend_from_slice(&px[y * row..(y + 1) * row]);
        }
        for a in out.iter_mut().skip(3).step_by(4) {
            *a = 255;
        }
        out
    }
}

fn write_png(path: &str, rgba: &[u8], w: u32, h: u32) {
    let file = std::io::BufWriter::new(std::fs::File::create(path).expect("screenshot file"));
    let mut enc = png::Encoder::new(file, w, h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().and_then(|mut wr| wr.write_image_data(rgba)).expect("png");
}
