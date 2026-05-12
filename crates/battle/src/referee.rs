use std::{
    path::{Path, PathBuf},
    process::Command,
    sync::Arc,
};

use include_dir::{Dir, include_dir};
use log::warn;
use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;

use crate::exec::{CommandExt, Executable, Target};

static REFEREES_CG_CPP: Dir<'_> = include_dir!("$CARGO_MANIFEST_DIR/assets/cg-cpp");

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
    pub fn from_preset(preset: &str) -> Result<Self, String> {
        let target = if preset.ends_with("-ref") {
            // try to look up Jar files locally
            let path = Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../referees/cg-jar")
                .join(format!("{}.jar", preset.trim_end_matches("-ref")));

            if !path.is_file() {
                return Err(format!(
                    "Jar referee not found: {}.\n\nNote that Jar referees are not distributed via crates.io.",
                    path.to_string_lossy()
                ));
            }

            Target::from_executable(Executable::from_jar(path.clone()).expect("read success"))
        } else {
            // look up C++ referees from REFEREES_CG_CPP
            let path = PathBuf::from(format!("{}.cpp", preset));

            if !REFEREES_CG_CPP.contains(&path) {
                return Err(format!("Referee not found: {preset}"));
            }

            // extract the folder
            let tmp = tempfile::tempdir().expect("failed to create temp dir");
            REFEREES_CG_CPP
                .extract(tmp.path())
                .expect("extract success");

            Target::from_entrypoint(tmp.path().join(&path)).expect("correct bundle")
        };

        let (min_agents, max_agents) = match preset.trim_end_matches("-ref").to_string().as_str() {
            "cg-fall-2023-fish" => (2, 2),
            "cg-winter-2024-sprawl" => (2, 2),
            "cg-spring-2024-olympics" => (3, 3),
            _ => {
                log::warn!(
                    "Referee file available '{}', but min/max agents is unknown. Assuming min=2 max=4.",
                    preset
                );
                (2, 4)
            }
        };

        assert!(min_agents >= 1);
        assert!(min_agents <= max_agents);

        Ok(Self {
            protocol: Protocol::CodinGame,
            target: Arc::new(target),
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

    // TODO: return Result
    pub fn from_string(str: &str) -> Self {
        match Referee::from_preset(str) {
            Ok(referee) => referee,
            Err(err) => {
                warn!("Failed to load referee from preset: {err}");

                Referee::from_target(
                    Target::from_entrypoint(PathBuf::from(str.to_string()))
                        .expect("to compile referee"),
                )
            }
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
