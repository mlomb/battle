use bundler::bundle;
use clap::arg;
use clap::Parser;
use std::path::Path;

/// Converts a C++/Rust project directory into a single source file
#[derive(Debug, Parser)]
struct Cli {
    /// Entry point file (main.cpp, Cargo.toml) or directory containing an entry file.
    /// If not provided, it will find an appropiate entry point in the current folder.
    #[arg(long)]
    entry: Option<String>,

    /// Output target file.
    /// If not provided, the output will be printed to stdout.
    #[arg(long)]
    output: Option<String>,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Cli::parse();
    let entry = args.entry.unwrap_or_else(|| ".".to_string());

    let source = bundle(Path::new(&entry))?;

    if let Some(output) = args.output {
        std::fs::write(output, source)?;
    } else {
        println!("{}", source);
    }

    Ok(())
}
