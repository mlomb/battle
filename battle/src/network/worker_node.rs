use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};

use anyhow::Result;
use futures_util::StreamExt;
use log::{info, trace, warn, error};
use tokio::net::TcpListener;
use tokio::runtime::Runtime;
use tokio_tungstenite::WebSocketStream;

use super::{
    ClientMsg, ServerMsg, TargetId, compute_target_id, random_peer_id, sanitize_protocol,
    start_discovery, ws_recv, ws_send,
};
use crate::builder::{Executable, SourceBuilder};
use crate::types::{GameResult, GameSetup, Target};

struct WorkEntry {
    game: GameSetup,
    tx: tokio::sync::oneshot::Sender<GameResult>,
}

/// Shared state accessed by all connection handlers and worker threads.
struct State {
    built_targets: Mutex<HashMap<TargetId, Executable>>,
    work_tx: async_channel::Sender<WorkEntry>,
}

impl State {
    fn run_game(&self, game: &GameSetup) -> GameResult {
        let targets = self.built_targets.lock().unwrap();
        let _referee = targets.get(&game.referee_id);
        let _agents: Vec<_> = game
            .agent_ids
            .iter()
            .filter_map(|id| targets.get(id))
            .collect();

        // TODO: actually run the game with the referee and agents
        GameResult {
            result: Ok(format!(
                "game referee={:016x} agents={:?} seed={}",
                game.referee_id, game.agent_ids, game.seed
            )),
        }
    }

    /// Drives a single producer connection: handles target setup and work requests.
    async fn handle_conn(
        self: Arc<Self>,
        ws: WebSocketStream<tokio::net::TcpStream>,
        addr: SocketAddr,
    ) -> Result<()> {
        let (mut sink, mut source) = ws.split();

        loop {
            match ws_recv::<ServerMsg<Target, GameSetup>, _>(&mut source).await? {
                ServerMsg::Target(target) => {
                    let id = compute_target_id(&target);
                    trace!("Received target {:016x}", id);

                    let state = self.clone();
                    let build_result = tokio::task::spawn_blocking(move || {
                        if state.built_targets.lock().unwrap().contains_key(&id) {
                            return Ok(());
                        }
                        info!("Building target {:016x}...", id);
                        let executable = match target {
                            Target::SourceCode(source) => {
                                source.build(HashMap::new()).map_err(|e| e.to_string())?
                            }
                            Target::Executable(exec) => exec,
                        };
                        info!("Target {:016x} built OK", id);
                        state.built_targets.lock().unwrap().insert(id, executable);
                        Ok::<(), String>(())
                    })
                    .await
                    .map_err(|e| anyhow::anyhow!("on_target panicked: {e}"))?;

                    match build_result {
                        Ok(()) => {
                            ws_send(&mut sink, &ClientMsg::<GameResult>::TargetOk(id)).await?;
                        }
                        Err(error) => {
                            warn!("Target {:016x} failed: {}", id, error);
                            ws_send(
                                &mut sink,
                                &ClientMsg::<GameResult>::TargetError { hash: id, error },
                            )
                            .await?;
                        }
                    }
                }
                ServerMsg::Work(game) => {
                    trace!("Received work from {}", addr);
                    let (tx, rx) = tokio::sync::oneshot::channel();
                    self.work_tx
                        .send(WorkEntry { game, tx })
                        .await
                        .map_err(|_| anyhow::anyhow!("work channel closed"))?;
                    let result = rx.await.map_err(|_| anyhow::anyhow!("result dropped"))?;
                    ws_send(&mut sink, &ClientMsg::<GameResult>::WorkResult(result)).await?;
                }
            }
        }
    }
}

pub struct WorkerNode {
    _runtime: Runtime,
    threads: Vec<std::thread::JoinHandle<()>>,
}

impl WorkerNode {
    pub fn new(protocol: &str, num_threads: usize) -> Self {
        let runtime = Runtime::new().expect("tokio runtime");
        let (work_tx, work_rx) = async_channel::bounded::<WorkEntry>(1);
        let service = sanitize_protocol(protocol);
        let peer_id = random_peer_id();
        let state = Arc::new(State {
            built_targets: Default::default(),
            work_tx,
        });

        let state_net = state.clone();
        runtime.spawn(async move {
            let listener = TcpListener::bind("0.0.0.0:0").await.expect("bind");
            let port = listener.local_addr().expect("local addr").port();
            info!("Worker listening on port {}", port);

            let handle = tokio::runtime::Handle::current();
            let (_rx, _guard) =
                start_discovery(service, peer_id, Some(port), &handle).expect("mDNS");

            loop {
                tokio::select! {
                    accept = listener.accept() => match accept {
                        Ok((stream, addr)) => {
                            info!("Producer connected from {}", addr);
                            let state = state_net.clone();
                            tokio::spawn(async move {
                                match tokio_tungstenite::accept_async(stream).await {
                                    Ok(ws) => {
                                        if let Err(e) = state.handle_conn(ws, addr).await {
                                            info!("Producer {} disconnected: {}", addr, e);
                                        }
                                    }
                                    Err(e) => warn!("WS handshake failed for {}: {}", addr, e),
                                }
                            });
                        }
                        Err(e) => warn!("Accept failed: {}", e),
                    },
                    _ = tokio::signal::ctrl_c() => {
                        info!("Received Ctrl+C, stopping...");
                        state_net.work_tx.close();
                        break;
                    }
                }
            }

            info!("No longer accepting connections, waiting for in-flight work to finish...");
        });

        let threads = (0..num_threads)
            .map(|_| {
                let work_rx = work_rx.clone();
                let state = state.clone();
                std::thread::spawn(move || {
                    loop {
                        match work_rx.recv_blocking() {
                            Ok(entry) => {
                                let _ = entry.tx.send(state.run_game(&entry.game));
                            }
                            Err(_) => break,
                        }
                    }
                })
            })
            .collect();

        WorkerNode {
            _runtime: runtime,
            threads,
        }
    }

    /// Block until Ctrl+C is received, then drain all in-flight work and shut down cleanly.
    pub fn wait(self) {
        for thread in self.threads {
            if let Err(e) = thread.join() {
                error!("Worker thread panicked: {:?}", e);
            }
        }
        info!("All worker threads finished, shutting down.");
    }
}
