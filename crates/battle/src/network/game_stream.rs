use futures_util::{StreamExt, stream::FuturesUnordered};
use std::{
    collections::{HashSet, VecDeque},
    sync::Arc,
    time::{Duration, SystemTime},
};
use tarpc::{context::current, tokio_serde::formats::Bincode};
use tokio::sync::{
    Mutex,
    mpsc::{Receiver, Sender, channel},
};

use crate::{
    exec::target::{Target, TargetId},
    game::{GameResultData, GameSetup},
    network::{WorkerServiceClient, start_discovery},
    referee::Referee,
};

/// Dispatches game setups to worker nodes and streams back results.
///
/// Accepts an input stream of [`GameSetup`]s and implements [`Stream`] yielding
/// `(GameSetup, GameResult)` pairs. Load-balances across available workers and
/// respects their capacity.
pub struct GameStream {
    /// Send game setups to be played
    pub tx: Sender<GameSetup>,

    /// Receives game results
    pub rx: Receiver<(GameSetup, GameResultData)>,
}

impl GameStream {
    pub async fn new() -> Self {
        let (tx_result, rx_result) = channel::<(GameSetup, GameResultData)>(32);
        let (tx_input, mut rx_input) = channel::<GameSetup>(1);

        tokio::spawn(async move {
            let handle = tokio::runtime::Handle::current();
            let (mut disc_rx, _disc_guard) = start_discovery(None, &handle);

            let mut futs: FuturesUnordered<_> = FuturesUnordered::new();
            let mut retry_queue: VecDeque<GameSetup> = VecDeque::new();
            let mut workers: Vec<Arc<ConsumerConnection>> = Vec::new();
            let (worker_tx, mut worker_rx) = channel::<Arc<ConsumerConnection>>(16);

            loop {
                tokio::select! {
                    item = futs.next(), if !futs.is_empty() => {
                        if let Some((game, result)) = item {
                            match result {
                                Ok(data) => {
                                    if tx_result.send((game, data)).await.is_err() {
                                        break;
                                    }
                                }
                                Err(e) => {
                                    log::warn!("game failed, queuing for retry: {}", e);
                                    retry_queue.push_back(game);
                                }
                            }
                        }
                    },
                    _rx = disc_rx.recv() => match _rx {
                        Some(addr) => {
                            let tx = worker_tx.clone();
                            tokio::spawn(async move {
                                match ConsumerConnection::new(&addr.to_string()).await {
                                    Ok(conn) => {
                                        log::info!("Connected to worker at {}", addr);
                                        let _ = tx.send(Arc::new(conn)).await;
                                    }
                                    Err(e) => {
                                        log::warn!("Failed to connect to worker at {}: {}", addr, e);
                                    }
                                }
                            });
                        }
                        None => break,
                    },
                    conn = worker_rx.recv() => {
                        if let Some(conn) = conn {
                            workers.push(conn);
                            log::info!("Worker added to pool, total: {}", workers.len());
                        }
                    },
                    _ = tokio::time::sleep(Duration::from_millis(100)) => {
                        let available = {
                            let mut found = None;
                            for worker in &workers {
                                if worker.can_accept_game().await {
                                    found = Some(worker.clone());
                                    break;
                                }
                            }
                            found
                        };
                        let Some(worker) = available else { continue };

                        let game = if let Some(game) = retry_queue.pop_front() {
                            game
                        } else {
                            match rx_input.try_recv() {
                                Ok(game) => game,
                                Err(tokio::sync::mpsc::error::TryRecvError::Empty) => continue,
                                Err(tokio::sync::mpsc::error::TryRecvError::Disconnected) => break,
                            }
                        };
                        futs.push(async move {
                            let res = worker.run_game(&game.clone()).await;
                            (game, res)
                        });
                    }
                }
            }

            while let Some((game, result)) = futs.next().await {
                if let Ok(data) = result {
                    let _ = tx_result.send((game, data)).await;
                } else {
                    retry_queue.push_back(game);
                }
            }
        });

        Self {
            tx: tx_input,
            rx: rx_result,
        }
    }
}

struct ConsumerConnection {
    client: WorkerServiceClient,

    /// Targets that the client already has available
    known_targets: Mutex<HashSet<TargetId>>,
}

impl ConsumerConnection {
    async fn new(address: &str) -> anyhow::Result<Self> {
        let mut connect = tarpc::serde_transport::tcp::connect(address, Bincode::default);
        connect.config_mut().max_frame_length(usize::MAX);
        let transport = connect.await?;
        let client = WorkerServiceClient::new(tarpc::client::Config::default(), transport).spawn();

        Ok(Self {
            client,
            known_targets: Mutex::new(HashSet::new()),
        })
    }

    async fn make_target_available(&self, target: Arc<Target>) -> TargetId {
        let id = target.id();

        {
            let known = self.known_targets.lock().await;
            if known.contains(&id) {
                return id;
            }
        }

        // ask remotely if it's available
        if self
            .client
            .target_exists(current(), id)
            .await
            .expect("RPC call")
        {
            self.known_targets.lock().await.insert(id);
            return id;
        }

        let mut ctx = current();
        ctx.deadline = SystemTime::now() + Duration::from_secs(40);

        // register target
        // can be expensive, since targets can be large
        self.client
            .register_target(ctx, target.as_ref().clone())
            .await
            .expect("RPC call")
            .expect("register target");

        self.known_targets.lock().await.insert(id);

        id
    }

    async fn can_accept_game(&self) -> bool {
        self.client
            .can_accept_game(current())
            .await
            .expect("RPC call")
    }

    async fn run_game(&self, game: &GameSetup) -> Result<GameResultData, String> {
        let referee = self
            .make_target_available(game.referee.target.clone())
            .await;
        let mut agents = Vec::with_capacity(game.agents.len());
        for agent in game.agents.iter() {
            agents.push(self.make_target_available(agent.clone()).await);
        }

        let net_setup = GameSetup::<TargetId> {
            referee: Referee::<TargetId> {
                protocol: game.referee.protocol.clone(),
                target: referee,
                min_agents: game.referee.min_agents,
                max_agents: game.referee.max_agents,
            },
            agents,
            seed: game.seed,
            capture_io: game.capture_io,
        };

        let mut ctx = current();
        ctx.deadline = SystemTime::now() + Duration::from_secs(40);

        self.client
            .run_game(ctx, net_setup)
            .await
            .expect("RPC call")
    }
}
