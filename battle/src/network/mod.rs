pub mod producer_node;
pub mod worker_node;

pub use producer_node::ProducerHandle;
pub use worker_node::WorkerNode;

use anyhow::{Context, Result, bail};
use futures_util::{SinkExt, StreamExt};
use log::{info, trace};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use std::collections::HashSet;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio_tungstenite::WebSocketStream;
use tokio_tungstenite::tungstenite::Message;

// ── Wire protocol ───────────────────────────────────────────────────────────

pub type TargetId = u64;

pub fn compute_target_id<T: Serialize>(target: &T) -> TargetId {
    let bytes = postcard::to_allocvec(target).expect("serialize target for hashing");
    let hash = blake3::hash(&bytes);
    u64::from_le_bytes(hash.as_bytes()[..8].try_into().unwrap())
}

/// Messages sent from the producer (server) to a worker (client).
#[derive(Serialize, Deserialize)]
pub(self) enum ServerMsg<T, W> {
    /// A target to build/prepare before work begins.
    Target(T),
    /// A unit of work to execute.
    Work(W),
}

/// Messages sent from a worker (client) back to the producer (server).
#[derive(Serialize, Deserialize)]
pub(self) enum ClientMsg<R> {
    TargetOk(TargetId),
    TargetError { hash: TargetId, error: String },
    WorkResult(R),
}

pub(self) type Sink<S> = futures_util::stream::SplitSink<WebSocketStream<S>, Message>;
pub(self) type Source<S> = futures_util::stream::SplitStream<WebSocketStream<S>>;

pub(self) async fn ws_send<S: AsyncRead + AsyncWrite + Unpin>(
    sink: &mut Sink<S>,
    msg: &impl Serialize,
) -> Result<()> {
    let bytes = postcard::to_allocvec(msg).context("serialize")?;
    sink.send(Message::Binary(bytes.into()))
        .await
        .context("send")
}

pub(self) async fn ws_recv<T: DeserializeOwned, S: AsyncRead + AsyncWrite + Unpin>(
    source: &mut Source<S>,
) -> Result<T> {
    loop {
        match source.next().await {
            Some(Ok(Message::Binary(bytes))) => {
                return postcard::from_bytes(&bytes).context("deserialize");
            }
            Some(Ok(Message::Ping(_) | Message::Pong(_))) => continue,
            Some(Ok(Message::Close(_))) | None => bail!("connection closed"),
            Some(Ok(other)) => bail!("unexpected WS message: {other:?}"),
            Some(Err(e)) => return Err(e.into()),
        }
    }
}

// ── mDNS discovery ──────────────────────────────────────────────────────────

pub(self) fn start_discovery(
    service_name: String,
    peer_id: String,
    broadcast_port: Option<u16>,
    handle: &tokio::runtime::Handle,
) -> Result<(
    tokio::sync::mpsc::Receiver<SocketAddr>,
    swarm_discovery::DropGuard,
)> {
    let (tx, rx) = tokio::sync::mpsc::channel::<SocketAddr>(64);
    let own_id = peer_id.clone();
    let mut builder = swarm_discovery::Discoverer::new_interactive(service_name, peer_id);

    if let Some(port) = broadcast_port {
        let addrs: Vec<std::net::IpAddr> = if_addrs::get_if_addrs()
            .unwrap_or_default()
            .into_iter()
            .filter(|iface| !iface.is_loopback())
            .map(|iface| iface.addr.ip())
            .collect();
        info!(
            "Broadcasting on mDNS: port={}, addrs={:?}",
            port,
            addrs.iter().map(|a| a.to_string()).collect::<Vec<_>>()
        );
        builder = builder.with_addrs(port, addrs);
    }

    let seen = Arc::new(Mutex::new(HashSet::<String>::new()));
    builder = builder.with_callback(move |peer_id_str, peer| {
        if peer_id_str == own_id {
            return;
        }
        let addrs: Vec<SocketAddr> = peer
            .addrs()
            .iter()
            .map(|(ip, port)| SocketAddr::new(*ip, *port))
            .collect();
        if addrs.is_empty() {
            return;
        }
        if !seen.lock().unwrap().insert(peer_id_str.to_string()) {
            return;
        }
        trace!("Discovered peer {} at {:?}", peer_id_str, addrs);
        for addr in addrs {
            let _ = tx.try_send(addr);
        }
    });

    let guard = builder.spawn(handle)?;
    Ok((rx, guard))
}

// ── Helpers ─────────────────────────────────────────────────────────────────

pub(self) fn random_peer_id() -> String {
    use rand::RngExt;
    let bytes: [u8; 8] = rand::rng().random();
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

pub(self) fn sanitize_protocol(protocol: &str) -> String {
    protocol
        .chars()
        .map(|c| {
            if c.is_alphanumeric() || c == '-' {
                c
            } else {
                '-'
            }
        })
        .collect()
}
