use super::{
    command::ToCommand,
    executable_command::ExecutableCommand,
    source_builder::{BuildError, SourceBuilder},
};
use bundler::source::Source;
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, path::PathBuf, process::Command};

/// An executable
///
/// This struct implements the [`ToCommand`] trait, that returns a command that can be executed.
#[derive(Serialize, Deserialize, Debug, Clone)]
pub enum Executable {
    /// A generic executable that can be run on any platform (e.g. java, python)
    /// It usually requires a pre-installed software to run
    GenericCommand(ExecutableCommand),
    /// An executable that requires a different command for each platform (e.g. main.exe, ./main)
    PlatformCommand {
        windows: Option<ExecutableCommand>,
        unix: Option<ExecutableCommand>,
    },
    /// The executable result of compiling source code (e.g. C, Rust)
    Source {
        source: Source,
        /// The compiled executable, lazily initialized when `command` is called.
        executable: Option<ExecutableCommand>,
        /// Additional files required for execution
        assets: HashMap<String, Vec<u8>>,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum ExecutableError {
    /// The source code could not compile
    BuildFailed(BuildError),
    /// The executable cannot run on the current platform
    UnsupportedPlatform,
    /// The name of the file is invalid
    InvalidFileName(PathBuf),
    /// The file was not found
    FileNotFound(PathBuf),
    /// Failed to initialize the temporary directory
    TempDirFailed(String),
    /// Failed to write the file to disk
    WriteFileFailed(String),
}

impl Executable {
    pub fn from_command(command: ExecutableCommand) -> Self {
        Executable::GenericCommand(command)
    }

    pub fn from_platform_command(
        windows: Option<ExecutableCommand>,
        unix: Option<ExecutableCommand>,
    ) -> Self {
        Executable::PlatformCommand { windows, unix }
    }

    pub fn from_source(source: Source, assets: HashMap<String, Vec<u8>>) -> Self {
        Executable::Source {
            source,
            executable: None,
            assets,
        }
    }
}

impl ToCommand for Executable {
    fn command(&mut self) -> Result<Command, ExecutableError> {
        match self {
            Executable::GenericCommand(executable) => executable.command(),
            Executable::PlatformCommand { windows, unix } => {
                if cfg!(windows) { windows } else { unix }
                    .as_mut()
                    .ok_or(ExecutableError::UnsupportedPlatform)?
                    .command()
            }
            Executable::Source {
                executable,
                source,
                assets,
            } => {
                if executable.is_none() {
                    // build from source
                    *executable = Some(
                        source
                            .build(assets.clone())
                            .map_err(ExecutableError::BuildFailed)?,
                    );
                }
                // unwrap is safe because we just set it
                executable.as_mut().unwrap().command()
            }
        }
    }
}
