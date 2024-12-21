use crate::game::GameSetup;
use clap::ValueEnum;
use rand::seq::SliceRandom;
use std::fmt::Display;

/// Tournament format
#[derive(ValueEnum, Debug, Clone)]
pub enum Format {
    /// Each agent plays against every other agent
    RoundRobin,
    /// The first agent plays against every other agent
    Gauntlet,
    /// NOT IMPLEMENTED YET
    Matchmaking,
}

pub struct Tournament {
    /// Matches in a round
    cycle_matches: Vec<GameSetup>,

    /// Current match index
    index: usize,
}

impl Tournament {
    pub fn new(format: Format, agents: Vec<Vec<String>>) -> Self {
        let cycle_matches = match format {
            Format::RoundRobin => {
                let mut matches = vec![GameSetup::new()];

                // for each player slot (player 0, player 1, ...)
                for p in 0..agents.len() {
                    // copy matches with fixed number of slots
                    let previous_matches = matches.clone();
                    // clear matches
                    matches.clear();

                    // for each agent option (agent 0, agent 1, ...)
                    for i in 0..agents[p].len() {
                        // copy all previous matches and add the new agent to the end
                        for match_setup in &previous_matches {
                            matches.push(match_setup.clone().with_agent(agents[p][i].clone()));
                        }
                    }
                }

                // shuffle for fun
                matches.shuffle(&mut rand::thread_rng());

                matches
            }
            Format::Gauntlet => (0..1).map(|i| GameSetup::new()).collect(),
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
            Format::Matchmaking => write!(f, "Matchmaking (seed based on uncertainty)"),
        }
    }
}
