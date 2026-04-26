mod project_watcher;
mod ws_server;

use bundler::BundlerArgs;
use project_watcher::run_project_watcher;
use tokio::sync::watch;
use ws_server::start_ws_server;

pub async fn cgsync(bundler_args: BundlerArgs) {
    let (code_tx, code_rx) = watch::channel("(bundler failed, check console)".to_string());

    start_ws_server(code_rx).await;
    run_project_watcher(bundler_args, code_tx);
}
