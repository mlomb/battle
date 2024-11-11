pub mod local;
pub mod remote;

use crate::scheduler::{MatchRequest, MatchResult};

pub trait WorkerPool {
    ///
    fn poll_send(&self, req: Option<MatchRequest>) -> Option<MatchResult>;
}
