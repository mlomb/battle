mod project_watcher;
mod ws_server;

use bundler::BundlerArgs;
use clap::Parser;
use project_watcher::run_project_watcher;
use tokio::sync::watch;
use ws_server::start_ws_server;

/// A tool to sync code between your local Rust/C++ project and CodinGame in the browser.
/// It watches for file changes and sends the code to the CG Local extension via a WebSocket.
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    #[clap(flatten)]
    bundler_args: BundlerArgs,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let (code_tx, code_rx) = watch::channel("(bundler failed, check console)".to_string());

    start_ws_server(code_rx).await;
    run_project_watcher(args.bundler_args, code_tx);
}
