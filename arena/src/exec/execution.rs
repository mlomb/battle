use child_wait_timeout::ChildWT;
use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, ErrorKind, Read},
    process::{Command, Stdio},
    time::{Duration, Instant},
};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Status {
    Exited(i32),
    Timeout,
    IoError(String),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExecutionResult {
    pub status: Status,
    pub stdout: String,
    pub stderr: String,
    pub duration: Duration,
}

pub trait Execute {
    /// Executes the command and waits for it to finish or the timeout to expire.
    fn execute(&mut self, timeout: Duration) -> ExecutionResult;
}

impl Execute for Command {
    fn execute(&mut self, timeout: Duration) -> ExecutionResult {
        let start = Instant::now();

        let mut child = self
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("failed to execute process");

        let status = child.wait_timeout(timeout);
        let duration = start.elapsed();

        let stdout = read_pipe_lines(child.stdout);
        let stderr = read_pipe_lines(child.stderr);

        // Temporal fix!
        let stdout = stdout
            .split("\n")
            .into_iter()
            .collect::<Vec<&str>>()
            .into_iter()
            .filter(|l| !l.starts_with("WARNING:"))
            .collect::<Vec<&str>>()
            .join("\n");

        let status = match status {
            Ok(exit_status) => match exit_status.code() {
                Some(code) => Status::Exited(code),
                None => Status::Exited(-1),
            },
            Err(io_err) if io_err.kind() == ErrorKind::TimedOut => Status::Timeout,
            Err(io_err) => Status::IoError(io_err.to_string()),
        };

        ExecutionResult {
            status,
            stdout,
            stderr,
            duration,
        }
    }
}

fn read_pipe_lines<R: Read>(reader: Option<R>) -> String {
    BufReader::new(reader.expect("to take pipe"))
        .lines()
        .map(|l| l.unwrap_or("(error reading pipe)".to_string()))
        .collect::<Vec<String>>()
        .join("\n")
}
