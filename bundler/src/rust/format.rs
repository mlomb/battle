use std::{
    io::Write,
    process::{Command, Stdio},
    vec,
};

pub enum FmtError {
    /// rustfmt is not installed
    RustfmtNotFound,
    /// rustfmt failed to format the code
    Failed(String),
}

/// Format the given code using rustfmt
pub fn format_code<T: ToString>(input: &T) -> Result<String, FmtError> {
    let rustfmt =
        toolchain_find::find_installed_component("rustfmt").ok_or(FmtError::RustfmtNotFound)?;

    let args: Vec<String> = vec!["--edition=2021".to_string()];

    let mut command = Command::new(&rustfmt)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();

    {
        let stdin = command.stdin.as_mut().unwrap();
        stdin.write_all(input.to_string().as_bytes()).unwrap();
    } // drop stdin so it can finish

    let output = command.wait_with_output().unwrap();

    if output.status.success() {
        Ok(String::from_utf8(output.stdout).expect("utf-8").to_string())
    } else {
        Err(FmtError::Failed(
            String::from_utf8(output.stderr).expect("utf-8").to_string(),
        ))
    }
}
