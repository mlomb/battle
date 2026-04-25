use console::style;
use log::{error, info};
use message_io::{
    network::{Endpoint, NetEvent, Transport},
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
    exec::{
        executable::Executable,
        execution::Status,
        target::{Target, TargetId},
    },
    game::{GameResult, GameSetup, run_game},
    network::{FromClient, FromWorker, WorkerStats},
};

#[derive(Debug)]
enum WorkerSignal {
    GameFinished(Endpoint, GameResult),
}

struct WaitingGame {
    /// The client that sent this game
    endpoint: Endpoint,
    game: GameSetup<TargetId>,
    /// Targets still missing before this game can run
    needed: HashSet<TargetId>,
}

pub struct WorkerNode2 {
    handler: NodeHandler<WorkerSignal>,
    listener: Option<NodeListener<WorkerSignal>>,

    /// Worker thread capacity, reported in stats
    threads: usize,

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
    /// Total number of games currently running across all threads
    running_count: usize,
}

impl WorkerNode2 {
    pub fn new(threads: usize, port: u16) -> Self {
        let (handler, listener) = node::split::<WorkerSignal>();

        handler
            .network()
            .listen(Transport::FramedTcp, ("0.0.0.0", port))
            .expect("to listen");

        info!("Worker listening on port {}", style(port).yellow());
        info!("Using {}", style(format!("{} threads", threads)).cyan());

        Self {
            handler,
            listener: Some(listener),
            threads,
            connected_clients: HashSet::new(),
            targets: HashMap::new(),
            inflight_requests: HashMap::new(),
            waiting_games: Vec::new(),
            abort_flags: HashMap::new(),
            running_count: 0,
        }
    }

    pub fn update_stats(&self) {
        let msg = FromWorker::Stats(WorkerStats {
            clients: self.connected_clients.len() as u32,
            running: self.running_count as u32,
            capacity: self.threads as u32,
        });

        for client in self.connected_clients.iter() {
            self.handler
                .network()
                .send(*client, &postcard::to_allocvec(&msg).expect("serialize"));
        }
    }

    pub fn start_game(&mut self, endpoint: Endpoint, game: GameSetup<TargetId>) {
        let abort_flag = self
            .abort_flags
            .entry(endpoint)
            .or_insert_with(|| Arc::new(AtomicBool::new(false)))
            .clone();

        self.running_count += 1;
        self.update_stats();

        let game = game.to_executable(&self.targets);
        let handler = self.handler.clone();

        std::thread::spawn(move || {
            let result = run_game(game, Some(abort_flag));
            handler
                .signals()
                .send(WorkerSignal::GameFinished(endpoint, result));
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
                    self.update_stats();
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

                    self.update_stats();
                }
                NetEvent::Message(endpoint, input_data) => {
                    info!("Message from {}", endpoint.addr());

                    let message: FromClient =
                        postcard::from_bytes(&input_data).expect("deserialize");
                    println!("message: {:?}", message);

                    match message {
                        FromClient::RunGame(game) => {
                            let needed: HashSet<TargetId> = game
                                .all_targets()
                                .into_iter()
                                .filter(|id| !self.targets.contains_key(id))
                                .collect();

                            if needed.is_empty() {
                                self.start_game(endpoint, game);
                            } else {
                                for &id in &needed {
                                    if !self.inflight_requests.contains_key(&id) {
                                        self.handler.network().send(
                                            endpoint,
                                            &postcard::to_allocvec(&FromWorker::RequestTarget(id))
                                                .expect("serialize"),
                                        );
                                        self.inflight_requests.insert(id, endpoint);
                                    }
                                }
                                self.waiting_games.push(WaitingGame {
                                    endpoint,
                                    game,
                                    needed,
                                });
                            }
                        }
                        FromClient::SendTarget(target_id, target) => {
                            let executable = match target {
                                Target::SourceCode(source) => {
                                    match build_cpp(&source.code, HashMap::new()) {
                                        Ok(exe) => exe,
                                        Err(e) => {
                                            error!("Failed to build target {target_id}: {e:?}");
                                            return;
                                        }
                                    }
                                }
                                Target::Executable(executable) => executable,
                            };

                            self.targets
                                .insert(target_id, Arc::new(tokio::sync::Mutex::new(executable)));
                            self.inflight_requests.remove(&target_id);

                            // Unblock any waiting games that now have all their targets.
                            let mut ready = Vec::new();
                            self.waiting_games.retain_mut(|w| {
                                w.needed.remove(&target_id);
                                if w.needed.is_empty() {
                                    ready.push((w.endpoint, w.game.clone()));
                                    false
                                } else {
                                    true
                                }
                            });
                            for (ep, game) in ready {
                                self.start_game(ep, game);
                            }
                        }
                    }
                }
            },
            NodeEvent::Signal(WorkerSignal::GameFinished(_endpoint, result)) => {
                match &result {
                    Ok(data) => match &data.r.status {
                        Status::Exited(code) => info!("Game finished with code {code}"),
                        Status::Timeout => error!("Game timed out"),
                        Status::Cancelled => info!("Game cancelled (client disconnected)"),
                        Status::IoError(err) => error!("Game I/O error: {err}"),
                    },
                    Err(err) => error!("Game error: {err}"),
                }

                self.running_count -= 1;
                self.update_stats();
            }
        });
    }
}

/// When dropped, sets the flag to true.
struct SignalOnDrop(Arc<AtomicBool>);

impl Drop for SignalOnDrop {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Relaxed);
    }
}
