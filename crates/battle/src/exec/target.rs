use std::path::PathBuf;

use bundler::{BundlerArgs, BundlerError, Source, bundle};
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
    fn new(kind: TargetKind) -> Self {
        Target {
            id: {
                let bytes = postcard::to_allocvec(&kind).expect("failed to serialize target");
                let hash = blake3::hash(&bytes);
                u64::from_le_bytes(hash.as_bytes()[..8].try_into().unwrap())
            },
            kind,
        }
    }

    pub fn from_executable(executable: Executable) -> Self {
        Self::new(TargetKind::Executable(executable))
    }

    pub fn from_entrypoint(entry: impl Into<PathBuf>) -> Result<Self, BundlerError> {
        let bundle_out = bundle(&BundlerArgs::default_from_entry(entry.into()))?;
        Ok(Self::new(TargetKind::SourceCode(bundle_out.source)))
    }
}
