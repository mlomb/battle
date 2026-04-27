use assert_cmd::cargo::CommandCargoExt;
use futures_util::StreamExt;
use serde_json::Value;
use std::path::Path;
use std::process::Child;
use std::process::Command;
use std::time::Duration;
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::MaybeTlsStream;
use tokio_tungstenite::WebSocketStream;

type WsStream = WebSocketStream<MaybeTlsStream<TcpStream>>;

/// A fake CG extension client that can be used for E2E tests
pub struct CGExtensionClient {
    child: Child,
    stream: WsStream,
}

impl Drop for CGExtensionClient {
    fn drop(&mut self) {
        self.child.kill().ok();
        self.child.wait().ok();
    }
}

impl CGExtensionClient {
    /// Spawns the cgsync binary pointing at `entrypoint`, waits for the WebSocket server
    /// to come up, then returns both the child handle and the connected stream.
    pub async fn start(entrypoint: &Path) -> Self {
        let child = Command::cargo_bin("cgsync")
            .expect("cargo_bin cgsync")
            .arg(entrypoint)
            .spawn()
            .expect("spawn cgsync");

        // wait a bit for the server to start
        tokio::time::sleep(Duration::from_millis(1000)).await;

        // connect to the server
        let (stream, _) = tokio_tungstenite::connect_async("ws://127.0.0.1:53135")
            .await
            .expect("connect to server");

        Self { child, stream }
    }

    /// Reads the next text frame from the stream and parses it as JSON.
    /// Binary/ping/pong frames are skipped.
    pub async fn next_json(&mut self) -> Value {
        loop {
            match self.stream.next().await {
                Some(Ok(Message::Text(text))) => {
                    return serde_json::from_str(&text)
                        .unwrap_or_else(|e| panic!("invalid JSON in WS frame: {e}\nraw: {text}"));
                }
                Some(Ok(_)) => continue, // skip non-text frames
                Some(Err(e)) => panic!("WebSocket error: {e}"),
                None => panic!("WebSocket stream closed unexpectedly"),
            }
        }
    }

    /// Waits up to `timeout` for an `update-code` frame, returning `None` if none arrives.
    /// Useful for asserting that a file change does NOT trigger an update.
    pub async fn try_next_update_code(&mut self, timeout: Duration) -> Option<String> {
        tokio::time::timeout(timeout, self.next_update_code())
            .await
            .ok()
    }

    /// Reads frames until an `update-code` action arrives (skips `ping` and other actions).
    pub async fn next_update_code(&mut self) -> String {
        loop {
            let msg = self.next_json().await;
            if msg["action"] == "update-code" {
                let code = msg["payload"]["code"]
                    .as_str()
                    .expect("payload.code should be a string");

                // we should always send play=false
                assert_eq!(msg["payload"]["play"], false, "play flag should be false");

                return code.to_string();
            }
        }
    }

    pub async fn assert_handshake(&mut self) {
        let msg = self.next_json().await;
        assert_eq!(msg["action"], "app-ready", "expected app-ready first");

        let msg = self.next_json().await;
        assert_eq!(
            msg["action"], "set-read-only",
            "expected set-read-only second"
        );
        assert_eq!(msg["payload"]["state"], true);
    }
}
