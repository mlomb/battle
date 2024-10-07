mod expander;

use crate::bundler::Bundler;
use expander::CppExpander;
use std::{error::Error, path::Path};

pub struct CppBundler {}

impl Bundler for CppBundler {
    /// Checks if a file is a C/C++ entrypoint
    fn is_entrypoint(path: &Path) -> bool {
        let name = path.file_name().unwrap_or_default().to_ascii_lowercase();
        name == "main.cpp" || name == "main.c"
    }

    fn bundle(main_path: &Path) -> Result<String, Box<dyn Error>> {
        assert!(Self::is_entrypoint(main_path));

        let source = CppExpander::new().expand_source(main_path)?;

        Ok(format!("{}\n{}", PRAGMAS, source))
    }
}

static PRAGMAS: &str = r###"#pragma warning( disable : 4068 ) // unknown pragma
#pragma GCC optimize("Ofast","unroll-loops","omit-frame-pointer","inline")
#pragma GCC option("arch=native","tune=native","no-zero-upper")
#pragma GCC target("avx,avx2,f16c,fma,sse3,ssse3,sse4.1,sse4.2")
"###;
