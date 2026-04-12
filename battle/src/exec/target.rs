use bundler::source::Source;
use serde::{Deserialize, Serialize};

use crate::exec::executable::Executable;

pub type TargetId = u64;

/// A target that can be sent to workers for building/validation.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Target {
    /// Source code that requires building on the worker before execution.
    SourceCode(Source),
    /// Already compiled/packaged executable.
    Executable(Executable),
}

impl Target {
    pub fn id(&self) -> TargetId {
        // TODO: optimize
        let bytes = postcard::to_allocvec(self).expect("failed to serialize target");
        let hash = blake3::hash(&bytes);
        u64::from_le_bytes(hash.as_bytes()[..8].try_into().unwrap())
    }
}
