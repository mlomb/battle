use log::{debug, error, info};
use message_io::{
    network::{Endpoint, NetEvent},
    node::{self, NodeEvent, NodeHandler, NodeListener},
};
use std::{
    collections::{HashMap, HashSet},
    net::SocketAddr,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
};

use crate::{
    exec::{BuildExecutable, Executable, Status, TargetId, TargetKind},
    game::{GameResult, GameSetup, run_game},
    network::{
        FromClient, FromWorker, GameId, MESSAGE_IO_TRANSPORT, WorkerStats, net_deserialize,
        net_serialize,
    },
};

#[derive(Debug)]
pub enum WorkerSignal {
    GameFinished(Endpoint, GameId, GameResult),
}

struct WaitingGame {
    /// The client that sent this game
    endpoint: Endpoint,
    id: GameId,
    game: GameSetup<TargetId>,
    /// Targets still missing before this game can run
    needed: HashSet<TargetId>,
}

pub struct WorkerNode {
    handler: NodeHandler<WorkerSignal>,
    listener: Option<NodeListener<WorkerSignal>>,

    /// Number of games that can be run concurrently
    capacity: usize,

    connected_clients: HashSet<Endpoint>,

    /// Compiled executables ready to be used in games
    targets: HashMap<TargetId, Arc<tokio::sync::Mutex<Executable>>>,
    /// Targets we have sent `RequestTarget` for, mapped to the endpoint we asked
    inflight_requests: HashMap<TargetId, Endpoint>,
    /// Games waiting for one or more targets to become available
    waiting_games: Vec<WaitingGame>,

    /// One shared abort flag per connected client; set to true on disconnect to
    /// cancel all running games from that client.
    abort_flags: HashMap<Endpoint, Arc<AtomicBool>>,
    /// Games currently running a thread (see `threads` for capacity), with the targets they use.
    running_games: HashMap<GameId, Vec<TargetId>>,

    /// Messages to send, written by a dedicated thread: message_io's `send` spins until the
    /// peer reads, so sending from the event loop deadlocks when the peer is sending too.
    outbox: mpsc::Sender<(Endpoint, Vec<u8>)>,
}

/// Physical CPUs minus 2, at least 1.
pub fn default_threads() -> usize {
    num_cpus::get_physical().saturating_sub(2).max(1)
}

impl WorkerNode {
    /// Listens on `addr` (port 0 picks a free port) and returns the address actually bound.
    pub fn new(capacity: usize, addr: SocketAddr) -> (Self, NodeHandler<WorkerSignal>, SocketAddr) {
        let (handler, listener) = node::split::<WorkerSignal>();

        let (_, addr) = handler
            .network()
            .listen(MESSAGE_IO_TRANSPORT, addr)
            .expect("to listen");

        let (outbox, outbox_rx) = mpsc::channel::<(Endpoint, Vec<u8>)>();
        let sender = handler.clone();
        std::thread::spawn(move || {
            for (endpoint, data) in outbox_rx {
                sender.network().send(endpoint, &data);
            }
        });

        (
            Self {
                handler: handler.clone(),
                listener: Some(listener),
                capacity,
                connected_clients: HashSet::new(),
                targets: HashMap::new(),
                inflight_requests: HashMap::new(),
                waiting_games: Vec::new(),
                abort_flags: HashMap::new(),
                running_games: HashMap::new(),
                outbox,
            },
            handler,
            addr,
        )
    }

    fn send(&self, endpoint: Endpoint, data: Vec<u8>) {
        let _ = self.outbox.send((endpoint, data));
    }

    /// Send the updated stats to all connected clients.
    pub fn send_stats_update(&self) {
        let stats = WorkerStats {
            clients: self.connected_clients.len(),
            running: self.running_games.len(),
            running_ids: self.running_games.keys().copied().collect(),
            capacity: self.capacity,
        };
        let data = net_serialize(FromWorker::Stats(stats));

        for client in self.connected_clients.iter() {
            self.send(*client, data.clone());
        }
    }

    pub fn start_game(&mut self, endpoint: Endpoint, game_id: GameId, game: GameSetup<TargetId>) {
        if self.running_games.contains_key(&game_id) {
            error!("Duplicate game_id {game_id}, ignoring");
            return;
        }
        self.running_games.insert(game_id, game.all_targets());
        let game = game.to_executable(&self.targets);
        self.send_stats_update();

        let abort_flag = self
            .abort_flags
            .entry(endpoint)
            .or_insert_with(|| Arc::new(AtomicBool::new(false)))
            .clone();

        let handler = self.handler.clone();

        std::thread::spawn(move || {
            let result = run_game(game, Some(abort_flag));

            handler
                .signals()
                .send(WorkerSignal::GameFinished(endpoint, game_id, result));
        });
    }

    pub fn run(mut self) {
        let listener = self.listener.take().unwrap();
        listener.for_each(move |event| match event {
            NodeEvent::Network(net_event) => match net_event {
                NetEvent::Connected(_, _) => (),
                NetEvent::Accepted(endpoint, _) => {
                    info!("Client ({}) connected", endpoint.addr());
                    self.connected_clients.insert(endpoint);
                    self.send_stats_update();
                }
                NetEvent::Disconnected(endpoint) => {
                    info!("Client ({}) disconnected", endpoint.addr());
                    self.connected_clients.remove(&endpoint);

                    // Collect targets whose inflight request was directed at this endpoint.
                    let dead_targets: HashSet<TargetId> = self
                        .inflight_requests
                        .iter()
                        .filter(|(_, ep)| **ep == endpoint)
                        .map(|(id, _)| *id)
                        .collect();

                    for id in &dead_targets {
                        self.inflight_requests.remove(id);
                    }

                    // Abort waiting games from this endpoint or needing a now-dead target.
                    self.waiting_games.retain(|w| {
                        let from_disconnected = w.endpoint == endpoint;
                        let needs_dead_target = w.needed.iter().any(|id| dead_targets.contains(id));
                        if from_disconnected || needs_dead_target {
                            info!(
                                "Aborting waiting game (seed={}) due to client disconnect",
                                w.game.seed
                            );
                            false
                        } else {
                            true
                        }
                    });

                    // Abort all running games from this endpoint via the shared flag.
                    if let Some(flag) = self.abort_flags.remove(&endpoint) {
                        flag.store(true, Ordering::Relaxed);
                    }

                    self.send_stats_update();
                }
                NetEvent::Message(endpoint, input_data) => {
                    let message: FromClient = net_deserialize(input_data);
                    debug!("message: {:?}", message);

                    match message {
                        FromClient::RunGame { id, game } => {
                            let needed: HashSet<TargetId> = game
                                .all_targets()
                                .into_iter()
                                .filter(|id| !self.targets.contains_key(id))
                                .collect();

                            if needed.is_empty() {
                                self.start_game(endpoint, id, game);
                            } else {
                                for &id in &needed {
                                    if !self.inflight_requests.contains_key(&id) {
                                        self.send(
                                            endpoint,
                                            net_serialize(FromWorker::RequestTarget(id)),
                                        );
                                        self.inflight_requests.insert(id, endpoint);
                                    }
                                }
                                self.waiting_games.push(WaitingGame {
                                    endpoint,
                                    id,
                                    game,
                                    needed,
                                });
                            }
                        }
                        FromClient::SendTarget(target_id, target) => {
                            let executable = match target.kind {
                                TargetKind::SourceCode(source) => match source.build() {
                                    Ok(exe) => exe,
                                    Err(e) => {
                                        error!("Failed to build target {target_id}: {e}");
                                        self.inflight_requests.remove(&target_id);

                                        // fail the games waiting for it, their clients would wait forever otherwise
                                        let (failed, waiting): (Vec<_>, Vec<_>) =
                                            std::mem::take(&mut self.waiting_games)
                                                .into_iter()
                                                .partition(|w| w.needed.contains(&target_id));
                                        self.waiting_games = waiting;
                                        for w in failed {
                                            let result = Err(format!("failed to build: {e}"));
                                            self.send(
                                                w.endpoint,
                                                net_serialize(FromWorker::GameResult {
                                                    id: w.id,
                                                    result,
                                                }),
                                            );
                                        }
                                        return;
                                    }
                                },
                                TargetKind::Executable(executable) => executable,
                            };

                            self.targets
                                .insert(target_id, Arc::new(tokio::sync::Mutex::new(executable)));
                            self.inflight_requests.remove(&target_id);

                            // Unblock any waiting games that now have all their targets.
                            let mut ready = Vec::new();
                            self.waiting_games.retain_mut(|w| {
                                w.needed.remove(&target_id);
                                if w.needed.is_empty() {
                                    ready.push((w.endpoint, w.id, w.game.clone()));
                                    false
                                } else {
                                    true
                                }
                            });
                            for (ep, id, game) in ready {
                                self.start_game(ep, id, game);
                            }
                        }
                    }
                }
            },
            NodeEvent::Signal(WorkerSignal::GameFinished(endpoint, game_id, result)) => {
                match &result {
                    Ok(data) => match &data.exec_res.status {
                        Status::Exited(code) => info!("Game finished with code {code}"),
                        Status::Timeout => error!("Game timed out"),
                        Status::Cancelled => info!("Game cancelled (client disconnected)"),
                        Status::IoError(err) => error!("Game I/O error: {err}"),
                    },
                    Err(err) => error!("Game error: {err}"),
                }

                // Send result back to the client that requested this game, if
                // it is still connected.
                self.send(
                    endpoint,
                    net_serialize(FromWorker::GameResult {
                        id: game_id,
                        result,
                    }),
                );

                // playback targets (transcripts) belong to a single game: free them
                for id in self.running_games.remove(&game_id).unwrap_or_default() {
                    let playback = self
                        .targets
                        .get(&id)
                        .is_some_and(|t| t.blocking_lock().is_playback());
                    let waited = self
                        .waiting_games
                        .iter()
                        .any(|w| w.game.all_targets().contains(&id));
                    if playback && !waited {
                        self.targets.remove(&id);
                    }
                }
                self.send_stats_update();
            }
        });
    }
}
