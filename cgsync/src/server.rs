use futures_channel::mpsc::unbounded;
use futures_channel::mpsc::Receiver;
use futures_channel::mpsc::UnboundedReceiver;
use futures_channel::mpsc::UnboundedSender;
use futures_util::future;
use futures_util::lock::Mutex;
use futures_util::pin_mut;
use futures_util::SinkExt;
use futures_util::StreamExt;
use futures_util::TryStreamExt;
use serde_json::json;
use std::net::SocketAddr;
use std::{collections::HashMap, sync::Arc};
use tokio::net::{TcpListener, TcpStream};
use tokio_tungstenite::tungstenite::Message;

type CodeTx = UnboundedSender<String>;
type CodeRx = UnboundedReceiver<String>;
pub type PeerMap = Arc<Mutex<HashMap<SocketAddr, CodeTx>>>;

/// Accepts a WebSocket connection and forwards code messages to it
async fn accept_ws(stream: TcpStream, mut rx: CodeRx) {
    let ws_stream = tokio_tungstenite::accept_async(stream)
        .await
        .expect("websocket handshake should succeed");

    let (mut outgoing, incoming) = ws_stream.split();

    outgoing
        .send(Message::Text(r#"{"action":"app-ready"}"#.to_string()))
        .await
        .unwrap();
    outgoing
        .send(Message::Text(
            r#"{"action":"set-read-only", "payload": { "state": true } }"#.to_string(),
        ))
        .await
        .unwrap();

    let forward_code = tokio::spawn(async move {
        while let Some(code) = rx.next().await {
            match outgoing
                .send(Message::Text(
                    json!({
                        "action": "update-code",
                        "payload": {
                            "play": false,
                            "code": code
                        }
                    })
                    .to_string(),
                ))
                .await
            {
                Ok(_) => {}
                Err(e) => {
                    println!("Error sending code: {}", e);
                    break;
                }
            }
        }
    });

    let discard_incoming = incoming.try_for_each(|_msg| {
        // println!("Received a message: {}", msg.to_text().unwrap());

        future::ok(())
    });

    pin_mut!(forward_code, discard_incoming);
    future::select(forward_code, discard_incoming).await;
}

async fn stream_thread(stream: TcpStream, peer_map: PeerMap, initial_code: Option<String>) {
    let addr = stream
        .peer_addr()
        .expect("connected streams should have a peer address");

    println!("Client connected: {}", addr);

    let (tx, rx) = unbounded();
    if let Some(initial_code) = initial_code {
        tx.unbounded_send(initial_code).unwrap();
    }

    peer_map.lock().await.insert(addr, tx);
    accept_ws(stream, rx).await;
    peer_map.lock().await.remove(&addr);

    println!("Client disconnected: {}", &addr);
}

pub async fn start_server(mut code_rx: Receiver<String>) {
    let addr = "127.0.0.1:53135";
    let listener = TcpListener::bind(&addr)
        .await
        .expect("Can't listen on port 53135. Port already in use?");
    println!("CGSync listening on {}", addr);

    let last_value = Arc::new(Mutex::new(None));
    let peer_map = PeerMap::new(Mutex::new(HashMap::new()));

    let peer_map1 = peer_map.clone();
    let last_value1 = last_value.clone();

    tokio::spawn(async move {
        while let Ok((stream, _)) = listener.accept().await {
            tokio::spawn(stream_thread(
                stream,
                peer_map1.clone(),
                last_value1.clone().lock().await.clone(),
            ));
        }
    });

    tokio::spawn(async move {
        while let Some(code) = code_rx.next().await {
            last_value.lock().await.replace(code.clone());

            for (_, tx) in peer_map.lock().await.iter_mut() {
                tx.unbounded_send(code.clone()).unwrap();
            }
        }
    });
}
