use clap::Parser;
use log::info;
use message_io::{
    network::{Endpoint, NetEvent, Transport},
    node::{self, NodeEvent, NodeHandler, NodeListener},
};
use std::{
    collections::{HashMap, HashSet, VecDeque},
    net::{IpAddr, SocketAddr},
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::sync::mpsc::{Receiver, Sender, channel};

use crate::{
    exec::target::{Target, TargetId},
    game::{GameId, GameResultData, GameSetup},
    network::{DEFAULT_WORKER_PORT, FromClient, FromWorker, WorkerStats},
};

fn parse_worker_address(s: &str) -> Result<SocketAddr, String> {
    // TODO: add domain resolution here?

    // 127.0.0.1:54321
    if let Ok(addr) = s.parse::<SocketAddr>() {
        return Ok(addr);
    }

    // 127.0.0.1
    if let Ok(ip) = s.parse::<IpAddr>() {
        return Ok(SocketAddr::new(ip, DEFAULT_WORKER_PORT));
    }

    Err(format!("invalid worker address: {s}"))
}

#[derive(Debug, Parser)]
pub struct NetworkArgs {
    /// Worker node addresses to connect to
    #[arg(short, long = "worker", env = "BATTLE_WORKERS", value_delimiter = ',', value_parser = parse_worker_address)]
    pub(crate) workers: Vec<SocketAddr>,
}

#[derive(Debug, Clone, Copy)]
enum Signal {
    /// Attempt (or re-attempt) a connection to the given address.
    Reconnect(SocketAddr),

    /// Attempt to send a game to an available worker
    SendGame,
}

const RECONNECT_DELAY: Duration = Duration::from_secs(2);

pub struct GameStream2 {
    /// Send game setups to be played
    pub tx: Sender<GameSetup>,

    /// Receives game results
    pub rx: Receiver<(GameSetup, GameResultData)>,
}

struct GameStreamNode {
    handler: NodeHandler<Signal>,
    listener: Option<NodeListener<Signal>>,

    rx_input: Receiver<GameSetup>,
    tx_result: Sender<(GameSetup, GameResultData)>,

    requeue: VecDeque<GameSetup>,

    workers_available: HashSet<Endpoint>,
    /// Games in flight: maps game id → full setup.
    pending_games: HashMap<Endpoint, HashMap<GameId, GameSetup>>,
    workers_stats: HashMap<Endpoint, WorkerStats>,
    /// All targets seen so far; used to respond to RequestTarget from workers.
    /// TODO: remove them from list when game is finished (and or add cache)
    known_targets: HashMap<TargetId, Arc<Target>>,
}

impl GameStreamNode {
    /// Sends a setup that already has a unique `id`. Returns the setup if no worker has capacity.
    fn try_send_to_worker(&mut self, game: GameSetup) -> Result<(), GameSetup> {
        for target in game.all_targets() {
            self.known_targets
                .entry(target.id)
                .or_insert_with(|| target.clone());
        }

        if let Some(&worker) = self.workers_available.iter().next().filter(|&worker| {
            let pending = self.pending_games.get(worker).map(|m| m.len()).unwrap_or(0);

            self.workers_stats
                .get(worker)
                .is_some_and(|stats| pending < stats.capacity)
        }) {
            println!("Sending game id={} to worker: {game:?}", game.id);

            let start_time = Instant::now();
            self.handler.network().send(
                worker,
                &postcard::to_allocvec(&FromClient::RunGame(game.to_target_id()))
                    .expect("serialize"),
            );
            let end_time = Instant::now();
            println!("Time taken to send game: {:?}", end_time - start_time);

            self.pending_games
                .entry(worker)
                .or_default()
                .insert(game.id, game);
            return Ok(());
        }
        Err(game)
    }

    fn try_send_game(&mut self) {
        let game = if let Some(g) = self.requeue.pop_front() {
            g
        } else if let Ok(g) = self.rx_input.try_recv() {
            g
        } else {
            return;
        };

        match self.try_send_to_worker(game) {
            Ok(()) => self.try_send_game(),
            Err(g) => self.requeue.push_front(g),
        }
    }

    fn run(mut self) {
        let listener = self.listener.take().unwrap();
        listener.for_each(move |event| match event {
            NodeEvent::Network(net_event) => {
                match net_event {
                    // `connect()` is non-blocking: this fires once the connection
                    // attempt resolves. `established == false` means it failed.
                    NetEvent::Connected(endpoint, established) => {
                        let addr = endpoint.addr();
                        if established {
                            info!("Connected to worker ({})", addr);
                            self.workers_available.insert(endpoint);
                        } else {
                            log::debug!(
                                "Failed to connect to worker ({}), retrying in {:?}",
                                addr,
                                RECONNECT_DELAY
                            );
                            self.handler
                                .signals()
                                .send_with_timer(Signal::Reconnect(addr), RECONNECT_DELAY);
                        }
                    }
                    NetEvent::Accepted(endpoint, _listener_id) => {
                        info!("Client ({}) accepted", endpoint.addr());
                    }
                    NetEvent::Message(endpoint, input_data) => {
                        info!(
                            "Message from {}, length: {}",
                            endpoint.addr(),
                            input_data.len()
                        );
                        let message: FromWorker =
                            postcard::from_bytes(&input_data).expect("deserialize");

                        match message {
                            FromWorker::Stats(stats) => {
                                println!("Stats: {:?}", stats);
                                self.workers_stats.insert(endpoint, stats);
                            }
                            FromWorker::RequestTarget(target_id) => {
                                println!("Requesting target: {:?}", target_id);
                                if let Some(target) = self.known_targets.get(&target_id) {
                                    let msg =
                                        FromClient::SendTarget(target_id, target.as_ref().clone());
                                    self.handler.network().send(
                                        endpoint,
                                        &postcard::to_allocvec(&msg).expect("serialize"),
                                    );
                                } else {
                                    log::error!("Worker requested unknown target {:?}", target_id);
                                    panic!("Misbehaving worker, aborting");
                                }
                            }
                            FromWorker::GameAck => {}
                            FromWorker::GameResult(game_id, result) => {
                                if let Some(game) = self.pending_games.get_mut(&endpoint).and_then(
                                    |m: &mut HashMap<GameId, GameSetup>| m.remove(&game_id),
                                ) {
                                    match result {
                                        Ok(data) => {
                                            let _ = self.tx_result.try_send((game, data));
                                        }
                                        Err(err) => {
                                            log::error!("Game failed: {err}");
                                        }
                                    }
                                } else {
                                    log::error!("Worker sent result for unknown game_id {game_id}");
                                    panic!("Misbehaving worker, aborting");
                                }
                            }
                        }
                    }
                    NetEvent::Disconnected(endpoint) => {
                        let addr = endpoint.addr();
                        log::warn!(
                            "Worker ({}) disconnected, reconnecting in {:?}",
                            addr,
                            RECONNECT_DELAY
                        );
                        self.workers_available.remove(&endpoint);
                        self.handler
                            .signals()
                            .send_with_timer(Signal::Reconnect(addr), RECONNECT_DELAY);
                    }
                }
            }
            NodeEvent::Signal(Signal::Reconnect(addr)) => {
                match self.handler.network().connect(Transport::Ws, addr) {
                    Ok(_) => info!("Attempting connection to ({})", addr),
                    Err(e) => {
                        log::debug!(
                            "connect() to {} errored: {}, retrying in {:?}",
                            addr,
                            e,
                            RECONNECT_DELAY
                        );
                        self.handler
                            .signals()
                            .send_with_timer(Signal::Reconnect(addr), RECONNECT_DELAY);
                    }
                }
            }
            NodeEvent::Signal(Signal::SendGame) => {
                self.try_send_game();

                self.handler
                    .signals()
                    .send_with_timer(Signal::SendGame, Duration::from_millis(10));
            }
        });
    }
}

impl GameStream2 {
    pub fn new(mut network_args: NetworkArgs) -> Self {
        let (tx_result, rx_result) = channel::<(GameSetup, GameResultData)>(32);
        let (tx_input, rx_input) = channel::<GameSetup>(1);

        network_args.workers.push(
            format!("127.0.0.1:{}", DEFAULT_WORKER_PORT)
                .parse::<SocketAddr>()
                .unwrap(),
        );

        let (handler, listener) = node::split::<Signal>();

        for worker in network_args.workers {
            handler.signals().send(Signal::Reconnect(worker));
        }
        handler.signals().send(Signal::SendGame);

        info!("Initialized workers");

        let node = GameStreamNode {
            handler,
            listener: Some(listener),
            rx_input,
            tx_result,
            requeue: VecDeque::new(),
            workers_available: HashSet::new(),
            pending_games: HashMap::new(),
            workers_stats: HashMap::new(),
            known_targets: HashMap::new(),
        };

        tokio::spawn(async move {
            node.run();
        });

        Self {
            tx: tx_input,
            rx: rx_result,
        }
    }
}
