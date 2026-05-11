mod exec;
mod game;
mod network;
mod referee;

use log::{LevelFilter, info};
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::Arc;
use std::{collections::HashMap, process::ExitCode};

use crate::exec::Executable;
use crate::exec::{BuildError, BuildExecutable};
use crate::exec::{Target, TargetKind};
use crate::game::{GameResultData, GameSetup};
use crate::network::client_node::{GameChannel, NetworkArgs};
use crate::network::worker_node::WorkerNode;
use crate::referee::Referee;
use bundler::{BundlerArgs, BundlerCli, bundle, bundler_main};
use cgsync::{CGSyncCli, cgsync_main};
use clap::{Parser, Subcommand};
use console::{Emoji, style};
use wrapcmd::{WrapCmdCli, wrapcmd_main};

static BUILDING: Emoji<'_, '_> = Emoji("🏗️ ", "");
static BOX: Emoji<'_, '_> = Emoji("📦 ", "");

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

        /// Match seed passed to the referee
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

    /// Start an MCP server
    #[allow(clippy::upper_case_acronyms)]
    MCP {
        /// The protocol to use
        #[arg(short, long)]
        protocol: String,
    },
}

#[derive(Parser, Debug)]
#[command(author, version, about, arg_required_else_help = true)]
struct Args {
    #[command(subcommand)]
    command: Commands,
}

fn bundle_and_build(bundler_args: BundlerArgs) -> Result<Executable, BuildError> {
    info!(
        "{} {}Bundling project... {}",
        style("[1/2]").bold().dim(),
        BOX,
        bundler_args.entry.clone().unwrap().to_string_lossy()
    );

    let bundle = bundle(&bundler_args).expect("correct bundle");

    info!("  OK {} bytes", bundle.source.code.len());

    info!(
        "{} {}Building binary...",
        style("[2/2]").bold().dim(),
        BUILDING
    );

    match bundle.source.build() {
        Ok(executable) => Ok(executable),
        Err(BuildError::MissingCompiler(e)) => {
            eprintln!("Missing compiler: {}", e);
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("Error: {:?}", e);
            std::process::exit(1);
        }
    }
}

#[tokio::main]
async fn main() -> ExitCode {
    env_logger::Builder::new()
        .filter_module("battle", LevelFilter::Trace)
        .init();

    let args = Args::parse();

    match args.command {
        Commands::Bundle { args } => {
            bundler_main(args);
        }
        Commands::Build { bundler_args } => {
            let exec = bundle_and_build(bundler_args).expect("correct bundle and build");
            println!("  OK: {:?}", exec);
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
            agent,
            n,
            seed,
            network_args,
        } => {
            info!("Using a networked worker pool");

            let referee = Referee::from_string(referee);

            let mut next_game_setup = GameSetup {
                referee,
                agents: agent
                    .iter()
                    .map(|path| {
                        let source = bundle(&BundlerArgs::default_from_entry(PathBuf::from(path)))
                            .expect("correct bundle")
                            .source
                            .clone();
                        Arc::new(Target::new(TargetKind::SourceCode(source)))
                    })
                    .collect(),
                seed,
                capture_io: false,
            };

            let mut game_channel = GameChannel::new(network_args);
            let mut results_received = 0;
            let mut in_flight = 0;

            loop {
                tokio::select! {
                    biased; // prioritize receive over send

                    _ = tokio::signal::ctrl_c() => {
                        info!("Received Ctrl+C, stopping...");
                        break;
                    }

                    item = game_channel.rx.recv() => match item {
                        Some((_, data)) => {
                            results_received += 1;
                            in_flight -= 1;

                            let s = |i: usize| data.agents.get(i).map(|a| a.score).unwrap_or(0);
                            println!(
                                "#{} {} {} {}",
                                results_received,
                                style(s(0)).cyan(),
                                style(s(1)).magenta(),
                                style(s(2)).yellow(),
                            );
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

            info!("Exiting!");
        }

        Commands::RefereeDiff {
            reference,
            candidate,
            agent,
            max_games,
            network_args,
        } => {
            let reference = Referee::from_string(reference);
            let candidate = Referee::from_string(candidate);

            let mut game_channel = GameChannel::new(network_args);

            let real_agents: Vec<Arc<Target>> = agent
                .iter()
                .map(|path| {
                    let source = bundle(&BundlerArgs::default_from_entry(PathBuf::from(path)))
                        .expect("correct bundle")
                        .source
                        .clone();
                    Arc::new(Target::new(TargetKind::SourceCode(source)))
                })
                .collect();

            let mut pending_ref: VecDeque<_> = (0..max_games as u64)
                .map(|index| GameSetup {
                    referee: reference.clone(),
                    agents: real_agents.clone(),
                    seed: 1 + index,
                    // we want to capture the transcript to mock the agents for the candidate referee
                    capture_io: true,
                })
                .collect();

            let dummy_setup = GameSetup {
                referee: reference.clone(),
                agents: vec![],
                seed: 0,
                capture_io: false,
            };

            // empty at first, since we need to wait for the reference to be played first
            let mut pending_cand: VecDeque<GameSetup> = VecDeque::new();

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

                            let s = |i: usize| result.agents.get(i).map(|a| a.score).unwrap_or(0);
                            println!(
                                "[{}] {} -> {} {} {}",
                                style(if is_reference { "ref" } else { "cand" }).bold().dim(),
                                style(format!("Seed {}", setup.seed)).white().dim(),
                                style(s(0)).cyan(),
                                style(s(1)).magenta(),
                                style(s(2)).yellow(),
                            );

                            if is_reference {
                                // generate a new game, based on the transcript of the agents
                                let new_game = GameSetup {
                                    referee: candidate.clone(), // candidate instead of reference
                                    agents: result
                                        .agents
                                        .iter()
                                        .map(|a| {
                                            Arc::new(Target::new(TargetKind::Executable(Executable::from_transcript(
                                                            a.transcript.as_ref().unwrap_or(&Default::default()),
                                            ))))
                                        })
                                    .collect(),
                                    seed: setup.seed, // same seed
                                    capture_io: true,
                                };

                                reference_results.insert(setup.seed, result);
                                pending_cand.push_back(new_game);
                            } else {
                                // compare the candidate game with the reference game
                                let reference_result = reference_results.remove(&setup.seed).expect("ref must precede candidate");
                                let candidate_result = result;

                                let scores_match = reference_result.agents.len() == candidate_result.agents.len()
                                    && reference_result.agents.iter().zip(candidate_result.agents.iter())
                                    .all(|(r, c)| r.score == c.score);

                                // TODO: improve
                                if scores_match {
                                    println!("{}", style("MATCH").green().bold());
                                    if pending_ref.is_empty() && pending_cand.is_empty() {
                                        break;
                                    }
                                } else {
                                    println!("{}", style("MISMATCH").red().bold());
                                    break;
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

        Commands::MCP { protocol: _ } => todo!(),
    }

    ExitCode::SUCCESS
}
