pub mod producer_node;
pub mod worker_node;

pub use crate::types::TargetId;

use crate::types::{GameResult, GameSetup, Target};

#[tarpc::service]
pub trait WorkerService {
    async fn target_exists(target_id: TargetId) -> bool;
    async fn register_target(target: Target) -> Result<(), String>;
    async fn run_game(game: GameSetup<TargetId>) -> GameResult;
}
