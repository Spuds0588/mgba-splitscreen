pub mod gba;
pub mod emulation;
mod bindings;

use std::sync::Arc;
use once_cell::sync::Lazy;
use tokio::net::TcpListener;
use tokio_tungstenite::tungstenite::Message;
use futures_util::{SinkExt, StreamExt};
use crate::emulation::EmulationManager;

/// Player count for the desktop app is configurable via `--players N` (2-4),
/// defaulting to 2, so 3- and 4-player link testing doesn't need a code change.
static EMULATOR: Lazy<Arc<EmulationManager>> = Lazy::new(|| {
    let mut players = 2usize;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--players" {
            players = args.next().and_then(|v| v.parse().ok()).unwrap_or(2);
        }
    }
    Arc::new(EmulationManager::new(players.clamp(2, 4)))
});

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

#[derive(serde::Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum ClientCommand {
    Keys { player: u8, keys: u32 },
    LoadRom { path: String },
}

async fn start_websocket_server() {
    let listener = TcpListener::bind("127.0.0.1:8088").await.expect("Failed to bind WS");
    println!("WebSocket server listening on ws://127.0.0.1:8088");

    while let Ok((stream, _)) = listener.accept().await {
        let emulator = EMULATOR.clone();
        tokio::spawn(async move {
            let mut rx = emulator.frame_sender.subscribe();
            let mut status_rx = emulator.status_sender.subscribe();
            let mut ws_stream = tokio_tungstenite::accept_async(stream)
                .await
                .expect("Error during WS handshake");
            let (mut sender, mut receiver) = ws_stream.split();

            loop {
                tokio::select! {
                    frame = rx.recv() => {
                        match frame {
                            Ok(data) => {
                                if sender.send(Message::Binary(data)).await.is_err() {
                                    break;
                                }
                            }
                            Err(_) => break,
                        }
                    }
                    status = status_rx.recv() => {
                        match status {
                            Ok(text) => {
                                // Overlay/stats line: text frame, distinct from binary pixels.
                                if sender.send(Message::Text(text.into())).await.is_err() {
                                    break;
                                }
                            }
                            Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {}
                            Err(_) => break,
                        }
                    }
                    incoming = receiver.next() => {
                        match incoming {
                            Some(Ok(Message::Text(text))) => {
                                if let Ok(cmd) = serde_json::from_str::<ClientCommand>(&text) {
                                    match cmd {
                                        ClientCommand::Keys { player, keys } => {
                                            let _ = emulator.set_keys(player, keys);
                                        }
                                        ClientCommand::LoadRom { path } => {
                                            let _ = emulator.load_rom(&path);
                                        }
                                    }
                                }
                            }
                            Some(Ok(Message::Close(_))) => break,
                            Some(Ok(_)) => {}
                            Some(Err(_)) | None => break,
                        }
                    }
                }
            }
        });
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    println!("Starting emulation manager...");
    // Emulation at 60 FPS with every emulated frame broadcast (video 60). The
    // frontend renders each one and drops only what its compositor can't keep up
    // with (see EmulationManager::start).
    EMULATOR.start(60);
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
