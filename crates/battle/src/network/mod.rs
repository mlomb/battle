pub mod game_stream;
pub mod worker_node;

use serde::{Deserialize, Serialize};

use crate::{
    exec::target::{Target, TargetId},
    game::GameSetup,
};

/// Default port for worker nodes to listen on
pub const DEFAULT_WORKER_PORT: u16 = 13670;

#[derive(Debug, Serialize, Deserialize)]
pub struct WorkerStats {
    /// Number of connected clients, potentially sending work
    clients: u32,

    /// Number of running games
    running: u32,

    /// Maximum number of games that can be run concurrently in this worker
    capacity: u32,
}

#[derive(Debug, Serialize, Deserialize)]
pub enum FromWorker {
    Stats(WorkerStats),
    RequestTarget(TargetId),
}

#[derive(Debug, Serialize, Deserialize)]
pub enum FromClient {
    RunGame(GameSetup<TargetId>),
    SendTarget(TargetId, Target),
}
