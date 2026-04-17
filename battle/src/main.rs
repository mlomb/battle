mod builder;
mod exec;
mod game;
mod network;
mod referee;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use crate::builder::BuildError;
use crate::exec::executable::Executable;
use crate::exec::target::Target;
use crate::game::{GameResultData, GameSetup};
use crate::network::producer_node::ProducerNode;
use crate::network::worker_node::run_worker_node;
use crate::referee::Referee;
use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};
use console::{Emoji, style};
use futures_util::StreamExt;
use futures_util::stream::FuturesUnordered;
use log::{LevelFilter, info};

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
        #[arg(long, default_value = "0")]
        threads: usize,
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
async fn main() {
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
        Commands::Worker { mut threads } => {
            info!("Starting worker node...");

            if threads == 0 {
                threads = (num_cpus::get_physical() - 2).max(1);
            }

            info!("Using {}", style(format!("{} threads", threads)).cyan());

            run_worker_node().await;

            info!("Exiting!");
        }
        Commands::Play { referee, agent, n } => {
            info!("Using a networked worker pool");

            let producer = Arc::new(ProducerNode::new().await);

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

            let mut futs = FuturesUnordered::new();

            loop {
                tokio::select! {
                    item = futs.next(), if !futs.is_empty() => match item {
                        Some((_, Ok(data))) => {
                            let data: GameResultData = data;
                            let s = |i: usize| data.agents.get(i).map(|a| a.score).unwrap_or(0);
                            println!(
                                "{} {} {}",
                                style(s(0)).cyan(),
                                style(s(1)).magenta(),
                                style(s(2)).yellow(),
                            );
                        }
                        Some((_, Err(e))) => eprintln!("{}", style(e).red()),
                        None => {}
                    },
                    _ = tokio::time::sleep(Duration::from_millis(100)) => {
                        if let Some(fut) = producer.play_game(game_setup.clone()).await {
                            futs.push(fut);
                        }
                    }
                    _ = tokio::signal::ctrl_c() => {
                        info!("Received Ctrl+C, stopping...");
                        break;
                    }
                }
            }

            info!("Exiting!");
        }
        Commands::MCP { protocol: _ } => todo!(),
    }
}
