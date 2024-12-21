use crate::game::{GameAgent, GameResultData};
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

#[derive(Clone)]
pub struct DbAgent {
    total: u32,
    rating: TrueSkillRating,
    rank_count: HashMap<u8, u32>,
}

impl Database {
    pub fn new() -> Self {
        Self {
            games: vec![],
            per_agent: HashMap::new(),
        }
    }

    pub fn receive_result(&mut self, result: &GameResultData) {
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

            let entry = self
                .per_agent
                .entry(agent.agent.clone())
                .or_insert(DbAgent::new());

            entry.total += 1;
            entry
                .rank_count
                .entry(rank as u8)
                .and_modify(|e| *e += 1)
                .or_insert(1);

            // fill team with rating and rank
            teams_and_ranks.push((
                // Free-for-all so a team with only one member
                vec![entry.rating],
                MultiTeamOutcome::new(rank as usize),
            ));
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
        use tabled::builder::Builder;
        use tabled::settings::Style;
        use tabled::settings::{object::Rows, themes::Colorization, Color};

        let mut agent_data = self.per_agent.iter().collect::<Vec<_>>();
        agent_data.sort_by_key(|(_, v)| (-v.rating.rating * 100000 as f64) as i32);

        let mut builder = Builder::default();
        builder.push_record(["Agent", "Rating", "Wins", "Losses", "Total"]);

        for (agent, db_agent) in agent_data.iter() {
            let places = &db_agent.rank_count;
            let first_place = *places.get(&1).unwrap_or(&0);
            let non_first_place = *places.get(&2).unwrap_or(&0)
                + *places.get(&3).unwrap_or(&0)
                + *places.get(&4).unwrap_or(&0);
            let total = db_agent.total;

            assert!(first_place + non_first_place == total);

            builder.push_record([
                agent.name.to_string(),
                format!(
                    "{:.2}±{:.2}",
                    db_agent.rating.rating, db_agent.rating.uncertainty
                ),
                format!(
                    "{} ({:.0}%)",
                    first_place,
                    (first_place as f64) / (total as f64) * 100.0
                ),
                format!(
                    "{} ({:.0}%)",
                    non_first_place,
                    (non_first_place as f64) / (total as f64) * 100.0
                ),
                total.to_string(),
            ]);
        }

        let mut table = builder.build();
        table
            .with(Style::rounded())
            .with(Colorization::columns([
                Color::FG_YELLOW,
                Color::FG_WHITE | Color::BOLD,
                Color::FG_GREEN,
                Color::FG_RED,
                Color::FG_CYAN,
            ]))
            .modify(Rows::first(), Color::FG_WHITE);

        writeln!(f, "{}", table)?;

        Ok(())
    }
}

impl DbAgent {
    pub fn new() -> Self {
        Self {
            rating: TrueSkillRating::new(),
            rank_count: HashMap::new(),
            total: 0,
        }
    }
}
