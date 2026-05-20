use anyhow::{Context, Result};
use reqwest::Client;
use serde_json::{Value, json};

const LIVE_BASE_URL: &str = "https://www.codingame.com/services";
const FIND_GAME_PATH: &str = "/gameResult/findByGameId";
const FIND_AGENT_GAMES_PATH: &str = "/gamesPlayersRanking/findLastBattlesByAgentId";
const LEADERBOARD_PATH: &str = "/Leaderboards/getFilteredChallengeLeaderboard";

pub type GameId = u64;
pub type AgentId = u64;

#[derive(Clone)]
pub struct CGApiClient {
    base_url: String,
    client: Client,
}

impl CGApiClient {
    pub fn prod() -> Result<Self> {
        Self::new(LIVE_BASE_URL)
    }

    fn new(base_url: &str) -> Result<Self> {
        let client = Client::builder()
            .user_agent("mlomb/battle")
            .build()
            .context("building HTTP client")?;

        Ok(Self {
            base_url: base_url.to_string(),
            client,
        })
    }

    async fn cg_post(&self, path: &str, body: Value) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self
            .client
            .post(url.clone())
            .header("Origin", "https://www.codingame.com")
            .header("Content-Type", "application/json;charset=UTF-8")
            .json(&body)
            .send()
            .await
            .with_context(|| format!("POST {url}"))?
            .error_for_status()
            .with_context(|| format!("non-2xx response, body: {}", body))?;

        resp.json::<Value>()
            .await
            .with_context(|| format!("decoding JSON from {url}"))
    }

    /// Returns the game data for the given game ID.
    pub async fn fetch_game(&self, game_id: GameId) -> Result<Value> {
        self.cg_post(FIND_GAME_PATH, json!([game_id, null])).await
    }

    /// Returns the game IDs of the given agent's recent games.
    pub async fn fetch_agent_game_ids(&self, agent_id: AgentId) -> Result<Vec<GameId>> {
        Ok(self
            .cg_post(FIND_AGENT_GAMES_PATH, json!([agent_id, null]))
            .await?
            .as_array()
            .with_context(|| format!("expected array of games for agent {agent_id}"))?
            .iter()
            .filter_map(|g| g.get("gameId").and_then(Value::as_u64))
            .collect())
    }

    /// Returns all (up to 1000) agent IDs from the given contest leaderboard, ordered by rank.
    pub async fn fetch_top_agent_ids(&self, contest: &str) -> Result<Vec<(AgentId, String)>> {
        Ok(self
            .cg_post(
                LEADERBOARD_PATH,
                json!([
                    contest,
                    null,
                    "global",
                    { "active": false, "column": "", "filter": "" },
                ]),
            )
            .await?
            .get("users")
            .and_then(Value::as_array)
            .context("leaderboard response missing `users` array")?
            .iter()
            .filter_map(|u| {
                let agent_id = u.get("agentId").and_then(Value::as_u64)?;
                let pseudo = u
                    .get("pseudo")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_owned();
                Some((agent_id, pseudo))
            })
            .collect())
    }
}
