mod builder;

use std::{collections::HashMap, path::PathBuf, time::Duration};

use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};
use console::{Emoji, style};
use distributed_channel::{NodeSetup, start_consumer_node};
use log::{LevelFilter, error, info};
use serde::{Deserialize, Serialize};

use crate::builder::BuildError;

struct Target {
    command: String,
    assets: HashMap<String, Vec<u8>>,
}

struct Referee {
    // protocol
    exec: Target,
    // min, max
}

static LOOKING_GLASS: Emoji<'_, '_> = Emoji("🔍  ", "");
static TRUCK: Emoji<'_, '_> = Emoji("🚚  ", "");
static CLIP: Emoji<'_, '_> = Emoji("🔗  ", "");
static PAPER: Emoji<'_, '_> = Emoji("📃  ", "");
static SPARKLE: Emoji<'_, '_> = Emoji("✨ ", ":-)");
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

    /// Start a worker node to listen for games on the local P2P network
    Worker {
        /// Number of threads to allocate for the worker.
        /// If set to 0, the default will be the number of physical CPUs minus 2
        #[arg(long, default_value = "0")]
        threads: usize,

        /// Interface to listen on
        /// By default, it will target all interfaces
        /// TODO: add support for this
        #[arg(long, default_value = "0.0.0.0")]
        interface: String,
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

#[derive(Debug, Clone, Serialize, Deserialize)]
struct GameEnv {}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct GameSetup {
    agent: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct GameResult {
    result: Result<String, String>,
}

fn main() {
    env_logger::Builder::new()
        .filter_module("distributed_channel", LevelFilter::Trace)
        .filter_module("battle", LevelFilter::Trace)
        .init();

    let args = Args::parse();

    let mut setup = NodeSetup::default();
    setup.protocol = "/mlomb/bot-tools/battle/1".to_string();

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
            info!("{} {}Bundling project...", style("[1/2]").bold().dim(), BOX);

            let bundle = bundle(&bundler_args).expect("correct bundle");

            info!("  OK {} bytes", bundle.source.code.len());

            info!(
                "{} {}Building binary...",
                style("[2/2]").bold().dim(),
                BUILDING
            );

            match builder::build_cpp(&bundle.source.code, HashMap::new()) {
                Ok(executable) => println!("  OK: {:?}", executable),
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
        Commands::Worker {
            mut threads,
            interface,
        } => {
            info!("Starting worker node...");

            if threads == 0 {
                threads = (num_cpus::get_physical() - 2).max(1);
            }

            info!("Using {}", style(format!("{} threads", threads)).cyan());

            start_consumer_node::<GameEnv, GameSetup, GameResult>(
                setup,
                threads,
                |env, game_setup| {
                    info!("Game setup: {:?}", game_setup);
                    GameResult {
                        result: Ok(format!("game result for {}", game_setup.agent)),
                    }
                },
            );
        }
        Commands::Play { agent } => {
            info!("Using a networked worker pool");

            let (node, input_tx, output_rx) =
                setup.into_producer::<GameEnv, GameSetup, GameResult>(GameEnv {});

            let mut i = 0;

            loop {
                let game_setup = GameSetup {
                    agent: format!("pepe{}", i),
                };
                i += 1;

                let result = crossbeam_channel::select! {
                    recv(output_rx) -> res => res.ok(),
                    send(input_tx, game_setup.clone()) -> _ => None,
                };
                match result {
                    Some(result) => {
                        info!("Result: {:?}", result);
                    }
                    None => {
                        // sent!
                    }
                }

                std::thread::sleep(Duration::from_millis(10));
            }
        }
        Commands::MCP { protocol } => todo!(),
    }
}
