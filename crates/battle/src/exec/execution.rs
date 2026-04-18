use log::info;
use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, Read},
    process::{Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    thread,
    time::{Duration, Instant},
};

/// Interval at which we poll the child process for status updates.
const POLL_INTERVAL: Duration = Duration::from_millis(100);

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Status {
    /// The process exited with the given code
    Exited(i32),
    /// The process timed out and was killed
    Timeout,
    /// The process was killed because the `abort` flag was set
    Cancelled,
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
    fn execute(&mut self, timeout: Duration, abort: Option<&AtomicBool>) -> ExecutionResult;
}

impl Execute for Command {
    fn execute(&mut self, timeout: Duration, abort: Option<&AtomicBool>) -> ExecutionResult {
        info!("Executing command: {:?}", self);

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

        let deadline = Instant::now() + timeout;
        let status = loop {
            match child.try_wait() {
                Ok(None) => {
                    if Instant::now() >= deadline {
                        child.kill().ok();
                        child.wait().ok();
                        break Status::Timeout;
                    }

                    if abort.is_some_and(|a| a.load(Ordering::Relaxed)) {
                        child.kill().ok();
                        child.wait().ok();
                        break Status::Cancelled;
                    }

                    thread::sleep(POLL_INTERVAL);
                }
                Ok(Some(exit_status)) => {
                    break match exit_status.code() {
                        Some(c) => Status::Exited(c),
                        None => Status::Exited(-1),
                    };
                }
                Err(e) => break Status::IoError(e.to_string()),
            }
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
