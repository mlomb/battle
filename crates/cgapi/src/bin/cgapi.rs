use anyhow::Result;
use clap::Parser;

use battle_cgapi::{CGFetchCli, cgapi_main};

#[tokio::main]
async fn main() -> Result<()> {
    let cli = CGFetchCli::parse();
    cgapi_main(cli).await
}
