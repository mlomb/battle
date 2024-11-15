use crate::{
    env::Env,
    game::{GameResult, GameSetup},
};
use distributed_channel::{consumer::WorkRx, NodeSetup};

pub struct NetworkWorker {
    consumer_node: distributed_channel::Node,
    work_rx: WorkRx<Env, GameSetup, GameResult>,

    local_pool: super::local::LocalWorkerPool,
}

impl NetworkWorker {
    pub fn new() -> Self {
        let (consumer_node, work_rx) = node_setup().into_consumer();

        NetworkWorker {
            consumer_node,
            work_rx,
            local_pool: super::local::LocalWorkerPool::new(Env::new(), 1),
        }
    }
}
pub fn node_setup() -> NodeSetup {
    let mut setup = NodeSetup::default();
    setup.protocol = "/mlomb/bot-tools/arena/1".to_string();
    setup
}
