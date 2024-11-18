use crate::{
    env::Env,
    exec::{
        command::ToCommand,
        executable::ExecutableError,
        execution::{Execute, ExecutionResult},
    },
};
use serde::{Deserialize, Serialize};
use std::{
    process::Command,
    sync::{Arc, Mutex},
};

/// A reference to an agent by name, with additional parameters.
#[derive(Debug, Clone, Serialize, Deserialize, Eq, Hash, PartialEq)]
pub struct GameAgent {
    pub name: String,
    //params: HashMap<String, f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameSetup {
    pub agents: Vec<GameAgent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameResultData {
    pub agents: Vec<GameAgentResult>,
    pub r: ExecutionResult,
}

pub type GameResult = Result<GameResultData, ExecutableError>;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameAgentResult {
    pub agent: GameAgent,
    pub score: i32,
    // TODO: other data
}

impl GameSetup {
    pub fn new() -> Self {
        Self { agents: vec![] }
    }

    pub fn with_agent(mut self, name: String) -> Self {
        self.agents.push(GameAgent { name });
        self
    }

    pub fn command(&self, env: &mut Env) -> Result<Command, ExecutableError> {
        let mut agent_cmds = vec![];

        for ga in &self.agents {
            agent_cmds.push(
                env.get_agent(ga.name.clone())
                    .expect("agent to exist")
                    .command()?,
            );
        }

        env.referee.command(&agent_cmds)
    }
}

pub fn run_game(env: Arc<Mutex<Env>>, setup: GameSetup) -> GameResult {
    let mut cmd = setup.command(&mut env.lock().unwrap())?;
    let result = cmd.execute(std::time::Duration::from_secs(40));
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
            .map(|(i, a)| GameAgentResult {
                agent: a.clone(),
                score: scores[i],
            })
            .collect(),
        r: result,
    })
}
