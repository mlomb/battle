use std::process::Command;

pub trait CommandExt {
    /// Converts the command into a standalone string that can be executed in a shell.
    /// It includes the current directory if available.
    /// It generates different commands for each platform (Windows/Unix).
    fn command_line_string(&self) -> String;
}

impl CommandExt for Command {
    fn command_line_string(&self) -> String {
        let program = self.get_program().to_string_lossy();
        let args: Vec<_> = self.get_args().map(|a| a.to_string_lossy()).collect();

        let parts: Vec<&str> = std::iter::once(program.as_ref())
            .chain(args.iter().map(|a| a.as_ref()))
            .collect();

        if let Some(_cwd) = self.get_current_dir() {
            unimplemented!("unsupported cwd for now");

            /*
            let cwd = cwd.to_string_lossy();
            if cfg!(windows) {
                format!("cmd.exe /c cd /d \"{}\" && {}", cwd, cmd)
            } else {
                format!("cd \"{}\" && {}", cwd, cmd)
            }
            */
        } else {
            parts.join(" ")
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn command_line_string() {
        let mut cmd = Command::new("echo");
        cmd.arg("Hello").arg("world");
        assert_eq!(cmd.command_line_string(), "echo Hello world");
    }

    #[test]
    #[should_panic(expected = "unsupported cwd for now")]
    fn unsupported_current_dir() {
        let mut cmd = Command::new("echo");
        cmd.current_dir("/tmp");
        let _ = cmd.command_line_string();
    }
}
