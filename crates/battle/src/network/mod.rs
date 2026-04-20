pub mod game_stream;
pub mod worker_node;

use log::info;
use std::{
    collections::HashSet,
    net::SocketAddr,
    sync::{Arc, Mutex},
};

use crate::{
    exec::target::{Target, TargetId},
    game::{GameResult, GameSetup},
};

#[tarpc::service]
pub trait WorkerService {
    async fn target_exists(target_id: TargetId) -> bool;
    async fn register_target(target: Target) -> Result<(), String>;
    async fn can_accept_game() -> bool;
    async fn run_game(game: GameSetup<TargetId>) -> GameResult;
}

// ── mDNS discovery ──────────────────────────────────────────────────────────

pub(self) fn start_discovery(
    broadcast_port: Option<u16>,
    handle: &tokio::runtime::Handle,
) -> (
    tokio::sync::mpsc::Receiver<SocketAddr>,
    swarm_discovery::DropGuard,
) {
    let service = format!("mlomb-bot-tools-battle");
    let peer_id = random_peer_id();

    let (tx, rx) = tokio::sync::mpsc::channel::<SocketAddr>(64);
    let own_id = peer_id.clone();
    let mut builder = swarm_discovery::Discoverer::new_interactive(service, peer_id);

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
        info!("Discovered peer {} at {:?}", peer_id_str, addrs);
        for addr in addrs {
            let _ = tx.try_send(addr);
        }
    });

    let guard = builder.spawn(handle).unwrap();
    (rx, guard)
}

fn random_peer_id() -> String {
    use rand::RngExt;
    let bytes: [u8; 8] = rand::rng().random();
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}
