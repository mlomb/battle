use bundler::Source;
use serde::{Deserialize, Serialize};

use crate::exec::executable::Executable;

pub type TargetId = u64;

/// The inner kind of a target.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum TargetKind {
    /// Source code that requires building on the worker before execution.
    SourceCode(Source),
    /// Already compiled/packaged executable.
    Executable(Executable),
}

/// A target that can be sent to workers for building/validation.
///
/// The content hash (`id`) is computed lazily on first access and cached.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Target {
    pub id: TargetId,

    pub kind: TargetKind,
}

impl Target {
    pub fn new(kind: TargetKind) -> Self {
        Target {
            id: {
                let bytes = postcard::to_allocvec(&kind).expect("failed to serialize target");
                let hash = blake3::hash(&bytes);
                u64::from_le_bytes(hash.as_bytes()[..8].try_into().unwrap())
            },
            kind,
        }
    }
}
