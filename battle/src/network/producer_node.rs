use std::{
    collections::HashSet,
    sync::Arc,
    time::{Duration, SystemTime},
};

use tarpc::{context::current, tokio_serde::formats::Bincode};
use tokio::sync::Mutex;

use crate::{
    exec::target::{Target, TargetId},
    game::{GameResult, GameSetup},
    network::WorkerServiceClient,
    referee::Referee,
};

/// Producer Node
///
/// It will load balance the work between available workers.
pub struct ProducerNode {
    clients: Vec<ConsumerConnection>,
}

// TODO: I think we can encapsulate this as a stream at some point
impl ProducerNode {
    pub async fn new() -> Self {
        Self {
            clients: vec![ConsumerConnection::new("127.0.0.1:8080").await],
        }
    }

    /// Checks worker capacity, then returns a future that performs the RPC game run.
    /// [`None`] means the worker had no capacity (same as [`Err`] `"Busy"` at the call site).
    pub async fn play_game(
        &self,
        game: GameSetup<Arc<Target>>,
    ) -> Option<impl std::future::Future<Output = (GameSetup<Arc<Target>>, GameResult)> + '_> {
        let client = self.clients.first().expect("at least one client");

        if client.can_accept_game().await {
            Some(async move {
                let res = client.run_game(&game).await;
                (game, res)
            })
        } else {
            None
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

    async fn run_game(&self, game: &GameSetup<Arc<Target>>) -> GameResult {
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
        };

        let mut ctx = current();
        ctx.deadline = SystemTime::now() + Duration::from_secs(40);

        self.client
            .run_game(ctx, net_setup)
            .await
            .expect("RPC call")
    }
}
