mod gba;
mod emulation;
mod bindings;

use std::sync::Arc;
use once_cell::sync::Lazy;
use tokio::net::TcpListener;
use tokio_tungstenite::tungstenite::Message;
use futures_util::SinkExt;
use crate::emulation::EmulationManager;

static EMULATOR: Lazy<Arc<EmulationManager>> = Lazy::new(|| Arc::new(EmulationManager::new()));

#[tauri::command]
async fn load_rom(path: String) -> Result<(), String> {
    let mut gba1 = EMULATOR.instance1.lock().map_err(|e| e.to_string())?;
    let mut gba2 = EMULATOR.instance2.lock().map_err(|e| e.to_string())?;
    
    if gba1.load_rom(&path) && gba2.load_rom(&path) {
        // Drop locks before re-attaching drivers (which takes its own locks)
        drop(gba1);
        drop(gba2);
        EMULATOR.attach_drivers();
        Ok(())
    } else {
        Err("Failed to load ROM in one or both instances".into())
    }
}

#[tauri::command]
async fn set_keys(player: u8, keys: u32) -> Result<(), String> {
    if player == 1 {
        let mut gba1 = EMULATOR.instance1.lock().map_err(|e| e.to_string())?;
        gba1.set_keys(keys);
    } else {
        let mut gba2 = EMULATOR.instance2.lock().map_err(|e| e.to_string())?;
        gba2.set_keys(keys);
    }
    Ok(())
}

async fn start_websocket_server() {
    let listener = TcpListener::bind("127.0.0.1:8088").await.expect("Failed to bind WS");
    println!("WebSocket server listening on ws://127.0.0.1:8088");

    while let Ok((stream, _)) = listener.accept().await {
        let mut rx = EMULATOR.frame_sender.subscribe();
        tokio::spawn(async move {
            let mut ws_stream = tokio_tungstenite::accept_async(stream).await.expect("Error during WS handshake");
            
            while let Ok(frame) = rx.recv().await {
                if ws_stream.send(Message::Binary(frame)).await.is_err() {
                    break;
                }
            }
        });
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    println!("Starting emulation manager...");
    // Start emulation thread
    EMULATOR.start();
    println!("Emulation manager started.");

    println!("Starting WebSocket server...");
    // Start WebSocket server in background
    tauri::async_runtime::spawn(start_websocket_server());

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![load_rom, set_keys])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
