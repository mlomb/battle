use super::WorkerPool;
use crate::scheduler::{MatchRequest, MatchResult};

pub struct NetworkWorkerPool {}

impl NetworkWorkerPool {
    pub fn new() -> Self {
        // -

        NetworkWorkerPool {}
    }
}

impl WorkerPool for NetworkWorkerPool {
    fn poll_send(&self, req: Option<MatchRequest>) -> Option<MatchResult> {
        todo!()
    }
    //-
}
