use std::{
    collections::{HashMap, HashSet},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

use console::style;
use futures_util::StreamExt;
use log::{error, info};
use message_io::{
    network::{Endpoint, NetEvent, Transport},
    node::{self, NodeEvent, NodeHandler, NodeListener},
};
use tarpc::{server, server::Channel, tokio_serde::formats::Bincode};
use tokio::sync::{Mutex, Semaphore};

use crate::{
    builder::build_cpp,
    exec::{
        executable::Executable,
        execution::Status,
        target::{Target, TargetId},
    },
    game::{GameResult, GameSetup, run_game},
    network::{FromClient, FromWorker, WorkerService, WorkerStats},
    referee::Referee,
};

#[derive(Debug)]
enum WorkerSignal {
    GameFinished(Endpoint, GameResult),
}

pub struct WorkerNode {
    targets: HashMap<TargetId, Arc<Mutex<Executable>>>,
}

#[derive(Clone)]
pub struct WorkerServer {
    node: Arc<std::sync::Mutex<WorkerNode>>,

    /// Limits concurrent games; `try_acquire` in `run_game` returns busy when full.
    game_slots: Arc<Semaphore>,
}

impl WorkerService for WorkerServer {
    async fn target_exists(self, _ctx: ::tarpc::context::Context, target_id: TargetId) -> bool {
        let _guard = self.node.lock();
        let node = _guard.unwrap();
        node.targets.contains_key(&target_id)
    }

    async fn register_target(
        self,
        _ctx: ::tarpc::context::Context,
        target: Target,
    ) -> Result<(), String> {
        let id = target.id();
        let executable = match target {
            Target::SourceCode(source) => build_cpp(&source.code, HashMap::new())
                .map_err(|e| format!("failed to build target: {:?}", e))?,
            Target::Executable(executable) => executable,
        };

        let mut node = self.node.lock().unwrap();
        node.targets.insert(id, Arc::new(Mutex::new(executable)));
        Ok(())
    }

    async fn can_accept_game(self, _ctx: ::tarpc::context::Context) -> bool {
        self.game_slots.available_permits() > 0
    }

    async fn run_game(
        self,
        _ctx: ::tarpc::context::Context,
        game: GameSetup<TargetId>,
    ) -> GameResult {
        let _slot = match self.game_slots.try_acquire() {
            Ok(p) => p,
            Err(_) => return Err("Busy".to_string()),
        };

        let game = {
            let node = self.node.lock().unwrap();
            GameSetup::<Arc<Mutex<Executable>>> {
                referee: Referee {
                    protocol: game.referee.protocol,
                    target: node.targets.get(&game.referee.target).unwrap().clone(),
                    min_agents: game.referee.min_agents,
                    max_agents: game.referee.max_agents,
                },
                agents: game
                    .agents
                    .iter()
                    .map(|id| node.targets.get(id).unwrap().clone())
                    .collect(),
                seed: game.seed,
                capture_io: game.capture_io,
            }
        };

        let abort_flag = Arc::new(AtomicBool::new(false));

        // If the client is disconnected, the WorkerServer will be dropped along with
        // the flag_guard, which will set the flag to true, causing the game to be cancelled.
        let _flag_guard = SignalOnDrop(abort_flag.clone());

        tokio::task::spawn_blocking(move || {
            let result = run_game(game, Some(abort_flag));

            match &result {
                Ok(result) => match &result.r.status {
                    Status::Exited(code) => {
                        info!("Game finished with code {code}")
                    }
                    Status::Timeout => error!("Game timed out"),
                    Status::Cancelled => error!("Game cancelled"),
                    Status::IoError(err) => {
                        error!("Game I/O error: {err}")
                    }
                },
                Err(err) => error!("{}", err),
            }

            result
        })
        .await
        .expect("join ok")
    }
}

pub async fn run_worker_node(threads: usize, port: u16) {
    let mut listener = tarpc::serde_transport::tcp::listen(
        (std::net::Ipv4Addr::UNSPECIFIED, port),
        Bincode::default,
    )
    .await
    .expect("listen");
    listener.config_mut().max_frame_length(usize::MAX);

    info!("Worker listening on port {}", style(port).yellow());
    info!("Using {}", style(format!("{} threads", threads)).cyan());

    let node = Arc::new(std::sync::Mutex::new(WorkerNode {
        targets: HashMap::new(),
    }));
    let game_slots = Arc::new(Semaphore::new(threads));

    let serve = listener
        .filter_map(|r| std::future::ready(r.ok()))
        .map(server::BaseChannel::with_defaults)
        .map(|channel| {
            info!("Created connection");
            let server = WorkerServer {
                node: node.clone(),
                game_slots: game_slots.clone(),
            };
            async move {
                channel
                    .execute(server.serve())
                    .for_each(|fut| {
                        tokio::spawn(fut);
                        std::future::ready(())
                    })
                    .await;
                info!("Connection closed");
            }
        })
        .buffer_unordered(10)
        .for_each(|_| async {});

    tokio::select! {
        _ = serve => {} // accept next connection
        _ = tokio::signal::ctrl_c() => {
            info!("Received Ctrl+C, stopping...");
        }
    }
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
