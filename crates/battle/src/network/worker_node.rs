use std::{
    collections::HashMap,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

use console::style;
use futures_util::StreamExt;
use log::{error, info};
use tarpc::{server, server::Channel, tokio_serde::formats::Bincode};
use tokio::sync::{Mutex, Semaphore};

use crate::{
    builder::build_cpp,
    exec::{
        executable::Executable,
        execution::Status,
        target::{Target, TargetId},
    },
    game::{GameResult, GameSetup, run_game},
    network::WorkerService,
    referee::Referee,
};

pub struct WorkerNode {
    targets: HashMap<TargetId, Arc<Mutex<Executable>>>,
}

#[derive(Clone)]
pub struct WorkerServer {
    node: Arc<std::sync::Mutex<WorkerNode>>,

    /// Limits concurrent games; `try_acquire` in `run_game` returns busy when full.
    game_slots: Arc<Semaphore>,
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
        let id = target.id();
        let executable = match target {
            Target::SourceCode(source) => build_cpp(&source.code, HashMap::new())
                .map_err(|e| format!("failed to build target: {:?}", e))?,
            Target::Executable(executable) => executable,
        };

        let mut node = self.node.lock().unwrap();
        node.targets.insert(id, Arc::new(Mutex::new(executable)));
        Ok(())
    }

    async fn can_accept_game(self, _ctx: ::tarpc::context::Context) -> bool {
        self.game_slots.available_permits() > 0
    }

    async fn run_game(
        self,
        _ctx: ::tarpc::context::Context,
        game: GameSetup<TargetId>,
    ) -> GameResult {
        let _slot = match self.game_slots.try_acquire() {
            Ok(p) => p,
            Err(_) => return Err("Busy".to_string()),
        };

        let game = {
            let node = self.node.lock().unwrap();
            GameSetup::<Arc<Mutex<Executable>>> {
                referee: Referee {
                    protocol: game.referee.protocol,
                    target: node.targets.get(&game.referee.target).unwrap().clone(),
                    min_agents: game.referee.min_agents,
                    max_agents: game.referee.max_agents,
                },
                agents: game
                    .agents
                    .iter()
                    .map(|id| node.targets.get(id).unwrap().clone())
                    .collect(),
                seed: game.seed,
                capture_io: game.capture_io,
            }
        };

        let abort_flag = Arc::new(AtomicBool::new(false));

        // If the client is disconnected, the WorkerServer will be dropped along with
        // the flag_guard, which will set the flag to true, causing the game to be cancelled.
        let _flag_guard = SignalOnDrop(abort_flag.clone());

        tokio::task::spawn_blocking(move || {
            let result = run_game(game, Some(abort_flag));

            match &result {
                Ok(result) => match &result.r.status {
                    Status::Exited(code) => {
                        info!("Game finished with code {code}")
                    }
                    Status::Timeout => error!("Game timed out"),
                    Status::Cancelled => error!("Game cancelled"),
                    Status::IoError(err) => {
                        error!("Game I/O error: {err}")
                    }
                },
                Err(err) => error!("{}", err),
            }

            result
        })
        .await
        .expect("join ok")
    }
}

pub async fn run_worker_node(threads: usize, port: u16) {
    let mut listener = tarpc::serde_transport::tcp::listen(
        (std::net::Ipv4Addr::UNSPECIFIED, port),
        Bincode::default,
    )
    .await
    .expect("listen");
    listener.config_mut().max_frame_length(usize::MAX);

    info!("Worker listening on port {}", style(port).yellow());
    info!("Using {}", style(format!("{} threads", threads)).cyan());

    let node = Arc::new(std::sync::Mutex::new(WorkerNode {
        targets: HashMap::new(),
    }));
    let game_slots = Arc::new(Semaphore::new(threads));

    let serve = listener
        .filter_map(|r| std::future::ready(r.ok()))
        .map(server::BaseChannel::with_defaults)
        .map(|channel| {
            info!("Created connection");
            let server = WorkerServer {
                node: node.clone(),
                game_slots: game_slots.clone(),
            };
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

/// When dropped, sets the flag to true.
struct SignalOnDrop(Arc<AtomicBool>);

impl Drop for SignalOnDrop {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Relaxed);
    }
}
