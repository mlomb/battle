use crate::bundler::Bundler;
use std::{error::Error, path::Path};

// https://docs.rs/tree-sitter-cpp/latest/tree_sitter_cpp/

pub struct CppBundler {}

impl CppBundler {
    pub fn new() -> Self {
        Self {}
    }
}

impl Bundler for CppBundler {
    /// Checks if a file is a C/C++ entrypoint
    fn is_entrypoint(path: &Path) -> bool {
        let ext = path.extension().unwrap_or_default().to_ascii_lowercase();
        ext == "cpp" || ext == "c"
    }

    fn bundle(path: &Path) -> Result<String, Box<dyn Error>> {
        unimplemented!()
    }
}
