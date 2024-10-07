mod expander;

use expander::CppExpander;

use crate::bundler::Bundler;
use std::{
    error::Error,
    fs,
    path::{Path, PathBuf},
};

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

    fn bundle(main_path: &Path) -> Result<String, Box<dyn Error>> {
        assert!(Self::is_entrypoint(main_path));

        CppExpander::new().expand_source(main_path)
    }
}
