use crate::scheduler::{Generator, MatchRequest};

pub struct RoundRobinTournament {
    count: u32,
}

impl Generator for RoundRobinTournament {
    fn next_game(&mut self) -> Option<MatchRequest> {
        if self.count > 0 {
            self.count -= 1;
            Some(MatchRequest { agents: vec![] })
        } else {
            None
        }
    }
}
