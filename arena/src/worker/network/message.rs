use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub enum MessageRequest {
    RequestWork,
    ProvideWork,
    DeliverWork,
}

#[derive(Debug, Serialize, Deserialize)]
pub enum MessageResponse {
    WorkRequested,
    WorkReceived,
    WorkFinished,
}
