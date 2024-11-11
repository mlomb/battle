use serde::{Deserialize, Serialize};

use crate::env::Env;

#[derive(Debug, Serialize, Deserialize)]
pub enum MessageRequest {
    /// A consumer is requesting an unknown peer its `Env`.
    /// If it provides one, the peer is a producer. Otherwise it is a consumer.
    ProvideEnv,
}

#[derive(Debug, Serialize, Deserialize)]
pub enum MessageResponse {
    Dummy,
    /// The peer is a producer with the given `Env`.
    EnvProvided {
        env: Env,
    },
    /// The peer is a consumer.
    EnvNotProvided,
}
