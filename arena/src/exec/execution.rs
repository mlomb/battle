use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, Read},
    process::{Command, Stdio},
    time::{Duration, Instant},
};
use wait_timeout::ChildExt;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Status {
    /// The process exited with the given code
    Exited(i32),
    /// The process timed out and was killed
    Timeout,
    /// An I/O error occurred while waiting for the process to finish
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
    ///
    /// We do not return `Result` because we want to capture stdio in case of errors.
    fn execute(&mut self, timeout: Duration) -> ExecutionResult;
}

impl Execute for Command {
    fn execute(&mut self, timeout: Duration) -> ExecutionResult {
        let start = Instant::now();

        let mut child = match self.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn() {
            Ok(child) => child,
            Err(io_err) => {
                return ExecutionResult {
                    status: Status::IoError(io_err.to_string()),
                    stdout: String::new(),
                    stderr: String::new(),
                    duration: Duration::from_secs(0),
                };
            }
        };

        let status = match child.wait_timeout(timeout) {
            Ok(Some(exit_status)) => match exit_status.code() {
                Some(code) => Status::Exited(code),
                None => Status::Exited(-1),
            },
            Ok(None) => {
                child.kill().ok();
                child.wait().ok();
                Status::Timeout
            }
            Err(io_err) => Status::IoError(io_err.to_string()),
        };
        
        let duration = start.elapsed();

        let stdout = read_pipe_lines(child.stdout);
        let stderr = read_pipe_lines(child.stderr);

        // Temporal fix!
        let stdout = stdout
            .split("\n")
            .collect::<Vec<&str>>()
            .into_iter()
            .filter(|l| !l.starts_with("WARNING:"))
            .collect::<Vec<&str>>()
            .join("\n");

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
