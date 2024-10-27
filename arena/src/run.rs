use child_wait_timeout::ChildWT;
use core::str;
use std::{
    io::{BufRead, BufReader, ErrorKind, Read},
    process::{Command, Stdio},
    time::{Duration, Instant},
};

#[derive(Debug)]
pub struct ExecutionResult {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
    pub duration: Duration,
    pub timed_out: bool,
}

pub fn execute(args: Vec<String>, timeout: Duration) -> ExecutionResult {
    let start = Instant::now();

    let mut child = Command::new(args[0].to_string())
        .args(args.iter().skip(1))
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
        Ok(exit_status) => ExecutionResult {
            exit_code: exit_status.code().expect("could not get exit code"), // signal?
            stdout,
            stderr,
            duration,
            timed_out: false,
        },
        Err(err) => ExecutionResult {
            exit_code: -1,
            stdout,
            stderr,
            duration,
            timed_out: err.kind() == ErrorKind::TimedOut,
        },
    }
}

fn read_pipe<R: Read>(reader: Option<R>) -> String {
    BufReader::new(reader.expect("to take pipe"))
        .lines()
        .map(|l| l.unwrap())
        .collect::<Vec<String>>()
        .join("\n")
}
