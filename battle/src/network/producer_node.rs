use std::{collections::HashSet, sync::Arc};

use tarpc::{client, context::current, tokio_serde::formats::Bincode};

use crate::{
    network::WorkerServiceClient,
    types::{GameResult, GameSetup, Target, TargetId},
};

/// Producer Node
///
/// It will load balance the work between available workers.
pub struct ProducerNode {
    clients: Vec<ConsumerConnection>,
}

impl ProducerNode {
    pub async fn new() -> Self {
        Self {
            clients: vec![ConsumerConnection::new("127.0.0.1:8080").await],
        }
    }

    /// Decies to which client to send the game and waits for the result
    pub async fn play_game(&mut self, game: GameSetup<Arc<Target>>) -> GameResult {
        let client = self.clients.first_mut().expect("at least one client");
        client.run_game(game).await
    }
}

struct ConsumerConnection {
    client: WorkerServiceClient,

    /// Targets that the client already has available
    known_targets: HashSet<TargetId>,
}

impl ConsumerConnection {
    async fn new(address: &str) -> Self {
        let mut connect = tarpc::serde_transport::tcp::connect(address, Bincode::default);
        connect.config_mut().max_frame_length(usize::MAX);
        let transport = connect.await.expect("connect");
        let client = WorkerServiceClient::new(tarpc::client::Config::default(), transport).spawn();

        Self {
            client,
            known_targets: HashSet::new(),
        }
    }

    async fn make_target_available(&mut self, target: Arc<Target>) -> TargetId {
        let id = target.id();

        // check local known
        if self.known_targets.contains(&id) {
            return id;
        }

        // ask remotely if it's available
        if self
            .client
            .target_exists(current(), id)
            .await
            .expect("RPC call")
        {
            // no need to send again!
            self.known_targets.insert(id);
            return id;
        }

        // register target
        // can be expensive, since targets can be large
        self.client
            .register_target(current(), target.as_ref().clone())
            .await
            .expect("RPC call")
            .expect("register target");

        self.known_targets.insert(id);

        id
    }

    async fn run_game(&mut self, game: GameSetup<Arc<Target>>) -> GameResult {
        let referee = self.make_target_available(game.referee.clone()).await;
        let mut agents = Vec::with_capacity(game.agents.len());
        for agent in game.agents.iter() {
            agents.push(self.make_target_available(agent.clone()).await);
        }

        let net_setup = GameSetup::<TargetId> {
            referee,
            agents,
            seed: game.seed,
        };

        self.client
            .run_game(current(), net_setup)
            .await
            .expect("run game")
    }
}
