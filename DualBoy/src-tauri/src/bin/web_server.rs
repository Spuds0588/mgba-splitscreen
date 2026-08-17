//! Standalone web server for the DualBoy browser demo.
//!
//! Run with `cargo run --bin dualboy-web -- --players 4`, then open
//! http://127.0.0.1:8080 in a browser.

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;

use axum::{
    body::Bytes,
    extract::ws::{Message, WebSocket, WebSocketUpgrade},
    extract::{DefaultBodyLimit, Path, State},
    http::{header, StatusCode, Uri},
    response::IntoResponse,
    routing::{get, post},
    Router,
};
use dualboy_lib::emulation::EmulationManager;
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;

#[derive(Clone)]
struct AppState {
    manager: Arc<EmulationManager>,
}

#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum ClientCommand {
    Keys { player: u8, keys: u32 },
}

fn parse_players() -> usize {
    let mut players = 2;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--players" {
            players = args.next().and_then(|v| v.parse().ok()).unwrap_or(2);
        }
    }
    players.clamp(2, 4)
}

fn parse_fps() -> u32 {
    let mut fps = 30;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--fps" {
            fps = args.next().and_then(|v| v.parse().ok()).unwrap_or(30);
        }
    }
    fps.clamp(1, 60)
}

#[tokio::main]
async fn main() {
    let players = parse_players();
    let fps = parse_fps();
    println!("Starting DualBoy web server with {players} players at {fps} video FPS...");
    let manager = Arc::new(EmulationManager::new(players));
    manager.start(fps);

    let state = AppState { manager };

    let app = Router::new()
        .route("/ws", get(ws_handler))
        .route("/load_rom", post(load_rom_handler))
        .route("/player_count", get(player_count_handler))
        .route("/save/:player", get(get_save_handler).post(post_save_handler))
        .route("/save_set", get(get_save_set_handler).post(post_save_set_handler))
        .layer(DefaultBodyLimit::max(64 * 1024 * 1024))
        .fallback(static_handler)
        .with_state(state);

    let addr = SocketAddr::from(([127, 0, 0, 1], 8080));
    println!("Serving on http://{addr}");
    let listener = tokio::net::TcpListener::bind(addr).await.expect("bind 8080");
    axum::serve(listener, app).await.expect("serve");
}

async fn ws_handler(ws: WebSocketUpgrade, State(state): State<AppState>) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, state))
}

async fn handle_socket(socket: WebSocket, state: AppState) {
    let (mut sender, mut receiver) = socket.split();
    let mut frames = state.manager.frame_sender.subscribe();
    let mut status_rx = state.manager.status_sender.subscribe();

    loop {
        tokio::select! {
            frame = frames.recv() => {
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
                        if let Ok(ClientCommand::Keys { player, keys }) = serde_json::from_str(&text) {
                            let _ = state.manager.set_keys(player, keys);
                        }
                    }
                    Some(Ok(Message::Close(_))) => break,
                    Some(Ok(_)) => {}
                    Some(Err(_)) | None => break,
                }
            }
        }
    }
}

async fn load_rom_handler(State(state): State<AppState>, body: Bytes) -> impl IntoResponse {
    let path = std::env::temp_dir().join("dualboy_upload.gba");
    if let Err(e) = std::fs::write(&path, &body) {
        return (StatusCode::INTERNAL_SERVER_ERROR, format!("write failed: {e}")).into_response();
    }
    match state.manager.load_rom(path.to_str().unwrap_or("")) {
        Ok(()) => "ok".into_response(),
        Err(e) => (StatusCode::BAD_REQUEST, e).into_response(),
    }
}

async fn player_count_handler(State(state): State<AppState>) -> String {
    state.manager.player_count().to_string()
}

async fn get_save_handler(
    State(state): State<AppState>,
    Path(player): Path<u8>,
) -> impl IntoResponse {
    match state.manager.export_save(player) {
        Ok(data) => (
            [(header::CONTENT_TYPE, "application/octet-stream")],
            data,
        )
            .into_response(),
        Err(e) => (StatusCode::NOT_FOUND, e).into_response(),
    }
}

async fn post_save_handler(
    State(state): State<AppState>,
    Path(player): Path<u8>,
    body: Bytes,
) -> impl IntoResponse {
    match state.manager.import_save(player, &body) {
        Ok(()) => "ok".into_response(),
        Err(e) => (StatusCode::BAD_REQUEST, e).into_response(),
    }
}

async fn get_save_set_handler(State(state): State<AppState>) -> impl IntoResponse {
    match state.manager.export_save_set() {
        Ok(data) => (
            [(header::CONTENT_TYPE, "application/octet-stream")],
            data,
        )
            .into_response(),
        Err(e) => (StatusCode::BAD_REQUEST, e).into_response(),
    }
}

async fn post_save_set_handler(State(state): State<AppState>, body: Bytes) -> impl IntoResponse {
    match state.manager.import_save_set(&body) {
        Ok(()) => "ok".into_response(),
        Err(e) => (StatusCode::BAD_REQUEST, e).into_response(),
    }
}

async fn static_handler(uri: Uri) -> impl IntoResponse {
    let mut path = uri.path().trim_start_matches('/').to_string();
    if path.is_empty() {
        path = "index.html".into();
    }
    if path.contains("..") {
        return (StatusCode::NOT_FOUND, "not found").into_response();
    }
    let full = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../src")
        .join(&path);
    match tokio::fs::read(&full).await {
        Ok(bytes) => {
            let ct = match full.extension().and_then(|e| e.to_str()) {
                Some("html") => "text/html",
                Some("js") => "text/javascript",
                Some("css") => "text/css",
                Some("png") => "image/png",
                _ => "application/octet-stream",
            };
            ([(header::CONTENT_TYPE, ct)], bytes).into_response()
        }
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}
