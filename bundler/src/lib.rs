extern crate cargo_metadata;
extern crate quote;
extern crate syn;

pub mod bundler;
mod cpp;
mod parameter;
mod rust;

use bundler::Bundler;
use clap::Parser;
use cpp::CppBundler;
use parameter::Parameter;
use rust::RustBundler;
use std::{
    collections::{HashMap, HashSet},
    error::Error,
    path::{Path, PathBuf},
};

#[derive(Debug, Parser)]
pub struct BundlerArgs {
    /// Entry point file (main.cpp, Cargo.toml) or directory containing an entry file.
    /// If not provided, it will find an appropiate entry point in the current folder.
    #[arg(long)]
    entry: Option<String>,
    // TODO: add flags: remove comments, etc
}

impl BundlerArgs {
    pub fn default_from_path(path: &str) -> Self {
        Self {
            entry: Some(path.to_string()),
        }
    }
}

/// The language of the bundled source code
#[derive(Debug)]
pub enum BundleLanguage {
    Cpp,
    Rust,
}

/// The result of bundling a project
#[derive(Debug)]
pub struct Bundle {
    /// The bundled source code
    pub source: String,

    /// The language of the bundled source code
    pub language: BundleLanguage,

    /// Parameters found in the original source. Now available to set via standard arguments
    pub params: HashMap<String, Parameter>,

    /// All relevant files used to create the bundle (and should be watched)
    pub src_files: HashSet<PathBuf>,
}

/// Bundles a C++/Rust project directory into a single source unit
pub fn bundle(args: &BundlerArgs) -> Result<Bundle, Box<dyn Error>> {
    let entry = args.entry.clone().unwrap_or_else(|| ".".to_string());
    let entry = Path::new(&entry);

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
    use crate::{bundle, BundlerArgs};

    #[test]
    fn test_cpp_bundle() {
        let bundle = bundle(&BundlerArgs {
            entry: Some("test_cases/cpp".to_string()),
        })
        .expect("correct bundle");
        println!("{}", bundle.source);
    }

    #[test]
    fn test_rust_main_bundle() {
        let bundle = bundle(&BundlerArgs {
            entry: Some("test_cases/rust_main".to_string()),
        })
        .expect("correct bundle");
        println!("{}", bundle.source);
    }

    #[test]
    fn test_rust_bin_bundle() {
        let bundle = bundle(&BundlerArgs {
            entry: Some("test_cases/rust_bin".to_string()),
        })
        .expect("correct bundle");
        println!("{}", bundle.source);
    }
}
