use crate::scheduler::{Generator, MatchRequest};

pub struct GauntletTournament {
    count: u32,
}

impl Generator for GauntletTournament {
    fn next_game(&mut self) -> Option<MatchRequest> {
        Some(MatchRequest { agents: vec![] })
    }
}
