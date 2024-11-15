pub mod agent;
pub mod env;
pub mod exec;
pub mod game;
pub mod interactive;
pub mod local_worker;
pub mod optim;
pub mod referee;
pub mod scheduler;
pub mod tournament;

use clap::{Parser, Subcommand};
use console::style;
use distributed_channel::{start_consumer_node, NodeSetup};
use env::{Env, EnvError};
use game::{run_game, GameResult, GameSetup};
use interactive::build_command_interactive;
use local_worker::LocalGameWorker;
use log::{info, LevelFilter};
use std::error::Error;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    #[command(subcommand)]
    command: Option<Commands>,

    /// The environment file to use
    #[arg(short, long, default_value = "env.yml")]
    env: PathBuf,
}

#[derive(Subcommand, Debug)]
enum Commands {
    /// Execute a tournament with the specified configuration
    Tournament {
        /// Specifies the tournament format to use
        #[arg(value_enum, long)]
        format: tournament::format::Format,

        /// List of agents participating in the tournament
        #[arg(short, long)]
        agent: Vec<String>, // ["agent1,agent2", "agent1,agent3"]

        /// Number of threads to use for running games.
        /// Cannot be used with `network`.
        /// If set to 0, the default will be the number of logical CPUs minus 1
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
        /// If set to 0, the default will be the number of logical CPUs minus 1
        #[arg(long, default_value = "0")]
        threads: usize,
    },
}

fn main() -> Result<(), Box<dyn Error>> {
    env_logger::Builder::new()
        .filter_module("distributed_channel", LevelFilter::Trace)
        .filter_module("arena", LevelFilter::Trace)
        .init();

    let args = Args::parse();

    let mut setup = NodeSetup::default();
    setup.protocol = "/mlomb/bot-tools/arena/1".to_string();

    if let Some(Commands::Worker { threads }) = args.command {
        info!("Starting worker node...");
        start_consumer_node::<Env, GameSetup, GameResult>(setup, get_threads(threads), run_game);
        return Ok(());
    }

    match Env::from_file(&args.env) {
        Ok(env) => {
            println!(
                "{} Env file read {}. Found {} agents.",
                style("[OK]").green().bold(),
                style(args.env.display()).magenta(),
                style(env.agents.len()).cyan(),
            );

            let command = if let Some(cmd) = args.command {
                cmd
            } else {
                Args::parse_from(build_command_interactive(args.env, &env))
                    .command
                    .expect("a well constructed command")
            };

            match command {
                Commands::Worker { .. } => unreachable!(),
                Commands::Tournament {
                    format,
                    agent,
                    network,
                    threads,
                } => {
                    println!("Running agents: {:?}", agent);

                    let (_node, tx, rx) = if network {
                        let (_node, tx, rx) =
                            setup.into_producer::<Env, GameSetup, GameResult>(env);
                        (Some(_node), tx, rx)
                    } else {
                        let (tx, rx) = crossbeam_channel::unbounded();
                        LocalGameWorker::new(env, threads as u32);

                        (None, tx, rx)
                    };

                    loop {
                        crossbeam_channel::select! {
                            recv(rx) -> res => {
                                let res = res.unwrap();
                                println!("Received: {:?}", res);
                            },
                            send(tx, GameSetup::new()) -> res => {
                                let res = res.unwrap();
                            },
                        }
                    }
                }
            }
        }
        Err(err) => {
            println!(
                "{} {}",
                style("[E]").red().bold(),
                match err {
                    EnvError::NotFound => {
                        format!("Env file not found {}", style(args.env.display()).magenta())
                    }
                    EnvError::ParseError(e) => {
                        format!("Error parsing the YAML file {}", style(e).red())
                    }
                    EnvError::NoAgents =>
                        format!("No agents provided. Please provide at least one."),
                    EnvError::BadReferee(e) => {
                        format!("Bad referee: {}", style(e).red())
                    }
                    EnvError::BadAgent(e) => {
                        format!("Bad agent: {}", style(e).red())
                    }
                    EnvError::BadField(e) => format!("Bad field: {}", style(e).red()),
                    EnvError::BundleError {
                        agent,
                        src_path,
                        error,
                    } => {
                        format!(
                            "Error bundling agent {} ({}): {}",
                            style(agent).magenta(),
                            style(src_path.display()).cyan(),
                            style(error).red()
                        )
                    }
                }
            );
        }
    }

    Ok(())
}

fn get_threads(mut threads: usize) -> usize {
    if threads == 0 {
        threads = (num_cpus::get_physical() - 2).max(1);
    }
    info!("Using {}", style(format!("{} threads", threads)).cyan());
    threads
}
