use std::path::PathBuf;

use bundler::{BundlerArgs, bundle};
use clap::{Parser, Subcommand};

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
        /// Entrypoint file (main.cpp, Cargo.toml) or directory containing an entry file.
        #[arg(short, long)]
        entrypoint: String,
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
        Commands::Build { entrypoint } => todo!(),
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

    println!("Hello, world!");
}
