// Prevent an extra console window on Windows in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use tauri::{
    menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    Manager,
};

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            // Tray menu: checkable "always on top" (default on), separator, quit.
            let always_on_top_i = CheckMenuItem::with_id(
                app,
                "always_on_top",
                "항상 위",
                true,
                true,
                None::<&str>,
            )?;
            let separator = PredefinedMenuItem::separator(app)?;
            let quit_i = MenuItem::with_id(app, "quit", "종료", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&always_on_top_i, &separator, &quit_i])?;

            // Clone the check item so the menu-event closure can read its state.
            let toggle_item = always_on_top_i.clone();

            TrayIconBuilder::with_id("main-tray")
                .tooltip("작가시계")
                .icon(app.default_window_icon().unwrap().clone())
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(move |app, event| match event.id.as_ref() {
                    "always_on_top" => {
                        // The native check item toggles itself on click; read the new state
                        // and sync the main window accordingly.
                        let checked = toggle_item.is_checked().unwrap_or(false);
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.set_always_on_top(checked);
                        }
                    }
                    "quit" => {
                        app.exit(0);
                    }
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        let app = tray.app_handle();
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.unminimize();
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                })
                .build(app)?;

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running AuthorClock");
}
