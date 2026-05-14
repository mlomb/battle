use std::{
    collections::HashMap,
    process::Command,
    sync::{Arc, atomic::AtomicBool},
};

use battle_wrapcmd::Transcript;
use serde::{Deserialize, Serialize};
use tempfile::tempdir;
use tokio::sync::Mutex;

use crate::exec::{Executable, Execute, ExecutionResult, Target, TargetId};
use crate::referee::Referee;

/// Lightweight game setup referencing pre-registered targets by content hash.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameSetup<T = Arc<Target>> {
    pub referee: Referee<T>,
    pub agents: Vec<T>,
    pub seed: u64,

    pub capture_io: bool,
    pub capture_game_data: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameResultData {
    pub exec_res: ExecutionResult,
    pub game_data: Option<String>,
    pub agents: Vec<GameAgentResult>,
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
    let current_exe = std::env::current_exe().expect("to get current executable path");

    // We use a temp dir that lives for the duration of this function so the transcript and
    // game data files are still present when we read them back after the referee exits.
    let game_dir = if setup.capture_io || setup.capture_game_data {
        Some(tempdir().map_err(|e| format!("failed to create temp dir for io capture: {e}"))?)
    } else {
        None
    };

    let game_data_path = if setup.capture_game_data {
        game_dir.as_ref().map(|dir| dir.path().join("game.data"))
    } else {
        None
    };

    let agent_cmds: Vec<std::process::Command> = setup
        .agents
        .iter()
        .enumerate()
        .map(|(i, a)| {
            let base_cmd = a.blocking_lock().command();

            // Optionally wrap each agent command with `<current_exe> wrap capture <path> --`.
            if setup.capture_io {
                let transcript_path = game_dir
                    .as_ref()
                    .expect("game dir available")
                    .path()
                    .join(format!("agent_{i}.io"));

                let mut w = Command::new(&current_exe);
                w.args(["wrap", "capture"]).arg(&transcript_path).arg("--");
                w.arg(base_cmd.get_program());
                w.args(base_cmd.get_args());
                w
            } else {
                base_cmd
            }
        })
        .collect();

    let mut cmd = setup
        .referee
        .command(&agent_cmds, setup.seed, game_data_path.clone());
    let result = cmd.execute(std::time::Duration::from_secs(200), abort.as_deref());
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
                let transcript = game_dir.as_ref().map(|dir| {
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
        exec_res: result,
        game_data: game_data_path.map(|path| std::fs::read_to_string(&path).unwrap_or_default()),
    })
}

impl<T: Clone> GameSetup<T> {
    pub fn all_targets(&self) -> Vec<T> {
        self.agents
            .iter()
            .chain(std::iter::once(&self.referee.target))
            .cloned()
            .collect()
    }
}

impl GameSetup<Arc<Target>> {
    pub fn to_target_id(&self) -> GameSetup<TargetId> {
        GameSetup::<TargetId> {
            referee: Referee::<TargetId> {
                protocol: self.referee.protocol.clone(),
                target: self.referee.target.id,
                min_agents: self.referee.min_agents,
                max_agents: self.referee.max_agents,
            },
            agents: self.agents.iter().map(|a| a.id).collect(),
            seed: self.seed,
            capture_io: self.capture_io,
            capture_game_data: self.capture_game_data,
        }
    }
}

impl GameSetup<TargetId> {
    pub fn to_executable(
        &self,
        targets: &HashMap<TargetId, Arc<Mutex<Executable>>>,
    ) -> GameSetup<Arc<Mutex<Executable>>> {
        GameSetup::<Arc<Mutex<Executable>>> {
            referee: Referee::<Arc<Mutex<Executable>>> {
                protocol: self.referee.protocol.clone(),
                target: targets
                    .get(&self.referee.target)
                    .expect("missing target")
                    .clone(),
                min_agents: self.referee.min_agents,
                max_agents: self.referee.max_agents,
            },
            agents: self
                .agents
                .iter()
                .map(|a| targets.get(a).expect("missing target").clone())
                .collect(),
            seed: self.seed,
            capture_io: self.capture_io,
            capture_game_data: self.capture_game_data,
        }
    }
}
