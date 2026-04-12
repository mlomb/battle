use std::{collections::HashMap, ffi::OsStr, path::PathBuf};

use bundler::source::Source;
use serde::{Deserialize, Serialize};

use crate::builder::{Executable, ExecutableKind};

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

    /// Creates a new executable that runs a JAR file ("java -jar main.jar")
    pub fn from_jar(jar_path: PathBuf) -> Result<Self, std::io::Error> {
        let name = PathBuf::from(jar_path.file_name().unwrap_or(OsStr::new("jarfile.jar")));

        Ok(Self::Executable(Executable {
            kind: ExecutableKind::Jar {
                jar_path: name.clone(),
            },
            files: HashMap::from([(name, std::fs::read(&jar_path)?)]),
        }))
    }
}
