use super::executable::ExecutableError;
use std::process::Command;

/// A trait to generate a command for an executable.
pub trait ToCommand {
    /// Returns a ready-to-use command. Prepares whatever is necessary: build source, copy files, etc.
    ///
    /// # Notes
    ///   - The command must be valid while `self` is not dropped.
    ///   - The execution of the command is expected to be idempotent (albeit randomness).
    fn command(&mut self) -> Result<Command, ExecutableError>;
}

pub trait CommandExt {
    /// Converts the command into a standalone string that can be executed in a shell.
    /// It includes the current directory if available.
    /// It generates different commands for each platform (Windows/Linux).
    fn command_line_string(&self) -> String;
}

impl CommandExt for Command {
    fn command_line_string(&self) -> String {
        if let Some(cwd) = self.get_current_dir() {
            if cfg!(windows) {
                format!("cmd.exe /c cd /d \"{:?}\" && {:?}", cwd, self)
            } else {
                format!("cd \"{:?}\" && {:?}", cwd, self)
            }
        } else {
            format!("{:?}", self)
        }
    }
}
