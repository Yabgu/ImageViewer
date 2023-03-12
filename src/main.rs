//#![windows_subsystem = "windows"]

use futures::join;
use std::env;
use std::ffi::OsStr;
use std::path::Path;

use glfw::{Action, Context, Key};

async fn process_input() {
    if let Some(arg) = env::args().last() {
        let path = Path::new(&arg);
        let extension = path
            .extension()
            .and_then(OsStr::to_str)
            .unwrap_or_default()
            .to_ascii_uppercase();

        if matches!(&*extension, "JPEG" | "JPG" | "JFIF") {
            println!("{}", path.display());
        }
    }
}

async fn initialize_window() {
    let mut glfw = glfw::init(glfw::FAIL_ON_ERRORS).unwrap();

    let (mut window, events) = glfw.create_window(300, 300, "image_viewer", glfw::WindowMode::Windowed)
        .expect("Failed to create GLFW window.");

    window.make_current();
    window.set_key_polling(true);

    while !window.should_close() {
        window.swap_buffers();

        glfw.poll_events();
        for (_, event) in glfw::flush_messages(&events) {
            println!("{:?}", event);
            match event {
                glfw::WindowEvent::Key(Key::Escape, _, Action::Press, _) => {
                    window.set_should_close(true)
                },
                _ => {},
            }
        }
    }
}


#[async_std::main]
async fn main() {
    let process_input_future = process_input();
    let initialize_window_future = initialize_window();
    join!(process_input_future, initialize_window_future);
}
