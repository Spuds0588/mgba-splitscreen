pub mod gba;
pub mod emulation;
mod bindings;

use std::sync::Arc;
use once_cell::sync::Lazy;
use tokio::net::TcpListener;
use tokio_tungstenite::tungstenite::Message;
use futures_util::SinkExt;
use crate::emulation::EmulationManager;

static EMULATOR: Lazy<Arc<EmulationManager>> = Lazy::new(|| Arc::new(EmulationManager::new(2)));

#[tauri::command]
async fn load_rom(path: String) -> Result<(), String> {
    EMULATOR.load_rom(&path)
}

#[tauri::command]
async fn set_keys(player: u8, keys: u32) -> Result<(), String> {
    EMULATOR.set_keys(player, keys)
}

#[tauri::command]
async fn player_count() -> Result<usize, String> {
    Ok(EMULATOR.player_count())
}

#[tauri::command]
async fn export_save(player: u8, path: String) -> Result<(), String> {
    let data = EMULATOR.export_save(player)?;
    std::fs::write(&path, data).map_err(|e| e.to_string())
}

#[tauri::command]
async fn import_save(player: u8, path: String) -> Result<(), String> {
    let data = std::fs::read(&path).map_err(|e| e.to_string())?;
    EMULATOR.import_save(player, &data)
}

#[tauri::command]
async fn export_save_set(path: String) -> Result<(), String> {
    let data = EMULATOR.export_save_set()?;
    std::fs::write(&path, data).map_err(|e| e.to_string())
}

#[tauri::command]
async fn import_save_set(path: String) -> Result<(), String> {
    let data = std::fs::read(&path).map_err(|e| e.to_string())?;
    EMULATOR.import_save_set(&data)
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
        .invoke_handler(tauri::generate_handler![
            load_rom,
            set_keys,
            player_count,
            export_save,
            import_save,
            export_save_set,
            import_save_set
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
