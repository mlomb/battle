pub mod run;

use clap::{Parser, Subcommand};
use run::execute;
use std::{path::PathBuf, time::Duration};

struct Referee {
    path: PathBuf,
}

struct Agent {
    path: PathBuf,
    params: Vec<String>,
}

struct MatchSetup {
    referee: Referee,
    agents: Vec<Agent>,
}

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand, Debug)]
enum Commands {
    /// Runs the given configuration file
    Run {
        /// The configuration file to run (.yml)
        #[arg()]
        config: PathBuf,
    },
    /// Starts a worker that listens for jobs in the local network (via P2P)
    Worker,
}

fn main() {
    let args = Args::parse();

    println!("{:?}", args);

    let N = 100;

    let mut args = vec!["java", "-jar", "summer-2024-olympics-1.0-SNAPSHOT.jar"];

    args.push("-p1");
    args.push("mlomb-146-2.exe");
    args.push("-p2");
    args.push("mlomb-146-2.exe");
    //args.push("SMITS_v04.exe");
    args.push("-p3");
    args.push("mlomb-146-2.exe");
    args.push("-l pepito.txt");
    //args.push("SMITS_v09.exe");

    println!("{:?}", execute(args, Duration::from_secs(10)));
}

// https://github.com/dreignier/game-ultimate-tictactoe/blob/master/src/main/java/com/codingame/gameengine/runner/CommandLineInterface.java
