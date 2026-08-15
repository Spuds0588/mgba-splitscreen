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

/// File format for a "save set": all instances' battery saves in one file.
/// Layout: b"DUALSAVE" | version:u32(1) | count:u32 | (size:u32, bytes)*
const SAVE_SET_MAGIC: &[u8; 8] = b"DUALSAVE";

fn serialize_save_set(saves: &[Vec<u8>]) -> Vec<u8> {
    let mut out = Vec::with_capacity(16 + saves.iter().map(|s| s.len() + 4).sum::<usize>());
    out.extend_from_slice(SAVE_SET_MAGIC);
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&(saves.len() as u32).to_le_bytes());
    for s in saves {
        out.extend_from_slice(&(s.len() as u32).to_le_bytes());
        out.extend_from_slice(s);
    }
    out
}

fn deserialize_save_set(data: &[u8]) -> Result<Vec<Vec<u8>>, String> {
    if data.len() < 16 || &data[0..8] != SAVE_SET_MAGIC {
        return Err("Not a DualBoy save set".into());
    }
    let version = u32::from_le_bytes(data[8..12].try_into().unwrap());
    if version != 1 {
        return Err(format!("Unsupported save set version {version}"));
    }
    let count = u32::from_le_bytes(data[12..16].try_into().unwrap()) as usize;
    let mut saves = Vec::with_capacity(count);
    let mut off = 16;
    for _ in 0..count {
        if off + 4 > data.len() {
            return Err("Truncated save set".into());
        }
        let size = u32::from_le_bytes(data[off..off + 4].try_into().unwrap()) as usize;
        off += 4;
        if off + size > data.len() {
            return Err("Truncated save set".into());
        }
        saves.push(data[off..off + size].to_vec());
        off += size;
    }
    Ok(saves)
}

#[tauri::command]
async fn export_save_set(path: String) -> Result<(), String> {
    let count = EMULATOR.player_count();
    let mut saves = Vec::with_capacity(count);
    for p in 1..=count {
        saves.push(EMULATOR.export_save(p as u8)?);
    }
    std::fs::write(&path, serialize_save_set(&saves)).map_err(|e| e.to_string())
}

#[tauri::command]
async fn import_save_set(path: String) -> Result<(), String> {
    let data = std::fs::read(&path).map_err(|e| e.to_string())?;
    let saves = deserialize_save_set(&data)?;
    for (p, save) in saves.iter().enumerate() {
        EMULATOR.import_save((p + 1) as u8, save)?;
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
