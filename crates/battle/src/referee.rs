use std::{path::Path, process::Command, sync::Arc};

use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;

use crate::exec::{CommandExt, Executable, Target, TargetKind};

/// The protocol used by the referee.
/// It defines how the agents are passed to the referee, how logs are collected, etc.
#[derive(Debug, Serialize, Deserialize, Clone)]
pub enum Protocol {
    /// CodinGame referee protocol compatible with cg-brutaltester.
    /// See: https://github.com/dreignier/cg-brutaltester
    ///
    /// Basically: `-p1 "./agent1" -p2 "./agent2"`
    CodinGame,
}

/// A referee (a program) that runs a match between agents (other programs) and collects the results.
#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Referee<T> {
    /// The protocol used by the referee
    pub protocol: Protocol,

    /// The target to execute
    pub target: T,

    /// The minimum number of agents required by the referee
    pub min_agents: usize,

    /// The maximum number of agents accepted by the referee
    pub max_agents: usize,
}

impl Referee<Arc<Target>> {
    /// Creates a new referee from a curated list of referess available in the `referees` directory.
    /// For now, only CodinGame referees are supported.
    pub fn from_preset<T: ToString>(preset: T) -> Result<Self, String> {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../referees")
            .join(format!("{}.jar", preset.to_string()));

        if !path.is_file() {
            return Err(format!("Referee not found: {}", path.to_string_lossy()));
        }

        let (min_agents, max_agents) = match preset.to_string().as_str() {
            "cg-fall-2023-fish" => (2, 2),
            "cg-winter-2024-sprawl" => (2, 2),
            "cg-spring-2024-olympics" => (3, 3),
            _ => {
                log::warn!(
                    "Referee file available '{}', but min/max agents is unknown. Assuming min=2 max=4.",
                    preset.to_string()
                );
                (2, 4)
            }
        };

        assert!(min_agents >= 1);
        assert!(min_agents <= max_agents);

        Ok(Self {
            protocol: Protocol::CodinGame,
            target: Arc::new(Target::new(TargetKind::Executable(
                Executable::from_jar(path.clone()).expect("read success"),
            ))),
            min_agents,
            max_agents,
        })
    }

    pub fn from_target(target: Target) -> Self {
        Self {
            protocol: Protocol::CodinGame,
            target: Arc::new(target),
            min_agents: 1,
            max_agents: 4,
        }
    }
}

impl Referee<Arc<Mutex<Executable>>> {
    pub fn command(&self, agent_cmds: &[Command], seed: u64) -> Command {
        let mut exe = self.target.blocking_lock();
        let mut cmd = exe.command();

        match self.protocol {
            Protocol::CodinGame => {
                for (i, agent) in agent_cmds.iter().enumerate() {
                    cmd.arg(format!("-p{}", i + 1));
                    cmd.arg(agent.command_line_string());
                }
                cmd.arg("-s").arg(seed.to_string());
            }
        }

        cmd
    }
}
