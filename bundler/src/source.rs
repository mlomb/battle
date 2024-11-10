/// The language of the bundled source code
#[derive(Debug, Clone, Copy)]
pub enum Language {
    Cpp,
    Rust,
}

/// The final source code of a bundle
#[derive(Debug, Clone)]
pub struct Source {
    /// The code
    pub code: String,

    /// The language of the code
    pub language: Language,
}
