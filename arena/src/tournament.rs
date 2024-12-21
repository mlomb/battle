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
    pub fn new(format: Format, agents: Vec<String>, min_agents: usize, max_agents: usize) -> Self {
        let cycle_matches = match format {
            Format::RoundRobin => {
                let mut matches = vec![];
                let mut matches_accum = vec![GameSetup::new()];

                assert!(min_agents == 2, "Implement for more than 2 agents :)");
                assert!(max_agents == 2, "Implement for more than 2 agents :)");

                // for each player slot (player 0, player 1, ...)
                for p in 1..=max_agents {
                    // copy matches with fixed number of slots
                    let previous_matches = matches_accum.clone();
                    // clear matches
                    matches_accum.clear();

                    // for each agent option
                    for next_agent in &agents {
                        // copy all previous matches and add the new agent to the end
                        for match_setup in &previous_matches {
                            if !match_setup.agents.iter().any(|a| &a.name == next_agent) {
                                matches_accum
                                    .push(match_setup.clone().with_agent(next_agent.clone()));
                            }
                        }
                    }

                    if p >= min_agents && p <= max_agents {
                        matches.append(&mut matches_accum.clone());
                    }
                }

                // shuffle for fun
                matches.shuffle(&mut rand::thread_rng());
                matches
            }
            Format::Gauntlet => todo!(),
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
