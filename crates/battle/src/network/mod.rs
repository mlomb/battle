pub mod client_node;
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
    /// The worker's current statistics, sent automatically by the worker node
    /// so clients can monitor the worker's load and send work to it.
    Stats(WorkerStats),

    /// The worker is requesting a target, since it doesn't have it yet.
    RequestTarget(TargetId),

    /// The worker has finished a game, and is sending the result back to the client.
    GameResult { id: GameId, result: GameResult },
}

#[derive(Debug, Serialize, Deserialize)]
pub enum FromClient {
    /// The client is requesting a game to be run.
    RunGame {
        id: GameId,
        game: GameSetup<TargetId>,
    },

    /// The client has compiled a target, and is sending it to the worker.
    /// This is a response to the [`FromWorker::RequestTarget`] message.
    SendTarget(TargetId, Target),
}

pub fn net_serialize<T: Serialize>(data: T) -> Vec<u8> {
    postcard::to_allocvec(&data).expect("serialize")
}

pub fn net_deserialize<'a, T: Deserialize<'a>>(data: &'a [u8]) -> T {
    postcard::from_bytes(data).expect("deserialize")
}
