use anyhow::{Context, Result};
use reqwest::Client;
use serde_json::{Value, json};

const FIND_GAME_URL: &str = "https://www.codingame.com/services/gameResult/findByGameId";
const FIND_AGENT_GAMES_URL: &str =
    "https://www.codingame.com/services/gamesPlayersRanking/findLastBattlesByAgentId";
const LEADERBOARD_URL: &str =
    "https://www.codingame.com/services/Leaderboards/getFilteredChallengeLeaderboard";

pub type GameId = u64;
pub type AgentId = u64;

#[derive(Clone)]
pub struct CGApiClient {
    client: Client,
}

impl CGApiClient {
    pub fn new() -> Result<Self> {
        let client = Client::builder()
            .user_agent("mlomb/battle")
            .build()
            .context("building HTTP client")?;

        Ok(Self { client })
    }

    async fn cg_post(&self, url: &str, body: Value) -> Result<Value> {
        let resp = self
            .client
            .post(url)
            .header("Origin", "https://www.codingame.com")
            .header("Content-Type", "application/json;charset=UTF-8")
            .json(&body)
            .send()
            .await
            .with_context(|| format!("POST {url}"))?
            .error_for_status()
            .with_context(|| format!("non-2xx response from {url}"))?;

        resp.json::<Value>()
            .await
            .with_context(|| format!("decoding JSON from {url}"))
    }

    /// Returns the game data for the given game ID.
    pub async fn fetch_game(&self, game_id: GameId) -> Result<Value> {
        self.cg_post(FIND_GAME_URL, json!([game_id, null])).await
    }

    /// Returns the game IDs of the given agent's recent games.
    pub async fn fetch_agent_game_ids(&self, agent_id: AgentId) -> Result<Vec<GameId>> {
        Ok(self
            .cg_post(FIND_AGENT_GAMES_URL, json!([agent_id, null]))
            .await?
            .as_array()
            .with_context(|| format!("expected array of games for agent {agent_id}"))?
            .iter()
            .filter_map(|g| g.get("gameId").and_then(Value::as_u64))
            .collect())
    }

    /// Returns all (up to 1000) agent IDs from the given contest leaderboard, ordered by rank.
    pub async fn fetch_top_agent_ids(&self, contest: &str) -> Result<Vec<AgentId>> {
        Ok(self
            .cg_post(
                LEADERBOARD_URL,
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
            .filter_map(|u| u.get("agentId").and_then(Value::as_u64))
            .collect())
    }
}
