use serde::{Deserialize, Serialize};
use std::fmt::Debug;

/// The language of the bundled source code
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub enum Language {
    Cpp,
    Rust,
}

/// The final source code of a bundle
#[derive(Clone, Serialize, Deserialize)]
pub struct Source {
    /// The code
    pub code: String,

    /// The language of the code
    pub language: Language,
}

impl Debug for Source {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Source {{ code: ({} bytes), language: {:?} }}",
            self.code.len(),
            self.language
        )
    }
}
