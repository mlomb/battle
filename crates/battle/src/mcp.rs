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
    /// Agent entrypoint paths (main.cpp, Cargo.toml) or directory containing an entry file.
    agents: Vec<PathBuf>,

    /// Number of games to play
    #[serde(default = "default_play_games")]
    n: usize,
    // About other possible params:
    // "referee": using MCP, this must be passed as an argument to mcp
    // "seed": we could also allow seed here but I prefer to give less params to the LLM
}

fn default_play_games() -> usize {
    1000
}

#[derive(Debug, Deserialize, JsonSchema)]
#[allow(dead_code)]
struct InspectMcp {
    /// Agent entrypoint paths (main.cpp, Cargo.toml) or directory containing an entry file.
    agents: Vec<PathBuf>,

    /// Seed to use
    seed: u64,
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
            entry: Some(bundler_mcp.entry),
        });
        match result {
            Ok(exec) => format!("OK: {:?}", exec),
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
            1,
            self.network_args.clone(),
            false,
            false,
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

        let result = PlayResult {
            games,
            agents,
            notable_games,
        };

        serde_json::to_string(&result).expect("json ok")
    }

    #[tool(
        name = "inspect",
        description = "Runs a single game between agents on a given seed, extracts each agent's transcript (stdin, stdout, stderr) for analysis"
    )]
    async fn inspect(&self, Parameters(inspect_mcp): Parameters<InspectMcp>) -> String {
        let mut results = play::play_games(
            self.referee.clone(),
            inspect_mcp
                .agents
                .iter()
                .map(|p| Arc::new(Target::from_entrypoint(p.clone()).expect("correct bundle")))
                .collect(),
            1,
            inspect_mcp.seed,
            self.network_args.clone(),
            false,
            true,
        )
        .await;

        let Some((setup, data)) = results.pop() else {
            return "Error: game did not complete".to_string();
        };

        #[derive(serde::Serialize)]
        struct AgentInspect {
            score: i32,
            transcript_path: String,
            transcript_bytes: u64,
        }

        #[derive(serde::Serialize)]
        struct InspectResult {
            seed: u64,
            agents: Vec<AgentInspect>,
        }

        let agents = data
            .agents
            .into_iter()
            .enumerate()
            .map(|(i, a)| {
                let path = self
                    .temp_dir
                    .path()
                    .join(format!("seed{}_agent{}.io", setup.seed, i));

                let transcript = a.transcript.unwrap_or_default();
                transcript.save(&path).unwrap_or_default();

                let transcript_bytes = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
                AgentInspect {
                    score: a.score,
                    transcript_path: path.to_string_lossy().into_owned(),
                    transcript_bytes,
                }
            })
            .collect();

        let result = InspectResult {
            seed: setup.seed,
            agents,
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
