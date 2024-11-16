use crate::env::Env;
use crate::game::{run_game, GameResult, GameSetup};
use console::style;
use crossbeam_channel::{bounded, unbounded, Receiver, Sender};
use distributed_channel::{Node, NodeSetup};
use log::info;
use std::sync::{Arc, Mutex};

pub struct WorkerPool {
    input_tx: Sender<GameSetup>,
    output_rx: Receiver<GameResult>,

    _network_node: Option<Node>,
    _local_worker: Option<LocalGameWorker>,
}

impl WorkerPool {
    pub fn make_local(env: Env, threads: usize) -> Self {
        info!("Using a local thread pool");

        let local_worker = LocalGameWorker::new(env, threads);

        WorkerPool {
            input_tx: local_worker.input_tx.clone(),
            output_rx: local_worker.output_rx.clone(),
            _network_node: None,
            _local_worker: Some(local_worker),
        }
    }

    pub fn make_networked(env: Env, setup: NodeSetup) -> Self {
        info!("Using a networked worker pool");

        let (node, input_tx, output_rx) = setup.into_producer::<Env, GameSetup, GameResult>(env);

        WorkerPool {
            input_tx,
            output_rx,
            _network_node: Some(node),
            _local_worker: None,
        }
    }

    pub fn submit_or_receive(&self, setup: Option<GameSetup>) -> Option<GameResult> {
        if let Some(setup) = setup {
            crossbeam_channel::select! {
                recv(self.output_rx) -> res => res.ok(),
                send(self.input_tx, setup) -> _ => None,
            }
        } else {
            self.output_rx.recv().ok()
        }
    }
}

struct LocalGameWorker {
    pub input_tx: Sender<GameSetup>,
    pub output_rx: Receiver<GameResult>,
}

impl LocalGameWorker {
    pub fn new(env: Env, threads: usize) -> Self {
        let env = Arc::new(Mutex::new(env));
        let (input_tx, input_rx) = bounded::<GameSetup>(1);
        let (output_tx, output_rx) = unbounded::<GameResult>();

        for _ in 0..threads {
            let env = env.clone();
            let input_rx = input_rx.clone();
            let output_tx = output_tx.clone();

            std::thread::spawn(move || loop {
                match input_rx.recv() {
                    Ok(work) => output_tx.send(run_game(env.clone(), work)).unwrap(),
                    Err(_) => break,
                }
            });
        }

        LocalGameWorker {
            input_tx,
            output_rx,
        }
    }
}

pub fn get_threads(mut threads: usize) -> usize {
    if threads == 0 {
        threads = (num_cpus::get_physical() - 2).max(1);
    }

    info!("Using {}", style(format!("{} threads", threads)).cyan());

    threads
}
