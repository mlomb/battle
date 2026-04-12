use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

use futures_util::StreamExt;
use log::info;
use tarpc::{server, server::Channel, tokio_serde::formats::Bincode};

use crate::{
    builder::{Executable, build_cpp},
    game::{GameResult, GameSetup, run_game},
    network::WorkerService,
    referee::Referee,
    types::{Target, TargetId},
};

pub struct WorkerNode {
    targets: HashMap<TargetId, Executable>,
}

#[derive(Clone)]
pub struct WorkerServer {
    node: Arc<Mutex<WorkerNode>>,
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
        let _guard = self.node.lock();
        let mut node = _guard.unwrap();
        let id = target.id();
        let executable = match target {
            Target::SourceCode(source) => build_cpp(&source.code, HashMap::new())
                .map_err(|e| format!("failed to build target: {:?}", e))?,
            Target::Executable(executable) => executable,
        };

        node.targets.insert(id, executable);
        Ok(())
    }

    async fn run_game(
        self,
        _ctx: ::tarpc::context::Context,
        game: GameSetup<TargetId>,
    ) -> GameResult {
        let _guard = self.node.lock();
        let node = _guard.unwrap();
        let game = GameSetup::<Executable> {
            referee: Referee {
                protocol: game.referee.protocol,
                target: node.targets.get(&game.referee.target).unwrap().clone(),
                min_agents: game.referee.min_agents,
                max_agents: game.referee.max_agents,
            },
            agents: game
                .agents
                .iter()
                .map(|id| node.targets.get(id).unwrap())
                .map(|executable| executable.clone())
                .collect(),
            seed: game.seed,
        };
        return run_game(game);
    }
}

pub async fn run_worker_node() {
    let mut listener = tarpc::serde_transport::tcp::listen(
        (std::net::Ipv4Addr::UNSPECIFIED, 8080),
        Bincode::default,
    )
    .await
    .expect("listen");
    listener.config_mut().max_frame_length(usize::MAX);

    info!("Worker listening on port {}", 8080);

    let node = Arc::new(Mutex::new(WorkerNode {
        targets: HashMap::new(),
    }));

    let serve = listener
        .filter_map(|r| std::future::ready(r.ok()))
        .map(server::BaseChannel::with_defaults)
        .map(|channel| {
            info!("Created connection");
            let server = WorkerServer { node: node.clone() };
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
