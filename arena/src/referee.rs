use crate::{executable::Executable, Agent};
use std::{path::Path, process::Command};

// TODO: improve errors, we are returning strings

/// The protocol used by the referee.
/// It defines how the agents are passed to the referee, how logs are collected, etc.
#[derive(Debug)]
pub enum Protocol {
    /// CodinGame referee protocol compatible with cg-brutaltester.
    /// See: https://github.com/dreignier/cg-brutaltester
    ///
    /// Basically: `-p1 "./agent1" -p2 "./agent2"`
    CodinGame,
}

/// A referee (a program) that runs a match between agents (other programs) and collects the results.
#[derive(Debug)]
pub struct Referee {
    /// The protocol used by the referee
    protocol: Protocol,

    /// The path to the referee executable
    exe: Executable,
}

impl Referee {
    /// Creates a new referee from a curated list of referess available in the `referees` directory.
    /// Note that only CodinGame contests are supported at the moment.
    pub fn from_preset<T: ToString>(preset: T) -> Result<Self, String> {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("referees")
            .join(format!("{}.jar", preset.to_string()));

        if !path.is_file() {
            return Err(format!("Referee not found: {}", path.to_string_lossy()));
        }

        Ok(Self {
            protocol: Protocol::CodinGame,
            exe: Executable::from_jar(path),
        })
    }

    pub fn command(&mut self, agents: &Vec<Agent>) -> Command {
        let mut cmd = self.exe.command();

        for (i, agent) in agents.iter().enumerate() {
            cmd.arg(format!("-p{}", i + 1));
            cmd.arg(agent.command());
        }

        cmd
    }
}
