pub mod agent;
mod network;
pub mod referee;
pub mod run;

use agent::Agent;
use clap::{Parser, Subcommand};
use futures::StreamExt;
use network::{Event, WorkError};
use referee::Referee;
use run::execute;
use serde::{Deserialize, Serialize};
use std::env::args;
use std::error::Error;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

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

// https://github.com/libp2p/rust-libp2p/blob/master/examples/file-sharing/src/main.rs
// https://github.com/libp2p/rust-libp2p/blob/master/examples/file-sharing/src/network.rs

fn main() -> Result<(), Box<dyn Error>> {
    // let rt = Runtime::new().unwrap();
    // let _guard = rt.enter();
    // let handle = work();
    // rt.block_on(handle);

    let args = Args::parse();

    println!("{:?}", args);

    let N = 100;

    let referee = Referee::new(PathBuf::from("summer-2024-olympics-1.0-SNAPSHOT.jar"));

    let agents = vec![
        Agent::new("mlomb-146-2.exe".into()),
        Agent::new("mlomb-146-2.exe".into()),
        Agent::new("mlomb-146-2.exe".into()), // SMITS_v04.exe
    ];

    // referee
    let args = referee.command(agents);

    println!("Args: {:?}", args);

    println!("{:?}", execute(args, Duration::from_secs(10)));

    // -

    /*
    let rt = Runtime::new().unwrap();
    let _guard = rt.enter();
    let handle = work();
    rt.block_on(handle);
    */

    // https://github.com/dreignier/game-ultimate-tictactoe/blob/master/src/main/java/com/codingame/gameengine/runner/CommandLineInterface.java

    Ok(())
}

type WorkId = u64;

#[derive(Debug, Serialize, Deserialize)]
pub struct Work {
    pub id: WorkId,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct WorkResult {
    pub id: WorkId,
}

async fn work() {
    let (mut network_client, mut network_events, mut network_loop) = network::new().await.unwrap();

    if args().len() > 1 {
        println!("Worker mode");
        //network_client.set_work_slots(2);
    }

    tokio::spawn(network_loop.run());

    // lock rw int
    let active_threads = Arc::new(Mutex::new(0));

    loop {
        //network_client.send_work(Work { num: 123 }).await;
        match network_events.next().await {
            Some(Event::WorkRequested { sender }) => {
                if args().len() > 1 {
                    sender.send(None).unwrap();
                } else {
                    sender.send(Some(Work { id: 123 })).unwrap();
                }
            }
            Some(Event::DoWork { work, sender }) => {
                let active_threads = active_threads.clone();

                if *active_threads.lock().unwrap() >= 2 {
                    sender.send(Err(WorkError::WorkerIsBusy)).unwrap();
                    continue;
                }

                thread::spawn(move || {
                    *active_threads.lock().unwrap() += 1;

                    println!("PREV: Workers active: {}", *active_threads.lock().unwrap());
                    thread::sleep(Duration::from_secs(2));
                    println!("NEXT: Workers active: {}", *active_threads.lock().unwrap());

                    *active_threads.lock().unwrap() -= 1;
                    sender.send(Ok(WorkResult { id: 123 })).unwrap();
                });
            }
            Some(Event::WorkDone { result }) => match result {
                Ok(work) => println!("Work done: {:?}", work),
                Err(err) => {
                    // asd
                    match err {
                        WorkError::WorkerIsBusy => {}
                        e => println!("Work failed: {:?}", e),
                    }
                }
            },
            // Some(e) => println!("E: {:?}", e),
            None => {}
        }
    }
}
