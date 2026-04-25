pub mod game_stream;
pub mod worker_node;

use message_io::network::Transport;
use serde::{Deserialize, Serialize};

use crate::{
    exec::target::{Target, TargetId},
    game::{GameResult, GameSetup},
};

/// Transport to use for message-io
pub const MESSAGE_IO_TRANSPORT: Transport = Transport::FramedTcp;

/// Default port for worker nodes to listen on
pub const DEFAULT_WORKER_PORT: u16 = 13670;

/// Unique identifier for a game
pub type GameId = u64;

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
    GameResult { id: GameId, result: GameResult },
}

#[derive(Debug, Serialize, Deserialize)]
pub enum FromClient {
    RunGame {
        id: GameId,
        game: GameSetup<TargetId>,
    },
    SendTarget(TargetId, Target),
}
