use super::{
    command::ToCommand,
    executable_command::ExecutableCommand,
    source_builder::{BuildError, SourceBuilder},
};
use bundler::source::Source;
use std::process::Command;

/// An executable
///
/// This struct implements the [`ToCommand`] trait, that returns a command that can be executed.
pub enum Executable {
    /// A generic executable that can be run on any platform (e.g. java, python)
    GenericCommand(ExecutableCommand),
    /// An executable that requires a different command for each platform (e.g. main.exe)
    PlatformCommand {
        windows: Option<ExecutableCommand>,
        unix: Option<ExecutableCommand>,
    },
    /// The executable result of compiling source code (e.g. C, Rust)
    Source {
        source: Source,
        /// The compiled executable, lazily initialized when `command` is called.
        executable: Option<ExecutableCommand>,
    },
}

pub enum ExecutableError {
    /// The executable can't run on the current platform
    UnsupportedPlatform,
    /// The source code could not compile
    BuildFailed(BuildError),
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

    pub fn from_source(source: Source) -> Self {
        Executable::Source {
            source,
            executable: None,
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
            Executable::Source { executable, source } => {
                if executable.is_none() {
                    // build from source
                    *executable = Some(source.build().map_err(ExecutableError::BuildFailed)?);
                }
                // unwrap is safe because we just set it
                executable.as_mut().unwrap().command()
            }
        }
    }
}
