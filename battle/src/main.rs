mod builder;
mod exec;
mod game;
mod network;
mod referee;
mod types;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use crate::network::worker_node::run_worker_node;
use crate::referee::Referee;
use crate::types::Target;
use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};
use console::{Emoji, style};
use log::{LevelFilter, info};

use crate::builder::{BuildError, Executable, ExecutableKind};
use crate::game::GameSetup;
use crate::network::producer_node::ProducerNode;

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
        /// List of agents participating in the tournament
        #[arg(short, long)]
        agent: Vec<String>,
        // TODO: seed, N
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
        Commands::Play { agent } => {
            info!("Using a networked worker pool");

            let mut producer = ProducerNode::new().await;

            let game_setup = GameSetup::<Arc<Target>> {
                referee: Referee::from_preset("cg-spring-2024-olympics").unwrap(),
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

            let res = producer.play_game(game_setup).await;
            println!("Game result: {:?}", res);
        }
        Commands::MCP { protocol: _ } => todo!(),
    }
}
