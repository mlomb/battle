pub mod agent;
pub mod env;
pub mod exec;
pub mod game;
pub mod interactive;
pub mod optim;
pub mod param;
pub mod referee;
pub mod scheduler;
pub mod tournament;
pub mod worker;

// TODO: errors and logging is lacking

use clap::{Parser, Subcommand};
use console::style;
use crossbeam_channel::select;
use distributed_channel::{start_consumer_node, NodeSetup};
use env::{Env, EnvError};
use game::{run_game, GameResult, GameSetup};
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
    /// Runs a tournament
    Tournament {
        /// Tournament format
        #[arg(value_enum, long)]
        format: tournament::format::Format,

        /// Agents to run the tournament with
        #[arg(short, long)]
        agent: Vec<String>, // ["agent1,agent2", "agent1,agent3"]
    },
    /// Starts a worker that listens for jobs in the local network (via P2P)
    Worker {
        /// Number of threads to use
        /// If 0, the number of threads will be the number of logical CPUs - 1
        #[arg(short, long, default_value = "0")]
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
        info!("Starting worker");
        start_consumer_node::<Env, GameSetup, GameResult>(setup, threads, run_game);
        return Ok(());
    }

    match Env::from_file(&args.env) {
        Ok(env) => {
            println!(
                "{} Env file read {}. Found {} agents.",
                style("[OK]").green().bold(),
                style(args.env.display()).magenta(),
                0 //style(env.agents.len()).cyan(),
            );

            let (_node, tx, rx) = setup.into_producer::<Env, GameSetup, GameResult>(env);

            loop {
                select! {
                    recv(rx) -> res => {
                        let res = res.unwrap();
                        println!("Received: {:?}", res);
                    },
                    send(tx, GameSetup::new()) -> res => {
                        let res = res.unwrap();
                    },
                }
            }

            /*
            let command = if let Some(cmd) = args.command {
                cmd
            } else {
                Args::parse_from(build_command_interactive(args.env, env))
                    .command
                    .expect("a well constructed command")
            };

            match command {
                Commands::Tournament { format, agent } => {
                    println!("Running agents: {:?}", agent);
                }
                Commands::Worker => panic!("Worker should not be invoked here"),
            }
            */
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
