use std::sync::Arc;

use clap::Parser;
use console::style;
use log::info;

use crate::exec::Target;
use crate::game::{GameAgentResult, GameResultData, GameSetup};
use crate::network::client_node::{GameChannel, NetworkArgs};
use crate::referee::Referee;

#[derive(Debug, Parser)]
pub struct PlayArgs {
    /// The referee of the game
    #[arg(short, long, env = "BATTLE_REFEREE")]
    pub referee: String,

    /// List of agents participating in the game
    #[arg(short, long)]
    pub agent: Vec<String>,

    /// Number of games to play
    #[arg(short, long, default_value_t = 1)]
    pub n: usize,

    /// Seed to use. If n > 1, the seed will be incremented each game.
    #[arg(long, default_value_t = 1)]
    pub seed: u64,
}

pub fn format_agent_scores(agents: &[GameAgentResult]) -> String {
    agents
        .iter()
        .enumerate()
        .map(|(i, a)| {
            let s = match i {
                0 => style(a.score).cyan(),
                1 => style(a.score).magenta(),
                2 => style(a.score).yellow(),
                3 => style(a.score).green(),
                _ => style(a.score).white(),
            };
            s.to_string()
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// Runs up to `n` games on the worker pool; collects each [`GameResultData`] in order.
pub async fn play_games(
    referee: Referee,
    agents: Vec<Arc<Target>>,
    n: usize,
    first_seed: u64,
    network_args: NetworkArgs,
    interrupt_on_ctrl_c: bool,
) -> Vec<(GameSetup, GameResultData)> {
    let mut next_game_setup = GameSetup {
        referee,
        agents,
        seed: first_seed,
        capture_io: false,
        capture_game_data: false,
    };

    let mut game_channel = GameChannel::new(network_args);
    let mut results_received = 0;
    let mut in_flight = 0;
    let mut out = Vec::new();

    loop {
        tokio::select! {
            biased; // prioritize receive over send

            _ = tokio::signal::ctrl_c(), if interrupt_on_ctrl_c => {
                info!("Received Ctrl+C, stopping...");
                break;
            }

            item = game_channel.rx.recv() => match item {
                Some((setup, data)) => {
                    results_received += 1;
                    in_flight -= 1;

                    let scores = format_agent_scores(&data.agents);
                    info!("#{} {} - seed {}", results_received, scores, setup.seed);
                    out.push((setup, data));
                    if results_received >= n {
                        break;
                    }
                }
                None => break,
            },

            _ = game_channel.tx.send(next_game_setup.clone()), if in_flight + results_received < n => {
                in_flight += 1;
                next_game_setup.seed += 1;
            },
        }
    }

    out
}
