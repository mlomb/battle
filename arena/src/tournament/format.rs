use clap::ValueEnum;
use std::fmt::Display;

/// Tournament format
#[derive(ValueEnum, Debug, Clone)]
pub enum Format {
    /// Each agent plays against every other agent
    RoundRobin,
    /// The first agent plays against every other agent
    Gauntlet,
    /// ASD
    Matchmaking,
}

impl Format {
    // - next
    pub fn next(&self, index: u32, num_agents: u32, agents_per_encounter: u32) -> Vec<u32> {
        match self {
            Format::RoundRobin => (0..agents_per_encounter).collect(),
            Format::Gauntlet => (0..agents_per_encounter).collect(),
            Format::Matchmaking => todo!(),
        }
    }
}

impl Display for Format {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Format::RoundRobin => write!(f, "Round Robin (all vs all)"),
            Format::Gauntlet => write!(f, "Gauntlet (first vs all)"),
            Format::Matchmaking => write!(f, "Matchmaking"),
        }
    }
}
