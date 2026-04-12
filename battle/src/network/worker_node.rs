use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

use futures_util::StreamExt;
use log::info;
use tarpc::{server, server::Channel, tokio_serde::formats::Bincode};

use crate::{
    builder::build_cpp,
    network::WorkerService,
    types::{GameResult, GameSetup, Target, TargetId},
};

pub struct WorkerNode {
    targets: HashMap<TargetId, Target>,
}

impl WorkerNode {
    pub fn new() -> Self {
        Self {
            targets: HashMap::new(),
        }
    }
}

#[derive(Clone)]
pub struct WorkerServer {
    node: Arc<Mutex<WorkerNode>>,
}

impl WorkerService for WorkerServer {
    async fn target_exists(self, context: ::tarpc::context::Context, target_id: TargetId) -> bool {
        let _guard = self.node.lock();
        let node = _guard.unwrap();
        node.targets.contains_key(&target_id)
    }

    async fn register_target(
        self,
        context: ::tarpc::context::Context,
        target: Target,
    ) -> Result<(), String> {
        let _guard = self.node.lock();
        let mut node = _guard.unwrap();
        let id = target.id();
        let executable = match target {
            Target::SourceCode(source) => Target::Executable(
                build_cpp(&source.code, HashMap::new())
                    .map_err(|e| format!("failed to build target: {:?}", e))?,
            ),
            Target::Executable(executable) => Target::Executable(executable),
        };

        node.targets.insert(id, executable);
        Ok(())
    }

    async fn run_game(
        self,
        context: ::tarpc::context::Context,
        game: GameSetup<TargetId>,
    ) -> GameResult {
        println!("run_game: {:?}", game);
        GameResult {
            result: Ok(format!(
                "game referee={:016x} agents={:?} seed={}",
                game.referee, game.agents, game.seed
            )),
        }
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
