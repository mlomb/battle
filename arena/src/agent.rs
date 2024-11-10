use std::process::Command;

use crate::executable::Executable;
use bundler::source::Source;

#[derive(Debug)]
pub struct Agent {
    /// The name of the agent
    pub name: String,

    /// The source code of the agent
    /// If absent, the agent is assumed to be a binary
    pub source: Option<Source>,

    /// The binaries of the agent. Can't be both None or be used with source
    pub win_bin: Option<Executable>,
    pub linux_bin: Option<Executable>,
}

impl Agent {
    pub fn from_source<T: ToString>(name: T, source: Source) -> Self {
        Self {
            name: name.to_string(),
            source: Some(source),
            win_bin: None,
            linux_bin: None,
        }
    }

    pub fn from_binaries<T: ToString>(
        name: T,
        win_bin: Option<Executable>,
        linux_bin: Option<Executable>,
    ) -> Self {
        Self {
            name: name.to_string(),
            source: None,
            win_bin,
            linux_bin,
        }
    }

    pub fn command(&mut self) -> Command {
        self.win_bin.as_mut().unwrap().command()
    }

    pub fn id(&self) -> String {
        // TODO: params
        self.name.clone()
    }
}
