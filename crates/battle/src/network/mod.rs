pub mod game_stream;
pub mod worker_node;

use crate::{
    exec::target::{Target, TargetId},
    game::{GameResult, GameSetup},
};

#[tarpc::service]
pub trait WorkerService {
    async fn target_exists(target_id: TargetId) -> bool;
    async fn register_target(target: Target) -> Result<(), String>;
    async fn can_accept_game() -> bool;
    async fn run_game(game: GameSetup<TargetId>) -> GameResult;
}
