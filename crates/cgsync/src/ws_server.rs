use console::style;
use futures_util::SinkExt;
use futures_util::StreamExt;
use indicatif::HumanCount;
use serde_json::json;
use std::time::Duration;
use tokio::net::{TcpListener, TcpStream};
use tokio::select;
use tokio::sync::watch::Receiver;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::WebSocketStream;

type CodeRx = Receiver<String>;

/// A wrapper for a WebSocket connection.
/// Is used to send code updates to the CGLocalExtension in the browser.
struct CGLocalClient {
    ws_stream: WebSocketStream<TcpStream>,
}

impl CGLocalClient {
    async fn run_from_stream(stream: TcpStream, code_rx: CodeRx) {
        let addr = stream
            .peer_addr()
            .expect("connected streams should have a peer address");

        let ws_stream = tokio_tungstenite::accept_async(stream)
            .await
            .expect("websocket handshake should succeed");

        println!("{} Client connected: {}", style("[+]").green(), addr);
        Self { ws_stream }.run_loop(code_rx).await;
        println!("{} Connection closed: {}", style("[-]").red(), addr);
    }

    async fn run_loop(mut self, mut code_rx: CodeRx) {
        self.send_init().await;

        loop {
            select! {
                // wait for code updates
                // it will be triggered automatically the first time
                _ = code_rx.changed() => {
                    let code = code_rx.borrow().clone();
                    self.send_code(&code).await;

                    println!(
                        "{} Code updated {}",
                        style("[U]").green(),
                        style(format!(
                            "({} chars)",
                            HumanCount(code.chars().count() as u64).to_string()
                        ))
                        .cyan()
                        .bold()
                    );
                },

                // receive messages
                msg = self.ws_stream.next() => match msg {
                    Some(Ok(Message::Text(_msg))) => {}, // discard incoming (valid) messages
                    _ => break, // close the connection
                },

                // every 10s of inactivity, send a ping
                // this prevents the browser from closing the connection (because it is running in a service worker)
                _ = tokio::time::sleep(Duration::from_secs(10)) => self.send_ping().await
            }
        }
    }

    async fn send(&mut self, msg: serde_json::Value) {
        self.ws_stream
            .send(Message::Text(msg.to_string().into()))
            .await
            // we don't care about errors when sending messages
            .ok();
    }

    async fn send_ping(&mut self) {
        // the extension does not support this action type, however it does not crash either
        // we just use it to keep the connection alive
        self.send(json!({"action":"ping"})).await;
    }

    async fn send_code(&mut self, code: &String) {
        self.send(json!({"action":"update-code", "payload": { "play": false, "code": code } }))
            .await;
    }

    async fn send_init(&mut self) {
        // notify the client that the app is ready
        self.send(json!({"action":"app-ready"})).await;
        // set the client to read-only mode (one-way sync)
        self.send(json!({"action":"set-read-only", "payload": { "state": true } }))
            .await;
    }
}

pub async fn start_ws_server(code_rx: CodeRx) {
    let addr = "127.0.0.1:53135";
    let listener = TcpListener::bind(&addr)
        .await
        .expect("Can't listen on port 53135. Port already in use?");

    println!("{} CGSync listening on {}", style("[I]").blue(), addr);
    println!(
        "{} Click the {} in your browser to connect",
        style("[I]").blue(),
        style("CG Local extension").yellow().underlined()
    );

    tokio::spawn(async move {
        while let Ok((stream, _)) = listener.accept().await {
            tokio::spawn(CGLocalClient::run_from_stream(stream, code_rx.clone()));
        }
    });
}
