use child_wait_timeout::ChildWT;
use core::str;
use std::{
    io::{BufRead, BufReader, ErrorKind, Read},
    process::{Command, Stdio},
    time::{Duration, Instant},
};

#[derive(Debug)]
pub struct ExecutionResult {
    exit_code: i32,
    stdout: String,
    stderr: String,
    duration: Duration,
    timed_out: bool,
}

pub fn execute(args: Vec<&str>, timeout: Duration) -> ExecutionResult {
    let start = Instant::now();

    let mut child = Command::new(args[0])
        .args(args.iter().skip(1))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("failed to execute process");

    let status = child.wait_timeout(timeout);
    let duration = start.elapsed();

    let stdout = read_pipe(child.stdout);
    let stderr = read_pipe(child.stderr);

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
    BufReader::new(reader.expect("Internal error, could not take pipe"))
        .lines()
        .map(|l| l.unwrap())
        .collect::<Vec<String>>()
        .join("\n")
}
