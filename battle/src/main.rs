mod builder;

use std::{collections::HashMap, path::PathBuf};

use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};
use console::{Emoji, style};

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

    /// Play a game between multiple bots
    Play {
        /// List of agents participating in the tournament
        #[arg(short, long)]
        agent: Vec<String>,
        // TODO: seed
    },

    /// Start an MCP server
    MCP {
        /// The protocol to use
        #[arg(short, long)]
        protocol: String,
    },

    /// Print all the information from the environment file.
    /// This lets you check that the environment file is being read as you expect.
    Env,
    /// Execute a tournament with the specified configuration
    Tournament {
        /// List of agents participating in the tournament
        #[arg(short, long)]
        agent: Vec<String>,

        /// Number of threads to use for running games.
        /// Cannot be used with `network`.
        /// If set to 0, the default will be the number of physical CPUs minus 2
        #[arg(long, group = "execution_mode", default_value = "0")]
        threads: usize,

        /// Use worker nodes in the P2P network for game execution.
        /// If enabled, games will ONLY run on worker nodes, so ensure at least one worker is available
        #[arg(long, group = "execution_mode")]
        network: bool,
    },
    /// Start a worker node to listen for games on the local P2P network
    Worker {
        /// Number of threads to allocate for the worker.
        /// If set to 0, the default will be the number of physical CPUs minus 2
        #[arg(long, default_value = "0")]
        threads: usize,
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

fn main() {
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
                    println!("{}", bundle.source.code);
                }
            }
            Err(err) => {
                eprintln!("Error: {}", err);
                std::process::exit(1);
            }
        },
        Commands::Build { bundler_args } => {
            println!("{} {}Bundling project...", style("[1/2]").bold().dim(), BOX);

            let bundle = bundle(&bundler_args).expect("correct bundle");

            println!("  OK {} bytes", bundle.source.code.len());

            println!(
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
        Commands::Play { agent } => todo!(),
        Commands::MCP { protocol } => todo!(),
        Commands::Env => todo!(),
        Commands::Tournament {
            agent,
            threads,
            network,
        } => todo!(),
        Commands::Worker { threads } => todo!(),
    }
}
