mod builder;
mod exec;
mod game;
mod network;
mod referee;

use std::path::PathBuf;
use std::sync::Arc;
use std::{collections::HashMap, process::ExitCode};

use crate::builder::BuildError;
use crate::exec::executable::Executable;
use crate::exec::target::Target;
use crate::game::GameSetup;
use crate::network::game_stream::GameStream;
use crate::network::worker_node::run_worker_node;
use crate::referee::Referee;
use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};
use console::{Emoji, style};
use futures_util::{StreamExt, stream};
use log::{LevelFilter, info};
use wrapcmd::{WrapCmdCommand, wrap_main};

static BUILDING: Emoji<'_, '_> = Emoji("🏗️ ", "");
static BOX: Emoji<'_, '_> = Emoji("📦 ", "");

#[derive(Subcommand, Debug)]
enum Commands {
    /// Converts a C++/Rust project directory into a single source file
    Bundle {
        #[clap(flatten)]
        bundler_args: BundlerArgs,

        /// Output target file.
        /// If not provided, the output will be printed to stdout.
        #[arg(long)]
        output: Option<String>,
    },

    /// Build a project into a binary
    Build {
        #[clap(flatten)]
        bundler_args: BundlerArgs,
        // TODO: output binary
        // TODO: platform, architecture, etc
    },

    /// Start a worker node listening on localhost:54321
    Worker {
        /// Number of threads to allocate for the worker.
        /// If set to 0, the default will be the number of physical CPUs minus 2
        #[arg(short, long, default_value = "0")]
        threads: usize,

        /// Port number to listen on.
        ///
        /// If not provided, a random port will be chosen (likely what you want for P2P)
        #[arg(short, long)]
        port: Option<u16>,
    },

    /// Play a game between multiple bots
    Play {
        /// The referee of the game
        #[arg(short, long)]
        referee: String,

        /// List of agents participating in the game
        #[arg(short, long)]
        agent: Vec<String>,

        /// Number of games to play
        #[arg(short, long, default_value = "1")]
        n: usize,
        // TODO: seed
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
        #[arg(long, default_value = "10")]
        max_games: usize,
    },

    /// Wraps an executable to record/replay stdin, stdout, and stderr.
    Wrap {
        #[command(subcommand)]
        command: WrapCmdCommand,
    },

    /// Start an MCP server
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

    /// The environment file to use
    #[arg(short, long, default_value = "env.yaml")]
    env: PathBuf,
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

    match builder::build_cpp(&bundle.source.code, HashMap::new()) {
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
        Commands::Bundle {
            bundler_args,
            output,
        } => match bundle(&bundler_args) {
            Ok(bundle) => {
                if let Some(output) = output {
                    std::fs::write(output, bundle.source.code).expect("a writeable output file");
                } else {
                    info!("{}", bundle.source.code);
                }
            }
            Err(err) => {
                eprintln!("Error: {}", err);
                std::process::exit(1);
            }
        },
        Commands::Build { bundler_args } => {
            let exec = bundle_and_build(bundler_args).expect("correct bundle and build");
            println!("  OK: {:?}", exec);
        }
        Commands::Worker { mut threads, port } => {
            info!("Starting worker node...");

            if threads == 0 {
                threads = (num_cpus::get_physical() - 2).max(1);
            }

            run_worker_node(threads, port).await;

            info!("Exiting!");
        }
        Commands::Play { referee, agent, n } => {
            info!("Using a networked worker pool");

            let game_setup = GameSetup::<Arc<Target>> {
                referee: Referee::from_preset(referee).unwrap(),
                agents: agent
                    .iter()
                    .map(|path| {
                        let source = bundle(&BundlerArgs::default_from_entry(PathBuf::from(path)))
                            .expect("correct bundle")
                            .source
                            .clone();
                        Arc::new(Target::SourceCode(source))
                    })
                    .collect(),
                seed: 0,
            };

            let mut game_stream = GameStream::new(stream::repeat(game_setup)).await;

            loop {
                tokio::select! {
                    item = game_stream.next() => match item {
                        Some((_, data)) => {
                            let s = |i: usize| data.agents.get(i).map(|a| a.score).unwrap_or(0);
                            println!(
                                "{} {} {}",
                                style(s(0)).cyan(),
                                style(s(1)).magenta(),
                                style(s(2)).yellow(),
                            );
                        }
                        None => break,
                    },
                    _ = tokio::signal::ctrl_c() => {
                        info!("Received Ctrl+C, stopping...");
                        break;
                    }
                }
            }

            info!("Exiting!");
        }
        Commands::MCP { protocol: _ } => todo!(),

        Commands::RefereeDiff {
            reference,
            candidate,
            agent,
            max_games,
        } => {
            /*
            if let Err(e) = crate::referee_diff::run(reference, candidate, agent, max_games).await {
                eprintln!("RefereeDiff failed: {}", e);
                std::process::exit(1);
            }
            */
        }
        Commands::Wrap { command } => {
            return wrap_main(command);
        }
    }

    ExitCode::SUCCESS
}
