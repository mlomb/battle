use crate::env::Env;
use crate::scheduler::{MatchRequest, MatchResult};
use crossbeam_channel::{bounded, Receiver, Sender};
use std::sync::{Arc, Mutex};

use super::WorkerPool;

pub struct LocalWorkerPool {
    input_tx: Sender<MatchRequest>,
    output_rx: Receiver<MatchResult>,
}

impl LocalWorkerPool {
    pub fn new(env: Env, threads: u32) -> Self {
        let (input_tx, input_rx) = bounded::<MatchRequest>(1);
        let (output_tx, output_rx) = bounded::<MatchResult>(1);

        let env = Arc::new(Mutex::new(env));

        for _ in 0..threads {
            let env = env.clone();
            let input_rx = input_rx.clone();
            let output_tx = output_tx.clone();

            std::thread::spawn(move || {
                let worker = Worker::new(env, input_rx, output_tx);
                worker.run();
            });
        }

        LocalWorkerPool {
            input_tx,
            output_rx,
        }
    }
}

impl WorkerPool for LocalWorkerPool {
    fn poll_send(&self, req: Option<MatchRequest>) -> Option<MatchResult> {
        if let Some(req) = req {
            crossbeam_channel::select! {
                send(self.input_tx, req) -> res => {
                    println!("sent: {:?}", res);
                    None
                },
                recv(self.output_rx) -> res => {
                    println!("received at main: {:?}", res);
                    Some(res.unwrap())
                },
            }
        } else {
            self.output_rx.recv().ok()
        }
    }
}

pub struct Worker {
    ///
    env: Arc<Mutex<Env>>,

    input_rx: Receiver<MatchRequest>,
    output_tx: Sender<MatchResult>,
}

impl Worker {
    pub fn new(
        env: Arc<Mutex<Env>>,
        input_rx: Receiver<MatchRequest>,
        output_tx: Sender<MatchResult>,
    ) -> Self {
        Worker {
            env,
            input_rx,
            output_tx,
        }
    }

    pub fn run(&self) {
        loop {
            let request = self.input_rx.recv().unwrap();

            println!("Worker received request: {:?}", request);

            let mut result = MatchResult {
                agents: request.agents.clone(),
                scores: vec![0; request.agents.len()],
            };

            self.output_tx.send(result).unwrap();
        }
    }
}
