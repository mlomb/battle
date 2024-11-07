use bundler::bundle;
use bundler::BundlerArgs;
use clap::arg;
use clap::Parser;

/// Converts a C++/Rust project directory into a single source file
#[derive(Debug, Parser)]
struct Cli {
    #[clap(flatten)]
    bundler_args: BundlerArgs,

    /// Output target file.
    /// If not provided, the output will be printed to stdout.
    #[arg(long)]
    output: Option<String>,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Cli::parse();

    let bundle = bundle(&args.bundler_args)?;

    if let Some(output) = args.output {
        std::fs::write(output, bundle.source)?;
    } else {
        println!("{}", bundle.source);
    }

    Ok(())
}
