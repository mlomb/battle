mod builder;
mod network;
mod types;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};
use console::{Emoji, style};
use log::{LevelFilter, info};
use network::{ProducerHandle, TargetId, WorkerNode};

use crate::builder::{BuildError, Executable, ExecutableKind};
use crate::types::{GameResult, GameSetup, Target};

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

fn main() {
    env_logger::Builder::new()
        .filter_module("battle", LevelFilter::Trace)
        .init();

    let args = Args::parse();

    let protocol = "/mlomb/bot-tools/battle/1";

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
        Commands::Worker {
            mut threads,
            interface: _,
        } => {
            info!("Starting worker node...");

            if threads == 0 {
                threads = (num_cpus::get_physical() - 2).max(1);
            }

            info!("Using {}", style(format!("{} threads", threads)).cyan());

            WorkerNode::new(protocol, threads).wait();
        }
        Commands::Play { agent } => {
            info!("Using a networked worker pool");

            let producer = ProducerHandle::<Target, GameSetup, GameResult>::new(protocol);

            // Register referee target
            let referee_target = Target::Executable(Executable {
                kind: ExecutableKind::Jar {
                    jar_path: PathBuf::from("referee.jar"),
                },
                files: HashMap::from([(
                    PathBuf::from("referee.jar"),
                    include_bytes!("../../arena/referees/cg-spring-2024-olympics.jar").to_vec(),
                )]),
            });
            let referee_id = producer.register_target(referee_target);
            info!("Registered referee target {:016x}", referee_id);

            // Register agent targets
            let agent_ids: Vec<TargetId> = agent
                .iter()
                .map(|path| {
                    let source = bundle(&BundlerArgs::default_from_entry(PathBuf::from(path)))
                        .expect("correct bundle")
                        .source
                        .clone();
                    let id = producer.register_target(Target::SourceCode(source));
                    info!("Registered agent target {:016x} from {}", id, path);
                    id
                })
                .collect();

            let producer = Arc::new(producer);

            // Watch for target build errors
            let producer_err = producer.clone();
            std::thread::spawn(move || {
                while let Some((_id, error)) = producer_err.recv_error() {
                    eprintln!("\n{} {}", style("Build error:").red().bold(), error);
                    std::process::exit(1);
                }
            });

            // Receive results in background
            let producer_bg = producer.clone();
            std::thread::spawn(move || {
                while let Some(result) = producer_bg.recv_result() {
                    info!("Result: {:?}", result);
                }
            });

            // Send games
            loop {
                let game_setup = GameSetup {
                    referee_id,
                    agent_ids: agent_ids.clone(),
                    seed: 0, // TODO: generate random seeds
                };
                producer.send_work(game_setup);
            }
        }
        Commands::MCP { protocol: _ } => todo!(),
    }
}
