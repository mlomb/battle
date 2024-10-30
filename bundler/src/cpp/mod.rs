mod expander;

use crate::bundler::{Bundle, Bundler};
use expander::CppExpander;
use std::{error::Error, path::Path};

pub struct CppBundler {}

impl Bundler for CppBundler {
    /// Checks if a file is a C/C++ entrypoint
    fn is_entrypoint(path: &Path) -> bool {
        let name = path.file_name().unwrap_or_default().to_ascii_lowercase();
        name == "main.cpp" || name == "main.c"
    }

    fn bundle(main_path: &Path) -> Result<Bundle, Box<dyn Error>> {
        assert!(Self::is_entrypoint(main_path));

        let source = CppExpander::new().expand_source(main_path)?;

        Ok(Bundle {
            source: format!("{}\n{}", PRAGMAS, source),
            files: vec![],
        })
    }
}

static PRAGMAS: &str = include_str!("pragmas.c");
