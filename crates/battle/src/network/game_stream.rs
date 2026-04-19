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
    network::WorkerServiceClient,
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
        let client = Arc::new(ConsumerConnection::new("127.0.0.1:8080").await);
        let (tx_result, rx_result) = channel::<(GameSetup, GameResultData)>(32);
        let (tx_input, mut rx_input) = channel::<GameSetup>(1);

        tokio::spawn(async move {
            let mut futs: FuturesUnordered<_> = FuturesUnordered::new();
            let mut retry_queue: VecDeque<GameSetup> = VecDeque::new();

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
                    _ = tokio::time::sleep(Duration::from_millis(100)) => {
                        if client.can_accept_game().await {
                            let game = if let Some(game) = retry_queue.pop_front() {
                                game
                            } else {
                                match rx_input.recv().await {
                                    Some(game) => game,
                                    None => break,
                                }
                            };
                            let client = client.clone();
                            futs.push(async move {
                                let res = client.run_game(&game).await;
                                (game, res)
                            });
                        }
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
    async fn new(address: &str) -> Self {
        let mut connect = tarpc::serde_transport::tcp::connect(address, Bincode::default);
        connect.config_mut().max_frame_length(usize::MAX);
        let transport = connect.await.expect("connect");
        let client = WorkerServiceClient::new(tarpc::client::Config::default(), transport).spawn();

        Self {
            client,
            known_targets: Mutex::new(HashSet::new()),
        }
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

        // register target
        // can be expensive, since targets can be large
        self.client
            .register_target(current(), target.as_ref().clone())
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
