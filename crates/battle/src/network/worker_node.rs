use log::{error, info};
use message_io::{
    network::{Endpoint, NetEvent},
    node::{self, NodeEvent, NodeHandler, NodeListener},
};
use std::{
    collections::{HashMap, HashSet},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

use crate::{
    builder::build_cpp,
    exec::{Executable, Status, TargetId, TargetKind},
    game::{GameResult, GameSetup, run_game},
    network::{
        FromClient, FromWorker, GameId, MESSAGE_IO_TRANSPORT, WorkerStats, net_deserialize,
        net_serialize,
    },
};

#[derive(Debug)]
enum WorkerSignal {
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
    /// IDs of games currently running a thread (see `threads` for capacity).
    running_games: HashSet<GameId>,
}

impl WorkerNode {
    pub fn new(capacity: usize, port: u16) -> Self {
        let (handler, listener) = node::split::<WorkerSignal>();

        handler
            .network()
            .listen(MESSAGE_IO_TRANSPORT, ("0.0.0.0", port))
            .expect("to listen");

        Self {
            handler,
            listener: Some(listener),
            capacity,
            connected_clients: HashSet::new(),
            targets: HashMap::new(),
            inflight_requests: HashMap::new(),
            waiting_games: Vec::new(),
            abort_flags: HashMap::new(),
            running_games: HashSet::new(),
        }
    }

    /// Send the updated stats to all connected clients.
    pub fn send_stats_update(&self) {
        let stats = WorkerStats {
            clients: self.connected_clients.len(),
            running: self.running_games.len(),
            running_ids: self.running_games.iter().copied().collect(),
            capacity: self.capacity,
        };
        let data = net_serialize(FromWorker::Stats(stats));

        for client in self.connected_clients.iter() {
            self.handler.network().send(*client, &data);
        }
    }

    pub fn start_game(&mut self, endpoint: Endpoint, game_id: GameId, game: GameSetup<TargetId>) {
        let game = game.to_executable(&self.targets);
        if !self.running_games.insert(game_id) {
            error!("Duplicate game_id {game_id}, ignoring");
            return;
        }
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
                    println!("message: {:?}", message);

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
                                        self.handler.network().send(
                                            endpoint,
                                            &net_serialize(FromWorker::RequestTarget(id)),
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
                                TargetKind::SourceCode(source) => {
                                    match build_cpp(&source.code, HashMap::new()) {
                                        Ok(exe) => exe,
                                        Err(e) => {
                                            error!("Failed to build target {target_id}: {e:?}");
                                            return;
                                        }
                                    }
                                }
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
                    Ok(data) => match &data.r.status {
                        Status::Exited(code) => info!("Game finished with code {code}"),
                        Status::Timeout => error!("Game timed out"),
                        Status::Cancelled => info!("Game cancelled (client disconnected)"),
                        Status::IoError(err) => error!("Game I/O error: {err}"),
                    },
                    Err(err) => error!("Game error: {err}"),
                }

                // Send result back to the client that requested this game, if
                // it is still connected.
                self.handler.network().send(
                    endpoint,
                    &net_serialize(FromWorker::GameResult {
                        id: game_id,
                        result,
                    }),
                );

                self.running_games.remove(&game_id);
                self.send_stats_update();
            }
        });
    }
}
