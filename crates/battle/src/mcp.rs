use rmcp::{
    ServiceExt, handler::server::wrapper::Parameters, schemars, tool, tool_router, transport::stdio,
};
use schemars::JsonSchema;
use serde::Deserialize;
use std::path::PathBuf;
use std::sync::Arc;

use crate::commands::code::bundle_and_build;
use crate::commands::play::{self};
use crate::exec::Target;
use crate::network::client_node::NetworkArgs;
use crate::referee::Referee;
use battle_bundler::BundlerArgs;

#[derive(Debug, Deserialize, JsonSchema)]
pub struct BuildMcp {
    /// Entry point file (main.cpp, Cargo.toml) or directory containing an entry file
    pub entry: PathBuf,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct PlayMcp {
    /// Agent entrypoint paths (main.cpp, Cargo.toml) or directory containing an entry file.
    pub agents: Vec<PathBuf>,

    /// Number of games to play
    #[serde(default = "default_play_games")]
    pub n: usize,
    // About other possible params:
    // "referee": using MCP, this must be passed as an argument to mcp
    // "seed": we could also allow seed here but I prefer to give less params to the LLM
}

fn default_play_games() -> usize {
    100
}

#[derive(Debug, Deserialize, JsonSchema)]
#[allow(dead_code)]
pub struct InspectMcp {
    /// Agent entrypoint paths (main.cpp, Cargo.toml) or directory containing an entry file.
    pub agents: Vec<PathBuf>,

    /// Seed to use
    pub seed: u64,
}

#[derive(Clone)]
pub struct BattleServer {
    referee: Referee,
    network_args: NetworkArgs,
}

#[tool_router(server_handler)]
impl BattleServer {
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
        )
        .await;
        let scores: Vec<Vec<i32>> = results
            .iter()
            .map(|g| g.agents.iter().map(|a| a.score).collect())
            .collect();

        serde_json::to_string_pretty(&scores).unwrap_or_else(|e| format!("json error: {e}"))
    }

    #[tool(
        name = "inspect",
        description = "Runs a single game between agents on a given seed, extracts each agent's transcript (stdin, stdout, stderr) for analysis"
    )]
    async fn inspect(&self, Parameters(_inspect_mcp): Parameters<InspectMcp>) -> String {
        unimplemented!()
    }
}

pub async fn mcp_main(referee: Referee, network_args: NetworkArgs) -> anyhow::Result<()> {
    let server = BattleServer {
        referee,
        network_args,
    }
    .serve(stdio())
    .await?;
    server.waiting().await?;
    Ok(())
}
