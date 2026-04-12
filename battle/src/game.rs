use serde::{Deserialize, Serialize};

use crate::exec::command::CommandExt;
use crate::exec::execution::Execute;
use crate::{builder::Executable, referee::Referee};

/// Lightweight game setup referencing pre-registered targets by content hash.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameSetup<T> {
    pub referee: Referee<T>,
    pub agents: Vec<T>,
    pub seed: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameResult {
    pub result: Result<String, String>,
}

pub fn run_game(mut setup: GameSetup<Executable>) -> GameResult {
    let mut cmd = setup
        .referee
        .command(&setup.agents.iter().map(|a| a.command()).collect());
    let result = cmd.execute(std::time::Duration::from_secs(40));

    println!("run_game: {:?}", setup);
    GameResult {
        result: Ok(format!(
            "game referee={:?} agents={:?} seed={}",
            setup.referee.target, setup.agents, setup.seed
        )),
    }
}
