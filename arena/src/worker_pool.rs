use crate::env::Env;
use crate::game::{run_game, GameResult, GameSetup};
use console::style;
use crossbeam_channel::{bounded, unbounded, Receiver, Sender};
use distributed_channel::{Node, NodeSetup, ProducerHandle};
use log::info;
use std::sync::Arc;
use std::sync::Mutex;

pub struct WorkerPool {
    input_tx: Sender<GameSetup>,
    output_rx: Receiver<GameResult>,

    _network_node: Option<Node>,
    _network_producer: Option<Arc<ProducerHandle<Env, GameSetup, GameResult>>>,
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
            _network_producer: None,
            _local_worker: Some(local_worker),
        }
    }

    // TODO: rework for the new target registration protocol
    pub fn make_networked(env: Env, setup: NodeSetup) -> Self {
        info!("Using a networked worker pool");

        let (node, producer) = setup.into_producer::<Env, GameSetup, GameResult>();
        producer.register_target(env);

        let producer = Arc::new(producer);
        let (input_tx, input_rx) = bounded::<GameSetup>(1);
        let (output_tx, output_rx) = unbounded::<GameResult>();

        let p = producer.clone();
        std::thread::spawn(move || {
            while let Ok(work) = input_rx.recv() {
                p.send_work(work);
            }
        });

        let p = producer.clone();
        std::thread::spawn(move || {
            while let Some(result) = p.recv_result() {
                if output_tx.send(result).is_err() {
                    break;
                }
            }
        });

        WorkerPool {
            input_tx,
            output_rx,
            _network_node: Some(node),
            _network_producer: Some(producer),
            _local_worker: None,
        }
    }

    pub fn submit_or_receive(&self, setup: Option<GameSetup>) -> Option<GameResult> {
        if let Some(setup) = setup {
            crossbeam_channel::select! {
                // TODO: really handle the failure
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
