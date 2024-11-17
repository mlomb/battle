use crate::game::GameSetup;
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

pub struct Tournament {
    /// Matches in a round
    cycle_matches: Vec<GameSetup>,

    /// Current match index
    index: usize,
}

impl Tournament {
    pub fn new(format: Format, agents_per_encounter: usize) -> Self {
        let cycle_matches = match format {
            Format::RoundRobin => (0..agents_per_encounter)
                .map(|i| GameSetup::new())
                .collect(),
            Format::Gauntlet => (0..agents_per_encounter)
                .map(|i| GameSetup::new())
                .collect(),
            Format::Matchmaking => todo!(),
        };
        Self {
            cycle_matches,
            index: 0,
        }
    }

    pub fn next(&mut self) -> Option<GameSetup> {
        let index = self.index;
        self.index += 1;
        Some(self.cycle_matches[index % self.cycle_matches.len()].clone())
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
