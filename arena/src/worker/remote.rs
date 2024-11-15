use super::WorkerPool;
use crate::{
    env::Env,
    game::{GameResult, GameSetup},
    scheduler::{MatchRequest, MatchResult},
};
use crossbeam_channel::{Receiver, Sender};

pub struct NetworkWorkerPool {
    producer_node: distributed_channel::Node,

    setup_tx: Sender<GameSetup>,
    result_rx: Receiver<GameResult>,
}

impl NetworkWorkerPool {
    pub fn new(env: Env) -> Self {
        let (producer_node, setup_tx, result_rx) =
            distributed_channel::NodeSetup::default().into_producer(env);

        NetworkWorkerPool {
            producer_node,
            setup_tx,
            result_rx,
        }
    }
}

impl WorkerPool for NetworkWorkerPool {
    fn poll_send(&self, req: Option<MatchRequest>) -> Option<MatchResult> {
        todo!()
    }
    //-
}
