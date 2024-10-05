use bundler::bundle;
use clap::Parser;
use futures_channel::mpsc::channel;
use futures_channel::mpsc::Receiver;
use futures_channel::mpsc::Sender;
use futures_util::future;
use futures_util::lock::Mutex;
use futures_util::pin_mut;
use futures_util::SinkExt;
use futures_util::StreamExt;
use futures_util::TryStreamExt;
use notify::{RecursiveMode, Watcher};
use serde_json::json;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::thread;
use std::{collections::HashMap, sync::Arc};
use tokio::net::{TcpListener, TcpStream};
use tokio_tungstenite::tungstenite::Message;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    #[arg(default_value = ".")]
    package_path: PathBuf,
}

type Tx = Sender<String>;
type PeerMap = Arc<Mutex<HashMap<SocketAddr, Tx>>>;

async fn accept_connection(stream: TcpStream, rx: Receiver<String>) {
    let ws_stream = tokio_tungstenite::accept_async(stream)
        .await
        .expect("Error during the websocket handshake occurred");

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

    /*
    let forward_code = rx
        .map(|code| {
            Message::Text(
                json!({
                    "action": "update-code",
                    "payload": {
                        "play": false,
                        "code": code
                    }
                })
                .to_string(),
            )
        })
        .map(Ok)
        .forward(outgoing);
    */
    let forward_code = tokio::spawn(async move {
        let mut rx = rx;
        let mut outgoing = outgoing;

        while let Some(code) = rx.next().await {
            //println!("Sending code: {}", code);

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

    let discard_incoming = incoming.try_for_each(|msg| {
        // println!("Received a message: {}", msg.to_text().unwrap());

        future::ok(())
    });

    pin_mut!(forward_code, discard_incoming);
    future::select(forward_code, discard_incoming).await;
}

async fn accept_stream(stream: TcpStream, peer_map: PeerMap) {
    let addr = stream
        .peer_addr()
        .expect("connected streams should have a peer address");

    println!("Client connected: {}", addr);

    let (tx, rx) = channel(100);
    peer_map.lock().await.insert(addr, tx);

    accept_connection(stream, rx).await;

    println!("Client disconnected: {}", &addr);

    peer_map.lock().await.remove(&addr);
}

#[tokio::main]
async fn main() {
    let args = Args::parse();
    let peer_map = PeerMap::new(Mutex::new(HashMap::new()));

    //// Start server
    let addr = "127.0.0.1:53135";
    let listener = TcpListener::bind(&addr)
        .await
        .expect("Can't listen. Port already in use?");
    println!("CGSync listening on {}", addr);

    let peer_map1 = peer_map.clone();
    tokio::spawn(async move {
        while let Ok((stream, _)) = listener.accept().await {
            tokio::spawn(accept_stream(stream, peer_map1.clone()));
        }
    });

    // Watcher
    let (tx, rx) = std::sync::mpsc::channel();
    let watch_path = &args.package_path.join("src");
    let mut watcher = notify::recommended_watcher(tx).unwrap();
    watcher.watch(watch_path, RecursiveMode::Recursive).unwrap();
    println!("Watching \"{}\"", watch_path.display());

    for res in &rx {
        let event = res.unwrap();
        // TODO: debounce
        println!("Source changed!");

        if let Ok(source) = bundle(&args.package_path) {
            println!("Sending...");

            let peer_map2 = peer_map.clone();
            tokio::spawn(async move {
                let mut peer_map1 = peer_map2.lock().await;

                for (_, tx) in peer_map1.iter_mut() {
                    tx.try_send(source.clone()).unwrap();
                }
            });
        } else {
            println!("Failed to bundle");
        }
    }
}
