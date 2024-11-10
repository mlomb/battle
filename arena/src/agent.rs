use std::process::Command;

use crate::{executable::Executable, source_build::SourceBuilder};
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
        if win_bin.is_none() && linux_bin.is_none() {
            panic!("at least one binary should be available");
        }

        Self {
            name: name.to_string(),
            source: None,
            win_bin,
            linux_bin,
        }
    }

    pub fn command(&mut self) -> Command {
        let bin = if cfg!(windows) {
            &mut self.win_bin
        } else {
            &mut self.linux_bin
        };

        if bin.is_none() {
            // gotta build it
            bin.replace(
                self.source
                    .clone()
                    .expect("source available when there are no binaries")
                    .build()
                    .expect("build to succeed"),
            );
        }

        bin.as_mut().expect("a working executable").command()
    }

    pub fn id(&self) -> String {
        // TODO: params
        self.name.clone()
    }
}
