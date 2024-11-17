use crate::game::{GameAgent, GameResult};
use skillratings::{
    trueskill::{trueskill_multi_team, TrueSkillConfig, TrueSkillRating},
    MultiTeamOutcome,
};
use std::{
    collections::HashMap,
    fmt::{Display, Formatter},
};

pub struct Database {
    games: Vec<GameDb>,
    per_agent: HashMap<GameAgent, DbAgent>,
}

pub struct GameDb {
    agents: Vec<GameDbAgent>,
}

pub struct GameDbAgent {
    agent: GameAgent,
    rank: u8,
    score: i32,
}

pub struct DbAgent {
    rating: TrueSkillRating,
}

impl Database {
    pub fn new() -> Self {
        Self {
            games: vec![],
            per_agent: HashMap::new(),
        }
    }

    pub fn receive_result(&mut self, result: &GameResult) {
        let mut scores = result.agents.iter().map(|a| a.score).collect::<Vec<_>>();
        scores.sort_by_key(|&score| std::cmp::Reverse(score)); // TODO: ASC or DESC
        scores.dedup();

        let mut teams_and_ranks = Vec::new();

        for (i, agent) in result.agents.iter().enumerate() {
            let rank = scores
                .iter()
                // TODO: -1
                .position(|&score| score == result.agents[i].score)
                .unwrap() as u32
                + 1;

            // fill team with rating and rank
            teams_and_ranks.push((
                // Free-for-all so a team with only one member
                vec![
                    self.per_agent
                        .entry(agent.agent.clone())
                        .or_insert(DbAgent::new())
                        .rating,
                ],
                MultiTeamOutcome::new(rank as usize),
            ));

            // fill rank
            //let entry = self
            //    .rank_counts
            //    .entry(agent.clone())
            //    .or_insert(HashMap::new());
            //let count = entry.entry(rank).or_insert(0);
            //*count += 1;
        }

        let teams_and_ranks: Vec<(&[TrueSkillRating], MultiTeamOutcome)> = teams_and_ranks
            .iter()
            .map(|(ratings, outcome)| (ratings.as_slice(), *outcome))
            .collect();
        let new_teams = trueskill_multi_team(
            &teams_and_ranks,
            &TrueSkillConfig {
                draw_probability: 0.05,
                ..Default::default()
            },
        );

        for (agent, new_rating) in result.agents.iter().zip(new_teams) {
            (*self.per_agent.get_mut(&agent.agent).unwrap()).rating =
                new_rating.first().unwrap().clone();
        }
    }
}

impl Display for Database {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        writeln!(f, "=============== SUMMARY ===============")?;
        writeln!(f, "{:<10} {}", "Agent", "Rating")?;

        for agent in self.per_agent.keys() {
            let r = self.per_agent[agent].rating;
            writeln!(
                f,
                "{:<10}: [R {:.2}±{:.2}]",
                agent.name, r.rating, r.uncertainty
            )?;
        }

        Ok(())
    }
}

impl DbAgent {
    pub fn new() -> Self {
        Self {
            rating: TrueSkillRating::new(),
        }
    }
}
