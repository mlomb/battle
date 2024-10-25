use std::collections::HashMap;

use rayon::iter::{plumbing::bridge, ParallelIterator};

use crate::{agent::Agent, referee::Referee};

pub struct MatchRequest {
    pub referee: Referee,
    pub agents: Vec<Agent>,
}

pub struct MatchResult {
    a: u32,
}

pub trait ResultReceiver {
    fn receive_result(&mut self, result: MatchResult);
}

pub struct Summary {
    total: u32,
    wins: HashMap<String, u32>,
}

impl ResultReceiver for Summary {
    fn receive_result(&mut self, result: MatchResult) {
        self.total += 1;
        self.wins.insert("a".to_string(), result.a);
    }
}

pub trait Generator {
    fn request_game(&mut self) -> MatchResult;
}

pub struct BasicGenerator {
    count: u32,
}

impl BasicGenerator {
    pub fn new(count: u32) -> Self {
        BasicGenerator { count }
    }
}

impl Iterator for BasicGenerator {
    type Item = MatchRequest;

    fn next(&mut self) -> Option<Self::Item> {
        if self.count > 0 {
            self.count -= 1;
            Some(MatchRequest {
                referee: Referee::new("summer-2024-olympics-1.0-SNAPSHOT.jar".into()),
                agents: vec![
                    Agent::new("mlomb-146-2.exe".into()),
                    Agent::new("mlomb-146-2.exe".into()),
                    Agent::new("mlomb-146-2.exe".into()),
                ],
            })
        } else {
            None
        }
    }
}
