use std::sync::{Arc, atomic::AtomicBool};

use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;

use crate::exec::{
    executable::Executable,
    execution::{Execute, ExecutionResult},
};
use crate::referee::Referee;

/// Lightweight game setup referencing pre-registered targets by content hash.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameSetup<T> {
    pub referee: Referee<T>,
    pub agents: Vec<T>,
    pub seed: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameResultData {
    pub agents: Vec<GameAgentResult>,
    pub r: ExecutionResult,
}

pub type GameResult = Result<GameResultData, String>;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameAgentResult {
    pub score: i32,
    // TODO: other data
}

pub fn run_game(
    setup: GameSetup<Arc<Mutex<Executable>>>,
    abort: Option<Arc<AtomicBool>>,
) -> GameResult {
    let agent_cmds: Vec<std::process::Command> = setup
        .agents
        .iter()
        .map(|a| a.blocking_lock().command())
        .collect();
    let mut cmd = setup.referee.command(&agent_cmds, setup.seed);
    let result = cmd.execute(std::time::Duration::from_secs(40), abort.as_deref());
    let scores = result
        .stdout
        .split_whitespace()
        .map(|s| s.parse())
        .filter_map(Result::ok)
        .collect::<Vec<i32>>();

    Ok(GameResultData {
        agents: setup
            .agents
            .iter()
            .enumerate()
            .map(|(i, _)| GameAgentResult {
                score: scores.get(i).copied().unwrap_or_default(),
            })
            .collect(),
        r: result,
    })
}
