use battle_cgsync::{cgsync_main, CGSyncCli};
use clap::Parser;

#[tokio::main]
async fn main() {
    let args = CGSyncCli::parse();
    cgsync_main(args).await;
}
