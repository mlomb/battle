use crate::exec::{
    command::ToCommand,
    executable::{Executable, ExecutableError},
};
use serde::{Deserialize, Serialize};
use std::process::Command;

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Agent {
    /// The name of the agent
    pub name: String,

    pub executable: Executable,
}

impl Agent {
    pub fn new<T: ToString>(name: T, executable: Executable) -> Self {
        Self {
            name: name.to_string(),
            executable,
        }
    }

    pub fn id(&self) -> String {
        // TODO: params
        self.name.clone()
    }
}

impl ToCommand for Agent {
    fn command(&mut self) -> Result<Command, ExecutableError> {
        self.executable.command()
    }
}
