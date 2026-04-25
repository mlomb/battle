use clap::Parser;
use log::info;
use message_io::{
    network::{NetEvent, Transport},
    node::{self, NodeEvent},
};
use std::{
    collections::{HashMap, HashSet},
    net::{IpAddr, SocketAddr},
    sync::Arc,
    time::Duration,
};
use tokio::sync::mpsc::{Receiver, Sender, channel};

use crate::{
    exec::target::{Target, TargetId},
    game::{GameResultData, GameSetup},
    network::{DEFAULT_WORKER_PORT, FromClient, FromWorker},
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

impl GameStream2 {
    pub fn new(mut network_args: NetworkArgs) -> Self {
        let (tx_result, rx_result) = channel::<(GameSetup, GameResultData)>(32);
        let (tx_input, mut rx_input) = channel::<GameSetup>(1);

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

        let mut workers_available = HashSet::new();
        let mut pending_game_acks = HashMap::new();
        let mut pending_games = HashMap::new();
        let mut workers_stats = HashMap::new();
        // All targets seen so far; used to respond to RequestTarget from workers.
        // TODO: remove them from list when game is finished (and or add cache)
        let mut known_targets: HashMap<TargetId, Arc<Target>> = HashMap::new();

        tokio::spawn(async move {
            listener.for_each(move |event| match event {
                NodeEvent::Network(net_event) => match net_event {
                    // `connect()` is non-blocking: this fires once the connection
                    // attempt resolves. `established == false` means it failed.
                    NetEvent::Connected(endpoint, established) => {
                        let addr = endpoint.addr();
                        if established {
                            info!("Connected to worker ({})", addr);
                            workers_available.insert(endpoint);
                        } else {
                            log::debug!(
                                "Failed to connect to worker ({}), retrying in {:?}",
                                addr,
                                RECONNECT_DELAY
                            );
                            handler
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

                        println!("message: {:?}", message);

                        match message {
                            FromWorker::Stats(stats) => {
                                println!("Stats: {:?}", stats);
                                workers_stats.insert(endpoint, stats);
                            }
                            FromWorker::RequestTarget(target_id) => {
                                println!("Requesting target: {:?}", target_id);
                                if let Some(target) = known_targets.get(&target_id) {
                                    let msg =
                                        FromClient::SendTarget(target_id, target.as_ref().clone());
                                    handler.network().send(
                                        endpoint,
                                        &postcard::to_allocvec(&msg).expect("serialize"),
                                    );
                                } else {
                                    log::error!("Worker requested unknown target {:?}", target_id);
                                    panic!("Misbehaving worker, aborting");
                                }
                            }
                            FromWorker::GameAck => {
                                pending_game_acks.remove(&endpoint);
                            }
                            FromWorker::GameResult(result) => {
                                if let Some(game) = pending_games
                                    .get_mut(&endpoint)
                                    .and_then(|games: &mut Vec<GameSetup>| games.pop())
                                {
                                    match result {
                                        Ok(data) => {
                                            let _ = tx_result.send((game, data));
                                        }
                                        Err(err) => {
                                            log::error!("Game failed: {err}");
                                        }
                                    }
                                } else {
                                    log::error!("Worker sent result for unknown game");
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
                        workers_available.remove(&endpoint);
                        handler
                            .signals()
                            .send_with_timer(Signal::Reconnect(addr), RECONNECT_DELAY);
                    }
                },
                NodeEvent::Signal(Signal::Reconnect(addr)) => {
                    match handler.network().connect(Transport::FramedTcp, addr) {
                        Ok(_) => info!("Attempting connection to ({})", addr),
                        Err(e) => {
                            log::debug!(
                                "connect() to {} errored: {}, retrying in {:?}",
                                addr,
                                e,
                                RECONNECT_DELAY
                            );
                            handler
                                .signals()
                                .send_with_timer(Signal::Reconnect(addr), RECONNECT_DELAY);
                        }
                    }
                }
                NodeEvent::Signal(Signal::SendGame) => {
                    // try to receive a game
                    if let Ok(game) = rx_input.try_recv() {
                        // Register all targets from this game so we can respond to RequestTarget.
                        for target in game.all_targets() {
                            known_targets
                                .entry(target.id())
                                .or_insert_with(|| target.clone());
                        }

                        if let Some(&worker) = workers_available
                            .iter()
                            .next()
                            .filter(|&worker| !pending_game_acks.contains_key(worker))
                            .filter(|&worker| {
                                workers_stats
                                    .get(worker)
                                    .is_some_and(|stats| stats.running < stats.capacity)
                            })
                        {
                            println!("Sending game to worker: {:?}", game);

                            handler.network().send(
                                worker,
                                &postcard::to_allocvec(&FromClient::RunGame(game.to_target_id()))
                                    .expect("serialize"),
                            );

                            pending_game_acks.insert(worker, true);
                            // insert to vec
                            pending_games.entry(worker).or_default().push(game);
                        }
                    }

                    // try to reschedule a game
                    handler
                        .signals()
                        .send_with_timer(Signal::SendGame, Duration::from_millis(10));
                }
            });
        });

        Self {
            tx: tx_input,
            rx: rx_result,
        }
    }
}
