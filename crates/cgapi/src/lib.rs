pub mod api;

use anyhow::{Context, Result};
use api::{AgentId, CGApiClient, GameId};
use clap::{Parser, Subcommand};
use console::style;
use futures::future::join_all;
use indicatif::{MultiProgress, ProgressBar, ProgressStyle};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use tokio::sync::Semaphore;

/// Fetch CodinGame replays into a local directory.
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
pub struct CGFetchCli {
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
    /// Fetch all recent games played by an agent ID.
    Agent {
        /// Agent ID to fetch games for.
        id: AgentId,
    },
    /// Fetch the top N players from a contest leaderboard, then crawl each agent's games.
    Top {
        /// Number of top-ranked agents to crawl (rank <= N).
        n: usize,

        /// Contest / puzzle ID (matches the URL slug on CodinGame).
        #[arg(long)]
        contest: String,
    },
}

fn agent_bar_style() -> ProgressStyle {
    ProgressStyle::with_template("{prefix:>24} [{bar:40.green/blue}] {pos}/{len}  {msg}")
        .unwrap()
        .progress_chars("=>-")
}

async fn fetch_games_with_progress(
    client: &CGApiClient,
    title: &str,
    game_ids: Vec<GameId>,
    out_dir: &PathBuf,
    game_sem: Arc<Semaphore>,
    mp: &MultiProgress,
) -> (u64, u64) {
    let bar = mp.add(ProgressBar::new(game_ids.len() as u64));
    bar.set_style(agent_bar_style());
    bar.set_prefix(title.to_string());

    let ok = Arc::new(AtomicU64::new(0));
    let failed = Arc::new(AtomicU64::new(0));
    let mut futs = Vec::new();

    for game_id in game_ids {
        let permit = game_sem
            .clone()
            .acquire_owned()
            .await
            .expect("acquiring semaphore");

        let client = client.clone();
        let out_dir = out_dir.clone();
        let bar = bar.clone();
        let mp = mp.clone();
        let failed = failed.clone();
        let ok = ok.clone();

        futs.push(tokio::spawn(async move {
            let _permit = permit;
            let path = out_dir.join(format!("{game_id}.json"));

            if path.exists() {
                // +1 ok
                ok.fetch_add(1, Ordering::Relaxed);
            } else {
                match client.fetch_game(game_id).await {
                    Ok(data) => {
                        let pretty =
                            serde_json::to_vec_pretty(&data).expect("serializing game JSON");
                        tokio::fs::write(&path, &pretty)
                            .await
                            .with_context(|| format!("writing {}", path.display()))
                            .expect("writing game JSON");

                        // +1 ok
                        ok.fetch_add(1, Ordering::Relaxed);
                    }
                    Err(e) => {
                        mp.println(format!("{}", style(format!("{e:?}")).red()))
                            .unwrap();

                        // +1 failed
                        failed.fetch_add(1, Ordering::Relaxed);
                    }
                }
            }

            bar.set_message(format!(
                "{ok} ok, {failed} failed",
                ok = style(ok.load(Ordering::Relaxed)).green(),
                failed = style(failed.load(Ordering::Relaxed)).red()
            ));
            bar.inc(1);
        }));
    }

    join_all(futs).await;

    bar.finish();

    (ok.load(Ordering::Relaxed), failed.load(Ordering::Relaxed))
}

pub async fn cgapi_main(cli: CGFetchCli) -> Result<()> {
    tokio::fs::create_dir_all(&cli.out)
        .await
        .with_context(|| format!("creating output directory {}", cli.out.display()))?;

    let client = CGApiClient::prod()?;
    let game_sem = Arc::new(Semaphore::new(cli.concurrency.max(1)));
    let mp = MultiProgress::new();

    let mut ok = 0;
    let mut failed = 0;

    match cli.command {
        Command::Game { id } => {
            (ok, failed) = fetch_games_with_progress(
                &client,
                &format!("Game {id}"),
                vec![id],
                &cli.out,
                game_sem,
                &mp,
            )
            .await;
        }
        Command::Agent { id } => {
            let game_ids = client.fetch_agent_game_ids(id).await?;
            (ok, failed) = fetch_games_with_progress(
                &client,
                &format!("Agent {id}"),
                game_ids,
                &cli.out,
                game_sem,
                &mp,
            )
            .await;
        }
        Command::Top { n, contest } => {
            let agent_ids = client.fetch_top_agent_ids(&contest).await?;
            let mut futs = Vec::new();

            for (agent_id, pseudo) in agent_ids.into_iter().take(n as usize) {
                let client = client.clone();
                let out_dir = cli.out.clone();
                let game_sem = game_sem.clone();
                let mp = mp.clone();
                futs.push(tokio::spawn(async move {
                    let game_ids = client
                        .fetch_agent_game_ids(agent_id)
                        .await
                        .expect("fetching game ids");

                    fetch_games_with_progress(
                        &client,
                        &format!("{pseudo} [{agent_id}]"),
                        game_ids,
                        &out_dir,
                        game_sem,
                        &mp,
                    )
                    .await
                }));
            }

            for result in join_all(futs).await {
                let (o, f) = result.expect("task panicked");
                ok += o;
                failed += f;
            }
        }
    }

    println!();
    println!();
    println!("{} games downloaded", style(ok).green());
    println!("{} games failed", style(failed).red());

    Ok(())
}
