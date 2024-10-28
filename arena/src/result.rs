use std::{collections::HashMap, pin::Pin};

use futures::Stream;
use skillratings::{
    trueskill::{trueskill_multi_team, TrueSkillConfig, TrueSkillRating},
    MultiTeamOutcome,
};
use std::task::{Context, Poll};

use crate::{agent::Agent, referee::Referee};

#[derive(Debug)]
pub struct MatchRequest {
    pub referee: Referee,
    pub agents: Vec<Agent>,
}

pub struct MatchResult {
    pub(crate) agents: Vec<Agent>,
    pub(crate) scores: Vec<u32>,
}

pub trait ResultReceiver {
    fn receive_result(&mut self, result: MatchResult);
}

pub trait Generator {}

pub struct BasicGenerator {
    count: u32,
}

impl BasicGenerator {
    pub fn new(count: u32) -> Self {
        BasicGenerator { count }
    }

    pub fn pepito(&self) {
        println!("pepito");
    }

    pub fn as_stream<'a>(&'a mut self) -> impl Stream<Item = u8> + 'a {
        futures::stream::unfold(self, |rng| async {
            let number = 5;
            Some((number, rng))
        })
    }

    pub fn next_game(&mut self) -> Option<MatchRequest> {
        if self.count > 0 {
            self.count -= 1;
            Some(MatchRequest {
                referee: Referee::new("summer-2024-olympics-1.0-SNAPSHOT.jar".into()),
                agents: vec![
                    Agent::new("mlomb-146-2.exe".into()),
                    Agent::new("SMITS_v04.exe".into()),
                    Agent::new("SMITS_v09.exe".into()),
                ],
            })
        } else {
            None
        }
    }
}

#[derive(Debug)]
pub struct Summary {
    total: u32,
    ratings: HashMap<String, TrueSkillRating>,
    rank_counts: HashMap<String, HashMap<u32, u32>>,
}

impl Summary {
    pub fn new() -> Self {
        Summary {
            total: 0,
            ratings: HashMap::new(),
            rank_counts: HashMap::new(),
        }
    }

    pub fn print(&self) {
        println!("================== Summary ==================");

        let mut ratings = self.ratings.clone().into_iter().collect::<Vec<_>>();

        // sort ratings from highest to lowest
        ratings.sort_by(|(_, a), (_, b)| b.rating.partial_cmp(&a.rating).unwrap());

        for (id, rating) in ratings {
            let places = self.rank_counts.get(&id).unwrap();
            let first_place = *places.get(&1).unwrap_or(&0);
            let non_first_place = *places.get(&2).unwrap_or(&0)
                + *places.get(&3).unwrap_or(&0)
                + *places.get(&4).unwrap_or(&0);

            let total = places.values().sum::<u32>();

            println!(
                "{}: [R {:.2}±{:.2}] [W {:.2}% L {:.2}%] [N {}]",
                id,
                rating.rating,
                rating.uncertainty,
                (first_place as f32) / (total as f32) * 100.0,
                (non_first_place as f32) / (total as f32) * 100.0,
                total
            );
        }
    }
}

impl ResultReceiver for Summary {
    fn receive_result(&mut self, result: MatchResult) {
        self.total += 1;

        let config = TrueSkillConfig {
            draw_probability: 0.05,
            ..Default::default()
        };

        let mut scores = result.scores.clone();
        scores.sort_by_key(|&score| std::cmp::Reverse(score)); // TODO: ASC or DESC
        scores.dedup();

        // TODO: https://docs.rs/skillratings/latest/skillratings/trueskill/index.html

        let mut teams_and_ranks = Vec::new();

        for (i, agent) in result.agents.iter().enumerate() {
            let rank = scores
                .iter()
                // TODO: -1
                .position(|&score| score == result.scores[i])
                .unwrap() as u32
                + 1;

            // fill team with rating and rank
            teams_and_ranks.push((
                // Free-for-all so a team with only one member
                vec![self
                    .ratings
                    .entry(agent.id())
                    .or_insert(TrueSkillRating::new())
                    .clone()],
                MultiTeamOutcome::new(rank as usize),
            ));

            // fill rank
            let entry = self.rank_counts.entry(agent.id()).or_insert(HashMap::new());
            let count = entry.entry(rank).or_insert(0);
            *count += 1;
        }

        let teams_and_ranks: Vec<(&[TrueSkillRating], MultiTeamOutcome)> = teams_and_ranks
            .iter()
            .map(|(ratings, outcome)| (ratings.as_slice(), *outcome))
            .collect();
        let new_teams = trueskill_multi_team(&teams_and_ranks, &config);

        for (agent, new_rating) in result.agents.iter().zip(new_teams) {
            self.ratings
                .insert(agent.id(), new_rating.first().unwrap().clone());
        }
    }
}
