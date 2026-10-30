use clap::Parser;
use log::{debug, info};
use message_io::{
    events::TimerId,
    network::{Endpoint, NetEvent},
    node::{self, NodeEvent, NodeHandler, NodeListener},
};
use std::{
    collections::{HashMap, HashSet},
    net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream},
    sync::{Arc, OnceLock},
    thread,
    time::Duration,
};
use tokio::sync::mpsc::{
    Receiver, Sender, UnboundedReceiver, UnboundedSender, channel, unbounded_channel,
};

use crate::{
    exec::{Target, TargetId},
    game::{GameResult, GameSetup},
    network::{
        DEFAULT_WORKER_PORT, FromClient, FromWorker, GameId, MESSAGE_IO_TRANSPORT, WorkerStats,
        net_deserialize, net_serialize,
        worker_node::{WorkerNode, default_threads},
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

/// The worker on this machine: the one on the default port, or, when it is not running and no
/// other worker is given (`standalone`), one started inside this process (once).
fn local_worker(standalone: bool) -> SocketAddr {
    let default = SocketAddr::from((Ipv4Addr::LOCALHOST, DEFAULT_WORKER_PORT));
    if !standalone || TcpStream::connect_timeout(&default, Duration::from_millis(200)).is_ok() {
        return default;
    }

    static IN_PROCESS: OnceLock<SocketAddr> = OnceLock::new();
    *IN_PROCESS.get_or_init(|| {
        let threads = default_threads();
        let (node, _, addr) = WorkerNode::new(threads, (Ipv4Addr::LOCALHOST, 0).into());
        thread::spawn(move || node.run());
        info!(target: "battle", "No worker running, playing in this process with {threads} threads");
        addr
    })
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

    /// Receives game results (or why a game could not be played)
    pub rx: UnboundedReceiver<(GameSetup, GameResult)>,

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
        let (tx_result, rx_result) = unbounded_channel::<(GameSetup, GameResult)>();
        let (tx_input, rx_input) = channel::<GameSetup>(1);

        // always add the local worker by default
        let standalone = network_args.workers.is_empty();
        network_args.workers.push(local_worker(standalone));

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
    tx_result: UnboundedSender<(GameSetup, GameResult)>,

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

                            // forget the targets no pending game uses (e.g. playback transcripts);
                            // the next game that needs one adds it back
                            for target in game_setup.all_targets() {
                                let in_use = self
                                    .pending_games
                                    .values()
                                    .flat_map(|games| games.values())
                                    .any(|g| g.all_targets().iter().any(|t| t.id == target.id));
                                if !in_use {
                                    self.known_targets.remove(&target.id);
                                }
                            }

                            let _ = self.tx_result.send((game_setup, result));

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
        // pick the worker with the most free slots (capacity - pending). This
        // keeps all workers saturated proportionally to their capacity, instead
        // of always feeding the first one that has any free slot.
        let Some(&worker) = self
            .workers_available
            .iter()
            .filter_map(|worker| {
                let capacity = self.workers_stats.get(worker)?.capacity;
                let pending = self.pending_games.get(worker).map(|m| m.len()).unwrap_or(0);
                let free = capacity.checked_sub(pending)?;
                (free > 0).then_some((worker, free))
            })
            .max_by_key(|&(_, free)| free)
            .map(|(w, _)| w)
        else {
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
