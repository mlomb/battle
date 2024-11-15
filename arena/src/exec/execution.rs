use child_wait_timeout::ChildWT;
use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, Read},
    process::{Command, Stdio},
    time::{Duration, Instant},
};

pub trait Execute {
    fn execute(&mut self, timeout: Duration) -> Result<ExecutionResult, ExecutionError>;
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExecutionResult {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
    pub duration: Duration,
    pub timed_out: bool,
}

pub enum ExecutionError {
    // -
}

impl Execute for Command {
    fn execute(&mut self, timeout: Duration) -> Result<ExecutionResult, ExecutionError> {
        let start = Instant::now();

        let mut child = self
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("failed to execute process");

        let status = child.wait_timeout(timeout);
        let duration = start.elapsed();

        let stdout = read_pipe(child.stdout);
        let stderr = read_pipe(child.stderr);

        // Temporal fix!
        let stdout = stdout
            .split("\n")
            .into_iter()
            .collect::<Vec<&str>>()
            .into_iter()
            .filter(|l| !l.starts_with("WARNING:"))
            .collect::<Vec<&str>>()
            .join("\n");

        match status {
            Ok(exit_status) => Ok(ExecutionResult {
                exit_code: exit_status.code().unwrap_or(-1),
                stdout,
                stderr,
                duration,
                timed_out: false,
            }),
            Err(err) => Ok(ExecutionResult {
                exit_code: -1,
                stdout,
                stderr,
                duration,
                timed_out: err.kind() == std::io::ErrorKind::TimedOut,
            }),
        }
    }
}

fn read_pipe<R: Read>(reader: Option<R>) -> String {
    BufReader::new(reader.expect("to take pipe"))
        .lines()
        .map(|l| l.unwrap())
        .collect::<Vec<String>>()
        .join("\n")
}
