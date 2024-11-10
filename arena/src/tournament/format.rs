use clap::ValueEnum;
use std::fmt::Display;

/// Tournament format
#[derive(ValueEnum, Debug, Clone)]
pub enum Format {
    /// Each agent plays against every other agent
    RoundRobin,
    /// The first agent plays against every other agent
    Gauntlet,
}

impl Format {
    // - next
}

impl Display for Format {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Format::RoundRobin => write!(f, "Round Robin (all vs all)"),
            Format::Gauntlet => write!(f, "Gauntlet (first vs all)"),
        }
    }
}
