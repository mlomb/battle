pub mod agent;
pub mod network;
pub mod referee;
pub mod result;
pub mod run;

use agent::Agent;
use clap::{Parser, Subcommand};
use crossbeam_channel::bounded;
use futures::{FutureExt, StreamExt};
use network::{Event, WorkError};
use rayon::iter::{
    IntoParallelIterator, IntoParallelRefIterator, ParallelBridge, ParallelIterator,
};
use referee::Referee;
use result::{BasicGenerator, Generator, MatchRequest, MatchResult, ResultReceiver, Summary};
use run::{execute, ExecutionResult};
use serde::{Deserialize, Serialize};
use std::env::args;
use std::error::Error;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

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
    /*
    let rt = Runtime::new().unwrap();
    let _guard = rt.enter();
    let handle = work();
    rt.block_on(handle);
    */

    //let args = Args::parse();
    //println!("{:?}", args);

    fn smth(gen: &mut BasicGenerator) {
        let mut summary = Summary::new();

        // It is necessary to explicit the type due a bug in rust-analyzer
        // https://github.com/rust-lang/rust-analyzer/issues/15984
        let (s, r) = bounded::<MatchRequest>(1);
        let (u, v) = bounded::<MatchResult>(1);

        for i in 0..8 {
            let r = r.clone();
            let u = u.clone();
            std::thread::spawn(move || {
                println!("Starting thread: {}", i);

                loop {
                    let req = r.recv().unwrap();
                    //println!("Received: {:?}", req);
                    let args = req.referee.command(&req.agents);
                    let res = execute(args, Duration::from_secs(10));

                    let scores = res
                        .stdout
                        .split("\n")
                        .take(3)
                        .map(|x| x.parse().unwrap())
                        .collect();

                    // ExecutionResult → MatchResult
                    let res = MatchResult {
                        agents: req.agents.clone(),
                        scores,
                    };

                    u.send(res).unwrap();
                }
            });
        }

        let mut next_req = None;

        // TODO: split in two threads
        loop {
            if let None = next_req {
                next_req = gen.next_game();
            }

            if let Some(req) = next_req.take() {
                crossbeam_channel::select! {
                    recv(v) -> res => {
                        summary.receive_result(res.unwrap());
                        println!("Summary: {:?}", summary);
                    },
                    send(s, req) -> res => {
                        next_req = None;
                        assert!(res.is_ok());
                    },
                }
            } else {
                unimplemented!();
                //crossbeam_channel::select! {
                //    recv(v) -> msg => {
                //        println!("received at main: {:?}", msg);
                //    },
                //    default(Duration::from_secs(1)) => {
                //        println!("timeout");
                //    },
                //}
            }
        }

        drop(s);

        //let pool = rayon::ThreadPoolBuilder::new()
        //    .num_threads(8)
        //    .build()
        //    .unwrap();
        //let a = gen.par_bridge().map(|req| {
        //    let args = req.referee.command(req.agents);
        //
        //    execute(args, Duration::from_secs(10))
        //});
        //
        //// gen.pepito();
        //a.for_each(|x| {
        //    println!("x: {:?}", x);
        //});
    }

    let mut gen = BasicGenerator::new(10000);
    smth(&mut gen);

    // TODO: un generador y receptor de resultados a la vez?
    //       y despues otro que solo recibe resultados, para el summary clasico a parte de lo otro

    // -

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
