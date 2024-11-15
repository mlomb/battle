use crate::env::Env;
use crate::game::{run_game, GameResult, GameSetup};
use crossbeam_channel::{bounded, unbounded, Receiver, Sender};
use std::sync::{Arc, Mutex};

pub struct LocalGameWorker {
    pub input_tx: Sender<GameSetup>,
    pub output_rx: Receiver<GameResult>,
}

impl LocalGameWorker {
    pub fn new(env: Env, threads: u32) -> Self {
        let env = Arc::new(Mutex::new(env));
        let (input_tx, input_rx) = bounded::<GameSetup>(1);
        let (output_tx, output_rx) = unbounded::<GameResult>();

        (0..threads).map(|_| {
            let env = env.clone();
            let input_rx = input_rx.clone();
            let output_tx = output_tx.clone();

            std::thread::spawn(move || loop {
                match input_rx.recv() {
                    Ok(work) => output_tx.send(run_game(env.clone(), work)).unwrap(),
                    Err(_) => break,
                }
            })
        });

        LocalGameWorker {
            input_tx,
            output_rx,
        }
    }
}
