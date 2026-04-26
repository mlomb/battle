use bundler::BundlerArgs;
use cgsync::cgsync;
use clap::Parser;

/// A tool to sync code between your local Rust/C++ project and CodinGame in the browser.
/// It watches for file changes and sends the code to the CG Local extension via a WebSocket.
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Cli {
    #[clap(flatten)]
    bundler_args: BundlerArgs,
}

#[tokio::main]
async fn main() {
    let args = Cli::parse();

    cgsync(args.bundler_args).await;
}
