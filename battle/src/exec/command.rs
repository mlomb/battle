use std::process::Command;

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
