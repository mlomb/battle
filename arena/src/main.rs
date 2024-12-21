// TODO: remove all pub and check for unused code
pub mod agent;
pub mod database;
pub mod env;
pub mod exec;
pub mod game;
pub mod interactive;
pub mod optim;
pub mod referee;
pub mod tournament;
pub mod worker_pool;

use clap::{Parser, Subcommand};
use console::style;
use database::Database;
use distributed_channel::{start_consumer_node, NodeSetup};
use env::{Env, EnvError};
use game::{run_game, GameResult, GameSetup};
use interactive::build_command_interactive;
use log::{error, info, LevelFilter};
use std::error::Error;
use std::path::PathBuf;
use tournament::Tournament;
use worker_pool::{get_threads, WorkerPool};

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    #[command(subcommand)]
    command: Option<Commands>,

    /// The environment file to use
    #[arg(short, long, default_value = "env.yaml")]
    env: PathBuf,
}

#[derive(Subcommand, Debug)]
enum Commands {
    /// Print all the information from the environment file.
    /// This lets you check that the environment file is being read as you expect.
    Env,
    /// Execute a tournament with the specified configuration
    Tournament {
        /// Specifies the tournament format to use
        #[arg(value_enum, long)]
        format: tournament::Format,

        /// List of agents participating in the tournament
        #[arg(short, long)]
        agent: Vec<String>, // ["agent1,agent2", "agent1,agent3"]

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
            info!(
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
                Commands::Env {} => println!("{}", env),
                Commands::Worker { .. } => unreachable!(), // handled above without reading an env
                Commands::Tournament {
                    format,
                    agent,
                    network,
                    threads,
                } => {
                    let mut tournament = Tournament::new(
                        format,
                        agent
                            .into_iter()
                            .map(|a| a.split(',').map(|s| s.to_string()).collect())
                            .collect(),
                    );
                    let mut database = Database::new();

                    let worker_pool = if network {
                        WorkerPool::make_networked(env, setup)
                    } else {
                        WorkerPool::make_local(env, get_threads(threads))
                    };

                    let mut next_game = tournament.next();
                    loop {
                        match worker_pool.submit_or_receive(next_game.clone()) {
                            None => {
                                // game has been submitted!
                                next_game = tournament.next();
                            }
                            Some(result) => {
                                match result {
                                    Ok(data) => {
                                        // result has been received!
                                        database.receive_result(&data);
                                        println!("{}", database);
                                    }
                                    Err(e) => {
                                        error!("Error running game: {:?}", style(e).red());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Err(EnvError::NotFound) => {
            error!("Env file not found {}", style(args.env.display()).magenta())
        }
        Err(EnvError::ParseError(e)) => {
            error!("Error parsing the YAML file {}", style(e).red())
        }
        Err(EnvError::NoAgents) => error!("No agents provided. Please provide at least one."),
        Err(EnvError::BadReferee(e)) => {
            error!("Bad referee: {}", style(e).red())
        }
        Err(EnvError::BadAgent(e)) => {
            error!("Bad agent: {}", style(e).red())
        }
        Err(EnvError::BadField(e)) => error!("Bad field: {}", style(e).red()),
        Err(EnvError::BundleError {
            agent,
            src_path,
            error,
        }) => {
            error!(
                "Error bundling agent {} ({}): {}",
                style(agent).magenta(),
                style(src_path.display()).cyan(),
                style(error).red()
            )
        }
    }

    Ok(())
}
