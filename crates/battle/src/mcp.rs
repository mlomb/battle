use rmcp::{
    ServiceExt, handler::server::wrapper::Parameters, schemars, tool, tool_router, transport::stdio,
};
use schemars::JsonSchema;
use serde::Deserialize;
use std::path::PathBuf;
use std::sync::Arc;
use tempfile::TempDir;

use crate::commands::code::bundle_and_build;
use crate::commands::play::{self};
use crate::exec::Target;
use crate::network::client_node::NetworkArgs;
use crate::referee::Referee;
use battle_bundler::BundlerArgs;

#[derive(Debug, Deserialize, JsonSchema)]
struct BuildMcp {
    /// Entry point file (main.cpp, Cargo.toml) or directory containing an entry file
    entry: PathBuf,
}

#[derive(Debug, Deserialize, JsonSchema)]
struct PlayMcp {
    /// Agent entrypoint paths (main.cpp, Cargo.toml) or directory containing an entry file
    agents: Vec<PathBuf>,

    /// Number of games to play
    #[serde(default = "default_play_games")]
    n: usize,

    /// Seed to use. If n > 1, the seed will be incremented each game
    #[serde(default = "default_seed")]
    seed: u64,

    /// Optionally stores each agent's transcript (stdin, stdout, stderr) for analysis
    #[serde(default = "default_capture_io")]
    capture_io: bool,
    // About other possible params:
    // "referee": using MCP, this must be passed as an argument to mcp
    // "seed": we could also allow seed here but I prefer to give less params to the LLM
}

fn default_play_games() -> usize {
    1000
}

fn default_seed() -> u64 {
    1
}

fn default_capture_io() -> bool {
    false
}

struct BattleMcpServer {
    referee: Referee,
    network_args: NetworkArgs,
    temp_dir: TempDir,
}

#[tool_router(server_handler)]
impl BattleMcpServer {
    #[tool(name = "build", description = "Bundle and build a bot project")]
    async fn build_bot(&self, Parameters(bundler_mcp): Parameters<BuildMcp>) -> String {
        let result = bundle_and_build(BundlerArgs {
            entry: bundler_mcp.entry,
        });
        match result {
            Ok(exec) => format!("Ok: {:?}", exec),
            Err(e) => format!("Error: {:?}", e),
        }
    }

    #[tool(
        name = "play",
        description = "Run N games between agents on sequential seeds"
    )]
    async fn play(&self, Parameters(play_mcp): Parameters<PlayMcp>) -> String {
        let num_agents = play_mcp.agents.len();
        let results = play::play_games(
            self.referee.clone(),
            play_mcp
                .agents
                .iter()
                .map(|p| Arc::new(Target::from_entrypoint(p.clone()).expect("correct bundle")))
                .collect(),
            play_mcp.n,
            play_mcp.seed,
            self.network_args.clone(),
            false,
            play_mcp.capture_io,
        )
        .await;

        const NOTABLE_COUNT: usize = 3;

        #[derive(serde::Serialize)]
        struct NotableGame {
            seed: u64,
            scores: Vec<i32>,
        }

        #[derive(serde::Serialize)]
        struct AgentSummary {
            wins: usize,
            draws: usize,
            losses: usize,
            avg_score: f64,
            min_score: i32,
            max_score: i32,
        }

        #[derive(serde::Serialize)]
        struct PlayResult {
            games: usize,
            agents: Vec<AgentSummary>,
            notable_games: serde_json::Value,

            #[serde(skip_serializing_if = "Option::is_none")]
            transcripts_dir: Option<String>,
            #[serde(skip_serializing_if = "Option::is_none")]
            transcripts_access: Option<&'static str>,
            #[serde(skip_serializing_if = "Option::is_none")]
            all_games: Option<Vec<NotableGame>>,
        }

        struct AgentAccum {
            wins: usize,
            draws: usize,
            losses: usize,
            score_sum: i64,
            // (agent_score, seed, all_scores) — sorted ascending after the loop
            games: Vec<(i32, u64, Vec<i32>)>,
        }

        let mut accum: Vec<AgentAccum> = (0..num_agents)
            .map(|_| AgentAccum {
                wins: 0,
                draws: 0,
                losses: 0,
                score_sum: 0,
                games: vec![],
            })
            .collect();
        let mut draw_games: Vec<NotableGame> = vec![];
        let mut all_games: Vec<NotableGame> = vec![];

        for (setup, data) in &results {
            let scores: Vec<i32> = data.agents.iter().map(|a| a.score).collect();
            let max_score = scores.iter().copied().max().unwrap_or(0);
            let is_draw = scores.iter().filter(|&&s| s == max_score).count() > 1;

            if is_draw {
                draw_games.push(NotableGame {
                    seed: setup.seed,
                    scores: scores.clone(),
                });
            }

            if play_mcp.capture_io {
                for (i, a) in data.agents.iter().enumerate() {
                    let path = self
                        .temp_dir
                        .path()
                        .join(format!("seed{}_agent{}.io", setup.seed, i));
                    let transcript = a.transcript.clone().unwrap_or_default();
                    transcript.save(&path).unwrap_or_default();
                }
            }

            all_games.push(NotableGame {
                seed: setup.seed,
                scores: scores.clone(),
            });

            for (i, &score) in scores.iter().enumerate() {
                let a = &mut accum[i];
                a.score_sum += score as i64;
                a.games.push((score, setup.seed, scores.clone()));
                match (is_draw, score == max_score) {
                    (true, _) => a.draws += 1,
                    (false, true) => a.wins += 1,
                    _ => a.losses += 1,
                }
            }
        }

        let games = results.len();

        let to_notable = |slice: &[(i32, u64, Vec<i32>)]| -> Vec<NotableGame> {
            slice
                .iter()
                .take(NOTABLE_COUNT)
                .map(|(_, seed, scores)| NotableGame {
                    seed: *seed,
                    scores: scores.clone(),
                })
                .collect()
        };

        let mut notable_games = serde_json::json!({
            "draws": draw_games.into_iter().take(NOTABLE_COUNT).collect::<Vec<_>>()
        });
        let agents: Vec<AgentSummary> = accum
            .iter_mut()
            .enumerate()
            .map(|(i, a)| {
                a.games.sort_unstable_by_key(|g| g.0);
                let min_score = a.games.first().map_or(0, |g| g.0);
                let max_score = a.games.last().map_or(0, |g| g.0);
                notable_games[format!("agent_{i}_best")] = serde_json::json!(to_notable(
                    a.games.iter().rev().cloned().collect::<Vec<_>>().as_slice()
                ));
                notable_games[format!("agent_{i}_worst")] = serde_json::json!(to_notable(&a.games));
                AgentSummary {
                    wins: a.wins,
                    draws: a.draws,
                    losses: a.losses,
                    avg_score: if games > 0 {
                        a.score_sum as f64 / games as f64
                    } else {
                        0.0
                    },
                    min_score,
                    max_score,
                }
            })
            .collect();

        let transcripts_dir = play_mcp
            .capture_io
            .then(|| self.temp_dir.path().to_string_lossy().into_owned());

        let transcripts_access = play_mcp.capture_io.then_some(
            "Agent transcripts are at {transcripts_dir}/seed{seed}_agent{agent_index}.io (agent_index is 0-based, same order as the agents argument).",
        );

        let result = PlayResult {
            games,
            agents,
            notable_games,
            transcripts_dir,
            transcripts_access,
            all_games: if play_mcp.capture_io {
                Some(all_games)
            } else {
                None
            },
        };

        serde_json::to_string(&result).expect("json ok")
    }
}

pub async fn mcp_main(referee: Referee, network_args: NetworkArgs) -> anyhow::Result<()> {
    let server = BattleMcpServer {
        referee,
        network_args,
        temp_dir: tempfile::tempdir()?,
    }
    .serve(stdio())
    .await?;
    server.waiting().await?;
    Ok(())
}
