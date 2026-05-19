use clap::Parser;
use log::{debug, error, info};
use message_io::{
    events::TimerId,
    network::{Endpoint, NetEvent},
    node::{self, NodeEvent, NodeHandler, NodeListener},
};
use std::{
    collections::{HashMap, HashSet},
    net::{IpAddr, SocketAddr},
    sync::Arc,
    thread,
    time::Duration,
};
use tokio::sync::mpsc::{Receiver, Sender, channel};

use crate::{
    exec::{Target, TargetId},
    game::{GameResultData, GameSetup},
    network::{
        DEFAULT_WORKER_PORT, FromClient, FromWorker, GameId, MESSAGE_IO_TRANSPORT, WorkerStats,
        net_deserialize, net_serialize,
    },
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

#[derive(Clone, Debug, Parser)]
pub struct NetworkArgs {
    /// Worker node addresses to connect to
    #[arg(short, long = "worker", env = "BATTLE_WORKERS", value_delimiter = ',', value_parser = parse_worker_address)]
    pub(crate) workers: Vec<SocketAddr>,
}

pub struct GameChannel {
    /// Send game setups to be played
    pub tx: Sender<GameSetup>,

    /// Receives game results
    pub rx: Receiver<(GameSetup, GameResultData)>,

    /// Share of the node's `NodeHandler` so on drop we call `NodeHandler::stop()` and
    /// the background `for_each` loop exits (see `message_io` node docs).
    node: NodeHandler<ClientSignal>,
}

impl Drop for GameChannel {
    fn drop(&mut self) {
        self.node.stop();
    }
}

impl GameChannel {
    pub fn new(mut network_args: NetworkArgs) -> Self {
        let (tx_result, rx_result) = channel::<(GameSetup, GameResultData)>(32);
        let (tx_input, rx_input) = channel::<GameSetup>(1);

        // always add the local worker by default
        network_args.workers.push(
            format!("127.0.0.1:{}", DEFAULT_WORKER_PORT)
                .parse()
                .unwrap(),
        );

        let (handler, listener) = node::split::<ClientSignal>();
        let node_shutdown = handler.clone();

        // start the connection loop for each worker
        for worker in network_args.workers {
            handler.signals().send(ClientSignal::Reconnect(worker));
        }
        // seed the dispatcher
        let initial_send_game = handler
            .signals()
            .send_with_timer(ClientSignal::SendGame, Duration::ZERO);

        let node = ClientNode {
            handler,
            listener: Some(listener),
            rx_input,
            tx_result,
            workers_available: HashSet::new(),
            pending_games: HashMap::new(),
            workers_stats: HashMap::new(),
            known_targets: HashMap::new(),
            pending_send_game: Some(initial_send_game),
        };

        thread::spawn(move || node.run());

        Self {
            tx: tx_input,
            rx: rx_result,
            node: node_shutdown,
        }
    }
}

enum ClientSignal {
    /// Attempt (or re-attempt) a connection to the given worker address.
    Reconnect(SocketAddr),

    /// This is a signal called periodically to try to send a game to an available worker.
    SendGame,
}

const RECONNECT_DELAY: Duration = Duration::from_secs(2);
const SEND_GAME_TICK: Duration = Duration::from_millis(1);

struct ClientNode {
    handler: NodeHandler<ClientSignal>,
    listener: Option<NodeListener<ClientSignal>>,

    rx_input: Receiver<GameSetup>,
    tx_result: Sender<(GameSetup, GameResultData)>,

    workers_available: HashSet<Endpoint>,
    pending_games: HashMap<Endpoint, HashMap<GameId, GameSetup>>,
    workers_stats: HashMap<Endpoint, WorkerStats>,

    /// Known targets received from rx, sent to workers when requested.
    known_targets: HashMap<TargetId, Arc<Target>>,

    /// Timer id of the currently-scheduled `SendGame` signal, if any.
    /// We keep at most one in flight; rescheduling cancels the previous one.
    pending_send_game: Option<TimerId>,
}

impl ClientNode {
    fn run(mut self) {
        let listener = self.listener.take().unwrap();
        listener.for_each(|event| match event {
            NodeEvent::Network(net_event) => match net_event {
                NetEvent::Connected(endpoint, established) => {
                    if established {
                        info!("Connected to worker {}", endpoint.addr());
                        self.workers_available.insert(endpoint);
                    } else {
                        debug!(
                            "Failed to connect to worker {}, retrying in {:?}",
                            endpoint.addr(),
                            RECONNECT_DELAY
                        );
                        self.handler.signals().send_with_timer(
                            ClientSignal::Reconnect(endpoint.addr()),
                            RECONNECT_DELAY,
                        );
                    }
                }
                NetEvent::Accepted(_, _) => (),
                NetEvent::Disconnected(endpoint) => {
                    self.workers_available.remove(&endpoint);
                    info!(
                        "Worker {} disconnected, reconnecting in {:?}",
                        endpoint.addr(),
                        RECONNECT_DELAY
                    );
                    self.handler
                        .signals()
                        .send_with_timer(ClientSignal::Reconnect(endpoint.addr()), RECONNECT_DELAY);
                }
                NetEvent::Message(endpoint, input_data) => {
                    let message: FromWorker = net_deserialize(input_data);

                    match message {
                        FromWorker::Stats(stats) => {
                            self.workers_stats.insert(endpoint, stats);
                        }
                        FromWorker::RequestTarget(target_id) => {
                            let target =
                                self.known_targets.get(&target_id).expect("to know target");

                            info!("Worker {} requested target {:?}", endpoint.addr(), target);

                            self.handler.network().send(
                                endpoint,
                                &net_serialize(FromClient::SendTarget(
                                    target_id,
                                    target.as_ref().clone(),
                                )),
                            );
                        }
                        FromWorker::GameResult {
                            id: game_id,
                            result,
                        } => {
                            let game_setup = self
                                .pending_games
                                .get_mut(&endpoint)
                                .and_then(|m: &mut HashMap<GameId, GameSetup>| m.remove(&game_id))
                                .expect("result for known game");

                            match result {
                                Ok(data) => {
                                    let _ = self.tx_result.try_send((game_setup, data));
                                }
                                Err(err) => {
                                    error!("Game failed: {err}");
                                }
                            }

                            // a slot just freed up on this worker, kick the dispatcher ASAP
                            self.schedule_send_game(Duration::ZERO);
                        }
                    }
                }
            },
            NodeEvent::Signal(ClientSignal::Reconnect(addr)) => {
                match self.handler.network().connect(MESSAGE_IO_TRANSPORT, addr) {
                    Ok(_) => debug!("Attempting connection to {}", addr),
                    Err(e) => {
                        debug!(
                            "connect() to {} errored: {}, retrying in {:?}",
                            addr, e, RECONNECT_DELAY
                        );
                        self.handler
                            .signals()
                            .send_with_timer(ClientSignal::Reconnect(addr), RECONNECT_DELAY);
                    }
                }
            }
            NodeEvent::Signal(ClientSignal::SendGame) => {
                // the timer just fired; nothing to cancel
                self.pending_send_game = None;

                if self.try_send_game() {
                    // dispatched a game, drain again ASAP (yielding to the event
                    // loop so we observe new messages/stats in between)
                    self.schedule_send_game(Duration::ZERO);
                } else {
                    // nothing to do right now, fall back to the periodic tick
                    self.schedule_send_game(SEND_GAME_TICK);
                }
            }
        });
    }

    /// Schedule a `SendGame` signal to fire after `delay`, cancelling any
    /// previously-scheduled one. This guarantees at most one `SendGame` is
    /// ever in flight, regardless of how many places call this.
    fn schedule_send_game(&mut self, delay: Duration) {
        if let Some(id) = self.pending_send_game.take() {
            self.handler.signals().cancel_timer(id);
        }
        let id = self
            .handler
            .signals()
            .send_with_timer(ClientSignal::SendGame, delay);
        self.pending_send_game = Some(id);
    }

    /// Tries to send a single game to an available worker.
    /// Returns `true` if a game was dispatched, `false` otherwise.
    fn try_send_game(&mut self) -> bool {
        // find available worker
        let Some(&worker) = self.workers_available.iter().find(|&worker| {
            let pending = self.pending_games.get(worker).map(|m| m.len()).unwrap_or(0);

            self.workers_stats
                .get(worker)
                .is_some_and(|stats| pending < stats.capacity)
        }) else {
            return false;
        };

        // get game from input channel
        let Ok(game) = self.rx_input.try_recv() else {
            return false;
        };

        for target in game.all_targets() {
            self.known_targets
                .entry(target.id)
                .or_insert_with(|| target.clone());
        }

        // generate a new id for this game
        let game_id = rand::random();

        info!("Sending game id={} to worker {}", game_id, worker.addr());

        self.handler.network().send(
            worker,
            &net_serialize(&FromClient::RunGame {
                id: game_id,
                game: game.to_target_id(),
            }),
        );

        self.pending_games
            .entry(worker)
            .or_default()
            .insert(game_id, game);

        true
    }
}
