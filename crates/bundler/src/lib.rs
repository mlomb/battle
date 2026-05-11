extern crate cargo_metadata;
extern crate quote;
extern crate syn;

mod bundler;
mod cpp;
mod error;
mod rust;
mod source;

use clap::Parser;
use std::path::PathBuf;

pub use crate::bundler::{bundle, Bundle};
pub use crate::error::BundlerError;
pub use crate::source::{Language, Source};

#[derive(Debug, Parser)]
pub struct BundlerArgs {
    /// Entry point file (main.cpp, Cargo.toml) or directory containing an entry file.
    /// If not provided, it will find an appropiate entry point in the current folder.
    pub entry: Option<PathBuf>,
    // TODO: add flags: remove comments, etc
}

impl BundlerArgs {
    pub fn default_from_entry(entry: PathBuf) -> Self {
        Self { entry: Some(entry) }
    }
}

/// Converts a C++/Rust project directory into a single source file
#[derive(Debug, Parser)]
pub struct BundlerCli {
    #[clap(flatten)]
    bundler_args: BundlerArgs,

    /// Output target file.
    /// If not provided, the output will be printed to stdout.
    #[arg(long)]
    output: Option<String>,
}

pub fn bundler_main(args: BundlerCli) {
    match bundle(&args.bundler_args) {
        Ok(bundle) => {
            if let Some(output) = args.output {
                std::fs::write(output, bundle.source.code).expect("a writeable output file");
            } else {
                println!("{}", bundle.source.code);
            }
        }
        Err(err) => {
            eprintln!("Error: {}", err);
            std::process::exit(1);
        }
    }
}
