use std::path::PathBuf;
use std::sync::Arc;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use console::style;
use futures::stream::{FuturesUnordered, StreamExt};
use indicatif::{MultiProgress, ProgressBar, ProgressStyle};
use tokio::sync::Semaphore;

use cgapi::api::{AgentId, CGApiClient, GameId};
use cgapi::crawl_game;

const DEFAULT_CONTEST: &str = "green-circle";

/// Fetch CodinGame replays / leaderboards into a local directory.
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Cli {
    /// Directory to store fetched game JSON files.
    #[arg(long, global = true, default_value = "games")]
    out: PathBuf,

    /// Max concurrent game downloads (global, across all agents).
    #[arg(long, global = true, default_value_t = 4)]
    concurrency: usize,

    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Fetch a single game by its game ID.
    Game {
        /// Game ID to fetch.
        id: GameId,
    },
    /// Fetch all recent battles played by an agent ID.
    Agent {
        /// Agent ID to fetch battles for.
        id: AgentId,
    },
    /// Fetch the top N players from a contest leaderboard, then crawl each agent's battles.
    Top {
        /// Number of top-ranked agents to crawl (rank <= N).
        n: u64,

        /// Contest / puzzle ID (matches the URL slug on CodinGame).
        #[arg(long, default_value = DEFAULT_CONTEST)]
        contest: String,
    },
}

fn agent_bar_style() -> ProgressStyle {
    ProgressStyle::with_template("{prefix:>14} [{bar:40.green/blue}] {pos}/{len}  {msg}")
        .unwrap()
        .progress_chars("=>-")
}

/// Fans out `crawl_game` calls for a list of game IDs with a progress bar and shared semaphore.
async fn crawl_games_with_progress(
    client: &CGApiClient,
    agent_id: u64,
    game_ids: Vec<u64>,
    out_dir: &PathBuf,
    game_sem: Arc<Semaphore>,
    mp: &MultiProgress,
) -> Result<()> {
    let bar = mp.add(ProgressBar::new(game_ids.len() as u64));
    bar.set_style(agent_bar_style());
    bar.set_prefix(format!("agent {agent_id}"));

    let mut failed: u64 = 0;
    let mut futs = FuturesUnordered::new();

    for game_id in game_ids {
        let permit = game_sem.clone().acquire_owned().await?;
        let client = client.clone();
        let out_dir = out_dir.clone();
        let bar = bar.clone();
        futs.push(tokio::spawn(async move {
            let _permit = permit;
            let res = crawl_game(&client, game_id, &out_dir).await;
            bar.inc(1);
            res
        }));
    }

    while let Some(res) = futs.next().await {
        if res?.is_err() {
            failed += 1;
        }
        let msg = if failed > 0 {
            format!("{}", style(format!("{failed} failed")).red())
        } else {
            String::new()
        };
        bar.set_message(msg);
    }

    bar.finish();
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    tokio::fs::create_dir_all(&cli.out)
        .await
        .with_context(|| format!("creating output directory {}", cli.out.display()))?;

    let client = CGApiClient::new()?;
    let game_sem = Arc::new(Semaphore::new(cli.concurrency.max(1)));
    let mp = MultiProgress::new();

    match cli.command {
        Command::Game { id } => {
            crawl_game(&client, id, &cli.out).await?;
        }
        Command::Agent { id } => {
            let game_ids = client.fetch_agent_game_ids(id).await?;
            crawl_games_with_progress(&client, id, game_ids, &cli.out, game_sem, &mp).await?;
        }
        Command::Top { n, contest } => {
            let agent_ids = client.fetch_top_agent_ids(&contest).await?;
            let agent_ids: Vec<_> = agent_ids.into_iter().take(n as usize).collect();
            let mut futs = FuturesUnordered::new();

            for agent_id in agent_ids {
                let client = client.clone();
                let out_dir = cli.out.clone();
                let game_sem = game_sem.clone();
                let mp = mp.clone();
                futs.push(tokio::spawn(async move {
                    let game_ids = client.fetch_agent_game_ids(agent_id).await?;
                    crawl_games_with_progress(&client, agent_id, game_ids, &out_dir, game_sem, &mp)
                        .await
                }));
            }

            while let Some(res) = futs.next().await {
                let _ = res?;
            }
        }
    }

    Ok(())
}
