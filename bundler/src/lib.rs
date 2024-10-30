extern crate cargo_metadata;
extern crate quote;
extern crate syn;

mod bundler;
mod cpp;
mod rust;

use bundler::{Bundle, Bundler};
use cpp::CppBundler;
use rust::RustBundler;
use std::{error::Error, path::Path};

/// Bundles a C++/Rust project directory into a single source unit
pub fn bundle(entry: &Path) -> Result<Bundle, Box<dyn Error>> {
    if let Some(entry) = RustBundler::find_entrypoint(entry) {
        return RustBundler::bundle(entry.as_path());
    }

    if let Some(entry) = CppBundler::find_entrypoint(entry) {
        return CppBundler::bundle(entry.as_path());
    }

    Err("No entrypoint found".into())
}

#[cfg(test)]
mod tests {
    use crate::bundle;

    #[test]
    fn test_cpp_bundle() {
        let bundle = bundle("test_cases/cpp".as_ref()).expect("correct bundle");
        println!("{}", bundle.source);
    }

    #[test]
    fn test_rust_bundle() {
        let bundle = bundle("test_cases/rust".as_ref()).expect("correct bundle");
        println!("{}", bundle.source);
    }
}
