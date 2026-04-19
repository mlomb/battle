use std::process::Command;

pub trait CommandExt {
    /// Converts the command into a standalone string that can be executed in a shell.
    /// It includes the current directory if available.
    /// It generates different commands for each platform (Windows/Linux).
    fn command_line_string(&self) -> String;
}

impl CommandExt for Command {
    fn command_line_string(&self) -> String {
        let program = self.get_program().to_string_lossy();
        let args: Vec<_> = self.get_args().map(|a| a.to_string_lossy()).collect();

        let parts: Vec<&str> = std::iter::once(program.as_ref())
            .chain(args.iter().map(|a| a.as_ref()))
            .collect();

        // Must NOT quote individual tokens. The CodinGame referee (built with
        // Apache Commons CLI ≥ 1.9) calls commandLine.split(" ") and passes the
        // resulting array directly to Runtime.exec(). Commons CLI's
        // stripLeadingAndTrailingQuotesDefaultOn strips outer quotes only when
        // the entire value is a single quoted token (no spaces). A multi-token
        // wrapped command like `"battle" "wrap" ...` has internal spaces, so
        // the outer quotes are NOT stripped, and the first split token becomes
        // `"battle"` (with literal quote chars) which exec cannot find on disk.
        // Producing a plain space-joined string avoids this entirely.
        let cmd = parts.join(" ");

        if let Some(cwd) = self.get_current_dir() {
            let cwd = cwd.to_string_lossy();
            if cfg!(windows) {
                format!("cmd.exe /c cd /d \"{}\" && {}", cwd, cmd)
            } else {
                format!("cd \"{}\" && {}", cwd, cmd)
            }
        } else {
            cmd
        }
    }
}
