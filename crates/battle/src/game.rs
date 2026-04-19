use std::sync::{Arc, atomic::AtomicBool};

use serde::{Deserialize, Serialize};
use tempfile::tempdir;
use tokio::sync::Mutex;
use wrapcmd::transcript::Transcript;

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
    pub capture_io: bool,
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
    pub transcript: Option<Transcript>,
}

pub fn run_game(
    setup: GameSetup<Arc<Mutex<Executable>>>,
    abort: Option<Arc<AtomicBool>>,
) -> GameResult {
    let current_exe = std::env::current_exe().ok();

    // Optionally wrap each agent command with `<current_exe> wrap capture <path> --`.
    // We use a temp dir that lives for the duration of this function so the transcript
    // files are still present when we read them back after the referee exits.
    let capture_dir = if setup.capture_io {
        Some(tempdir().map_err(|e| format!("failed to create temp dir for capture: {e}"))?)
    } else {
        None
    };

    let agent_cmds: Vec<std::process::Command> = setup
        .agents
        .iter()
        .enumerate()
        .map(|(i, a)| {
            let base_cmd = a.blocking_lock().command();

            if setup.capture_io {
                let tr_path = capture_dir
                    .as_ref()
                    .unwrap()
                    .path()
                    .join(format!("agent_{i}.io"));
                let mut w = std::process::Command::new(current_exe.as_ref().unwrap());
                w.args(["wrap", "capture"]).arg(&tr_path).arg("--");
                w.arg(base_cmd.get_program());
                w.args(base_cmd.get_args());
                if let Some(cwd) = base_cmd.get_current_dir() {
                    w.current_dir(cwd);
                }
                w
            } else {
                base_cmd
            }
        })
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
            .map(|(i, _)| {
                // Read back the transcript file written by `wrap capture` for this agent.
                let transcript = capture_dir.as_ref().map(|dir| {
                    let tr_path = dir.path().join(format!("agent_{i}.io"));
                    let text = std::fs::read_to_string(&tr_path).unwrap_or_default();
                    text.parse::<Transcript>().unwrap_or_default()
                });
                GameAgentResult {
                    score: scores.get(i).copied().unwrap_or_default(),
                    transcript,
                }
            })
            .collect(),
        r: result,
    })
}
