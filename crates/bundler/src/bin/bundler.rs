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

fn main() {
    let args = Cli::parse();

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
