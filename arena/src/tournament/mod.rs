pub mod format;
pub mod gauntlet;
pub mod round_robin;

use crate::scheduler::{Generator, MatchRequest};

pub struct Tournament {
    count: u32,
}

impl Generator for Tournament {
    fn next_game(&mut self) -> Option<MatchRequest> {
        Some(MatchRequest {
            agents: vec!["a".to_string(), "b".to_string(), "c".to_string()],
        })
    }
}
