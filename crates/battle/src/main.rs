mod commands;
mod exec;
mod game;
mod mcp;
mod network;
mod referee;

use log::{LevelFilter, info};
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::Arc;
use std::{collections::HashMap, fs, process::ExitCode};

use crate::commands::play::{self};
use crate::exec::Executable;
use crate::exec::Target;
use crate::game::{GameResultData, GameSetup};
use crate::network::client_node::{GameChannel, NetworkArgs};
use crate::network::worker_node::WorkerNode;
use crate::referee::Referee;
use battle_bundler::{BundlerArgs, BundlerCli, bundler_main};
use battle_cgsync::{CGSyncCli, cgsync_main};
use battle_wrapcmd::{WrapCmdCli, wrapcmd_main};
use clap::{Parser, Subcommand};
use console::style;
use tempfile::tempdir;

#[derive(Subcommand, Debug)]
enum Commands {
    Bundle {
        #[clap(flatten)]
        args: BundlerCli,
    },

    /// Build a project into a binary
    Build {
        #[clap(flatten)]
        bundler_args: BundlerArgs,
        // TODO: output binary
        // TODO: platform, architecture, etc
    },

    #[command(alias = "cgsync")]
    CGSync {
        #[clap(flatten)]
        args: CGSyncCli,
    },

    /// Start a worker node
    Worker {
        /// Number of threads to allocate for the worker.
        /// If set to 0, the default will be the number of physical CPUs minus 2
        #[arg(short, long, default_value_t = 0)]
        threads: usize,

        /// Port number to listen on
        #[arg(short, long, default_value_t = network::DEFAULT_WORKER_PORT)]
        port: u16,
    },

    /// Play a game between multiple bots
    Play {
        /// The referee of the game
        #[arg(short, long, env = "BATTLE_REFEREE")]
        referee: String,

        /// List of agents participating in the game
        #[arg(short, long)]
        agent: Vec<String>,

        /// Number of games to play
        #[arg(short, long, default_value_t = 1)]
        n: usize,

        /// Seed to use. If n > 1, the seed will be incremented each game.
        #[arg(long, default_value_t = 1)]
        seed: u64,

        #[clap(flatten)]
        network_args: NetworkArgs,
    },

    /// Compare two referees by running the same game on both.
    ///
    /// First, it will play a game with the reference referee, then it will fake both
    /// agent's outputs and replicate the same
    ///
    /// Stops at the first mismatch in scores or process status, or after `--max-games` identical runs.
    /// Useful when porting or optimizing a referee (e.g. Java to C++) while preserving outcomes.
    RefereeDiff {
        /// Reference referee (treated as ground truth)
        #[arg(long)]
        reference: String,

        /// Candidate referee under test
        #[arg(long)]
        candidate: String,

        /// Agents to use for the comparison. The agents must not be trivial bots to allow for meaningful comparisons.
        /// A single non-trivial non-deterministic agent works wonders.
        #[arg(short, long)]
        agent: Vec<String>,

        /// First game seed to use
        #[arg(long, default_value_t = 1)]
        seed: u64,

        /// Number of games to compare (seed runs from `0` to `max_games - 1`)
        #[arg(long, default_value_t = 10)]
        max_games: usize,

        #[clap(flatten)]
        network_args: NetworkArgs,
    },

    Wrap {
        #[clap(flatten)]
        args: WrapCmdCli,
    },

    /// Start an MCP server with the stdio transport
    #[allow(clippy::upper_case_acronyms)]
    MCP {
        /// The referee to use throughout this MCP session
        #[arg(long)]
        referee: String,

        #[clap(flatten)]
        network_args: NetworkArgs,
    },
}

#[derive(Parser, Debug)]
#[command(author, version, about, arg_required_else_help = true)]
struct Args {
    #[command(subcommand)]
    command: Commands,
}

#[tokio::main]
async fn main() -> ExitCode {
    env_logger::Builder::new()
        .filter_module("battle", LevelFilter::Info)
        .filter_module("battle::network", LevelFilter::Warn)
        .target(env_logger::Target::Stderr)
        .init();

    let args = Args::parse();

    match args.command {
        Commands::Bundle { args } => {
            bundler_main(args);
        }
        Commands::Build { bundler_args } => {
            commands::code::build_main(bundler_args);
        }
        Commands::CGSync { args } => {
            cgsync_main(args).await;
        }
        Commands::Worker { mut threads, port } => {
            info!("Starting worker node...");

            if threads == 0 {
                threads = (num_cpus::get_physical() - 2).max(1);
            }

            info!("Worker listening on port {}", style(port).yellow());
            info!("Using {}", style(format!("{} threads", threads)).cyan());

            let (node, handler) = WorkerNode::new(threads, port);
            let join = std::thread::spawn(move || {
                node.run();
            });

            let _ = tokio::signal::ctrl_c().await;
            info!("Received interrupt, shutting down worker...");

            handler.stop();
            join.join().expect("worker thread panicked");

            info!("Exiting!");
        }
        Commands::Play {
            referee,
            agent: agents,
            n,
            seed,
            network_args,
        } => {
            let referee = Referee::from_string(&referee);
            let agents = agents
                .iter()
                .map(|path| {
                    Arc::new(Target::from_entrypoint(PathBuf::from(path)).expect("correct bundle"))
                })
                .collect();

            info!("Using a networked worker pool");
            play::play_games(referee, agents, n, seed, network_args, true).await;
            info!("Exiting!");
        }

        Commands::RefereeDiff {
            reference,
            candidate,
            agent,
            seed,
            max_games,
            network_args,
        } => {
            let reference = Referee::from_string(&reference);
            let candidate = Referee::from_string(&candidate);

            let mut game_channel = GameChannel::new(network_args);

            let real_agents: Vec<Arc<Target>> = agent
                .iter()
                .map(|path| {
                    Arc::new(Target::from_entrypoint(PathBuf::from(path)).expect("correct bundle"))
                })
                .collect();

            let mut pending_ref: VecDeque<_> = (0..max_games as u64)
                .map(|index| GameSetup {
                    referee: reference.clone(),
                    agents: real_agents.clone(),
                    seed: seed + index,
                    // we want to capture the transcript to mock the agents for the candidate referee
                    capture_io: true,
                    capture_game_data: true,
                })
                .collect();

            // empty at first, since we need to wait for the reference to be played first
            let mut pending_cand: VecDeque<GameSetup> = VecDeque::new();

            // not actually used, but needed to satisfy the type checker
            let dummy_setup = GameSetup {
                referee: reference.clone(),
                agents: vec![],
                seed: 0,
                capture_io: false,
                capture_game_data: false,
            };

            // reference results by seed
            let mut reference_results: HashMap<u64, GameResultData> = HashMap::new();

            loop {
                tokio::select! {

                    // prioritize receive over send, then
                    // prioritize sending candidate games over reference games to bail fast
                    biased;

                    _ = tokio::signal::ctrl_c() => {
                        info!("Received Ctrl+C, stopping...");
                        break;
                    }

                    item = game_channel.rx.recv() => match item {
                        Some((setup, result)) => {
                            let is_reference = Arc::ptr_eq(&setup.referee.target, &reference.target);

                            let scores = play::format_agent_scores(&result.agents);

                            if is_reference {
                                info!(
                                    "[{}] {} -> {}",
                                    style("ref").bold().dim(),
                                    style(format!("Seed {}", setup.seed)).white().dim(),
                                    scores,
                                );

                                // generate a new game, based on the transcript of the agents
                                let new_game = GameSetup {
                                    referee: candidate.clone(), // candidate instead of reference
                                    agents: result
                                        .agents
                                        .iter()
                                        .map(|a| {
                                            Arc::new(Target::from_executable(Executable::from_transcript(
                                                            a.transcript.as_ref().unwrap_or(&Default::default()),
                                            )))
                                        })
                                    .collect(),
                                    seed: setup.seed, // very important, same seed
                                    capture_io: true,
                                    capture_game_data: true,
                                };

                                reference_results.insert(setup.seed, result);
                                pending_cand.push_back(new_game);
                            } else {
                                // compare the candidate game with the reference game
                                let reference_result = reference_results.remove(&setup.seed).expect("ref must precede candidate");
                                let candidate_result = result;

                                let exit_code_match = reference_result.exec_res.status == candidate_result.exec_res.status;

                                let scores_match = reference_result.agents.len() == candidate_result.agents.len()
                                    && reference_result.agents.iter().zip(candidate_result.agents.iter())
                                    .all(|(r, c)| r.score == c.score);

                                let transcripts_match = reference_result.agents.len() == candidate_result.agents.len()
                                    && reference_result.agents.iter().zip(candidate_result.agents.iter())
                                    .all(|(r, c)| r.transcript == c.transcript);

                                let game_data_match = true;// || reference_result.game_data == candidate_result.game_data;

                                if scores_match && exit_code_match && transcripts_match && game_data_match {
                                    info!(
                                        "[{}] {} -> {} {}",
                                        style("cand").bold().dim(),
                                        style(format!("Seed {}", setup.seed)).white().dim(),
                                        scores,
                                        style("MATCH").green().bold(),
                                    );

                                    if pending_ref.is_empty() && pending_cand.is_empty() {
                                        break;
                                    }
                                } else {
                                    println!();
                                    println!("{} (reference vs candidate)", style("MISMATCH DETECTED").red().bold());
                                    println!("Seed: {}", style(setup.seed).yellow());

                                    println!("Exit status: {:?} vs {:?}", style(reference_result.exec_res.status).blue(), style(candidate_result.exec_res.status).blue());
                                    println!(
                                        "Scores: {} vs {}",
                                        play::format_agent_scores(&reference_result.agents),
                                        play::format_agent_scores(&candidate_result.agents),
                                    );

                                    // TODO: improve the following outputs to be more readable
                                    // while avoiding massive prints

                                    for (i, (r, c)) in reference_result
                                        .agents
                                        .iter()
                                        .zip(candidate_result.agents.iter())
                                        .enumerate()
                                    {
                                        println!(
                                            "Transcript agent {i}: {} ({} vs {} events)",
                                            if r.transcript != c.transcript {
                                                style("MISMATCH").red().bold()
                                            } else {
                                                style("MATCH").green().bold()
                                            },
                                            r.transcript.as_ref().map(|t| t.events.len()).unwrap_or_default(),
                                            c.transcript.as_ref().map(|t| t.events.len()).unwrap_or_default(),
                                        );
                                    }

                                    println!("Game data: {} ({} vs {} bytes)",
                                        if reference_result.game_data != candidate_result.game_data {
                                            style("MISMATCH").red().bold()
                                        } else {
                                            style("MATCH").green().bold()
                                        },
                                        reference_result.game_data.as_ref().map(|d| d.len()).unwrap_or_default(),
                                        candidate_result.game_data.as_ref().map(|d| d.len()).unwrap_or_default(),
                                    );

                                    println!();
                                    println!("Files for further inspection:");

                                    // `keep` relinquishes automatic cleanup so artifacts survive `exit`.
                                    let dir = tempdir().expect("tempdir").keep();

                                    for (i, (r, c)) in reference_result
                                        .agents
                                        .iter()
                                        .zip(candidate_result.agents.iter())
                                        .enumerate()
                                    {
                                        let ref_path = dir.join(format!("reference_agent_{i}.io"));
                                        let cand_path = dir.join(format!("candidate_agent_{i}.io"));
                                        if let Some(ref_transcript) = &r.transcript {
                                            ref_transcript.save(&ref_path).expect("write reference transcript");
                                        }
                                        if let Some(cand_transcript) = &c.transcript {
                                            cand_transcript.save(&cand_path).expect("write candidate transcript");
                                        }
                                    }

                                    fs::write(
                                        dir.join("reference_game.data"),
                                        reference_result.game_data.as_deref().unwrap_or(""),
                                    )
                                    .expect("write reference game data");
                                    fs::write(
                                        dir.join("candidate_game.data"),
                                        candidate_result.game_data.as_deref().unwrap_or(""),
                                    )
                                    .expect("write candidate game data");

                                    let dir_display = dir.canonicalize().unwrap_or(dir);
                                    println!("{}", style(dir_display.display()).bold().magenta());
                                    println!();

                                    println!("Use '--seed {} --max-games 1' to replay the game.", style(setup.seed).yellow());
                                    println!();

                                    std::process::exit(1);
                                }
                            }
                        }
                        None => break,
                    },

                    _ = game_channel.tx.send(pending_cand.front().cloned().unwrap_or(dummy_setup.clone())), if !pending_cand.is_empty() => {
                        pending_cand.pop_front().unwrap();
                    },

                    _ = game_channel.tx.send(pending_ref.front().cloned().unwrap_or(dummy_setup.clone())), if !pending_ref.is_empty() && pending_cand.is_empty() => {
                        pending_ref.pop_front().unwrap();
                    },

                    // _ = tokio::time::sleep(Duration::from_millis(100)) => {
                    // },
                }
            }

            info!("Exiting!");
        }

        Commands::Wrap { args } => {
            return wrapcmd_main(args);
        }

        Commands::MCP {
            referee,
            network_args,
        } => {
            let referee = Referee::from_string(&referee);
            if let Err(e) = mcp::mcp_main(referee, network_args).await {
                eprintln!("MCP server error: {e}");
                return ExitCode::FAILURE;
            }
        }
    }

    ExitCode::SUCCESS
}
