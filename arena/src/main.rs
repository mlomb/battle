mod network;
pub mod run;

use clap::{Parser, Subcommand};
use futures::executor::ThreadPool;
use futures::StreamExt;
use libp2p::gossipsub::PublishError;
use libp2p::identify;
use libp2p::request_response::ProtocolSupport;
use libp2p::swarm::SwarmEvent;
use libp2p::{gossipsub, mdns, swarm::NetworkBehaviour};
use libp2p::{ping, request_response, Multiaddr, StreamProtocol};
use network::{Event, Message, WorkError};
use run::execute;
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::env::args;
use std::error::Error;
use std::hash::{DefaultHasher, Hash, Hasher};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;
use tokio::runtime::Runtime;
use tokio::{io, io::AsyncBufReadExt, select};
use tracing_subscriber::EnvFilter;

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

fn main() -> Result<(), Box<dyn Error>> {
    let rt = Runtime::new().unwrap();
    let _guard = rt.enter();
    let handle = work();
    rt.block_on(handle);

    // https://github.com/dreignier/game-ultimate-tictactoe/blob/master/src/main/java/com/codingame/gameengine/runner/CommandLineInterface.java

    /*
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

        // -
    */

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
