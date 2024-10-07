extern crate cargo_metadata;
extern crate quote;
extern crate syn;

mod bundler;
mod cpp;
mod rust;

use bundler::Bundler;
use cpp::CppBundler;
use rust::RustBundler;
use std::{error::Error, path::Path};

/// Bundles a C++/Rust project directory into a single source file
/// TODO: parameters (constants)
/// TODO: return lang
/// TODO: watchable files/directories?
pub fn bundle(entry: &Path) -> Result<String, Box<dyn Error>> {
    if let Some(entry) = RustBundler::find_entrypoint(entry) {
        return RustBundler::bundle(entry.as_path());
    }

    if let Some(entry) = CppBundler::find_entrypoint(entry) {
        return CppBundler::bundle(entry.as_path());
    }

    Err("No entrypoint found".into())
}
