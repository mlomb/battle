mod project_watcher;
mod ws_server;

use bundler::{bundle, BundlerArgs};
use clap::Parser;
use console::style;
use notify::{RecursiveMode, Watcher};
use project_watcher::{start_project_watcher, ProjectWatcher};
use std::collections::HashSet;
use tokio::sync::watch;
use ws_server::start_ws_server;

#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    #[clap(flatten)]
    bundler_args: BundlerArgs,
}

#[tokio::main]
async fn main() {
    let args = Args::parse();

    let (code_tx, code_rx) = watch::channel("".to_string());

    start_ws_server(code_rx).await;
    start_project_watcher(args.bundler_args, code_tx);

    std::future::pending::<u32>().await;
    unreachable!();
}
