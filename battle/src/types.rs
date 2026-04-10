use bundler::source::Source;
use serde::{Deserialize, Serialize};

use crate::builder::Executable;
use crate::network::TargetId;

/// A target that can be sent to workers for building/validation.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Target {
    /// Source code that requires building on the worker before execution.
    SourceCode(Source),
    /// Already compiled/packaged executable.
    Executable(Executable),
}

/// Lightweight game setup referencing pre-registered targets by content hash.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameSetup {
    pub referee_id: TargetId,
    pub agent_ids: Vec<TargetId>,
    pub seed: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameResult {
    pub result: Result<String, String>,
}
