pub mod api;

use std::path::Path;

use anyhow::{Context, Result};

use api::CGApiClient;

/// Fetches a game by ID and writes it to `<out_dir>/<game_id>.json`.
/// Does nothing if the file already exists.
pub async fn crawl_game(client: &CGApiClient, game_id: u64, out_dir: &Path) -> Result<()> {
    let path = out_dir.join(format!("{game_id}.json"));
    if path.exists() {
        return Ok(());
    }
    let data = client.fetch_game(game_id).await?;
    let pretty = serde_json::to_vec_pretty(&data).context("serializing game JSON")?;
    tokio::fs::write(&path, &pretty)
        .await
        .with_context(|| format!("writing {}", path.display()))?;
    Ok(())
}
