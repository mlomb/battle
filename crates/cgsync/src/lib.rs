mod project_watcher;
mod ws_server;

use bundler::BundlerArgs;
use clap::Parser;
use console::style;
use tokio::sync::watch;

use project_watcher::run_project_watcher;
use ws_server::start_ws_server;

/// Sync code between your local Rust/C++ project and CodinGame IDE.
///
/// It watches for file changes and sends the code to the CG Local extension via a WebSocket.
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
pub struct CGSyncCli {
    #[clap(flatten)]
    bundler_args: BundlerArgs,
}

pub async fn cgsync_main(args: CGSyncCli) {
    let (code_tx, code_rx) = watch::channel("(bundler failed, check console)".to_string());

    tokio::spawn(start_ws_server(code_rx));
    tokio::spawn(run_project_watcher(args.bundler_args, code_tx));

    tokio::signal::ctrl_c().await.ok();

    println!("{} Exit signal received, exiting...", style("[I]").blue());
    // TODO: proper shutdown?
    println!("{} Goodbye!", style("[I]").blue());
}
