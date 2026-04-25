pub mod game_stream;
pub mod worker_node;

use serde::{Deserialize, Serialize};

use crate::{
    exec::target::{Target, TargetId},
    game::{GameId, GameResult, GameSetup},
};

/// Default port for worker nodes to listen on
pub const DEFAULT_WORKER_PORT: u16 = 13670;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WorkerStats {
    /// Number of connected clients, potentially sending work
    clients: usize,

    /// Number of running games (same as `running_ids.len()`)
    running: usize,

    /// IDs of games currently using a thread on this worker
    running_ids: Vec<GameId>,

    /// Maximum number of games that can be run concurrently in this worker
    capacity: usize,
}

#[derive(Debug, Serialize, Deserialize)]
pub enum FromWorker {
    Stats(WorkerStats),
    RequestTarget(TargetId),
    GameAck,
    GameResult(GameId, GameResult),
}

#[derive(Debug, Serialize, Deserialize)]
pub enum FromClient {
    RunGame(GameSetup<TargetId>),
    SendTarget(TargetId, Target),
}
