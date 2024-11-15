use crate::{
    env::Env,
    run::{execute, ExecutionResult},
};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    process::Command,
    sync::{Arc, Mutex},
};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameAgent {
    name: String,
    params: HashMap<String, f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameSetup {
    agents: Vec<GameAgent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameResult {
    scores: Vec<GameAgentResult>,
    r: ExecutionResult,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameAgentResult {
    score: f64,
    // TODO: other data
}

impl GameSetup {
    pub fn new() -> Self {
        Self {
            agents: vec![
                GameAgent {
                    name: "v04".to_string(),
                    params: HashMap::new(),
                },
                GameAgent {
                    name: "v09".to_string(),
                    params: HashMap::new(),
                },
                GameAgent {
                    name: "v09".to_string(),
                    params: HashMap::new(),
                },
            ],
        }
    }

    pub fn command(&self, env: &mut Env) -> Command {
        let mut agent_cmds = vec![];

        for ga in &self.agents {
            agent_cmds.push(
                env.get_agent(ga.name.clone())
                    .expect("agent to exist")
                    .command(),
            );
        }

        env.referee.command(&agent_cmds)
    }
}

pub fn run_game(env: Arc<Mutex<Env>>, setup: GameSetup) -> GameResult {
    let cmd = setup.command(&mut env.lock().unwrap());

    let r = execute(cmd, std::time::Duration::from_secs(15));

    GameResult { scores: vec![], r }
}
