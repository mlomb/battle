use log::info;
use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, Read},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    thread,
    time::{Duration, Instant},
};

#[cfg(unix)]
use std::os::unix::process::CommandExt;

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
    pub pid: Option<u32>,
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

        // Put the child in its own process group (leader PID == PGID) so `kill(-pid, SIGKILL)`
        // on Unix tears down subprocesses spawned by that child (mirrors `taskkill /T` on Windows).
        // See terminate_child_process_tree()
        #[cfg(unix)]
        {
            self.process_group(0);
        }

        let mut child = match self.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn() {
            Ok(child) => child,
            Err(io_err) => {
                return ExecutionResult {
                    status: Status::IoError(io_err.to_string()),
                    stdout: String::new(),
                    stderr: String::new(),
                    duration: Duration::from_secs(0),
                    pid: None,
                };
            }
        };

        tie_child_lifetime_to_ours(&child).expect("tie child should succeed");

        let deadline = Instant::now() + timeout;
        let status = loop {
            match child.try_wait() {
                Ok(None) => {
                    if Instant::now() >= deadline {
                        terminate_child_process_tree(&mut child);
                        break Status::Timeout;
                    }

                    if abort.is_some_and(|a| a.load(Ordering::Relaxed)) {
                        terminate_child_process_tree(&mut child);
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

        let pid = child.id();
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
            pid: Some(pid),
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

/// Terminates the child process tree.
fn terminate_child_process_tree(child: &mut Child) {
    // On Windows, `Child::kill()` only terminates the direct child. Match runners (e.g. Java
    // referees) often spawn agent subprocesses; those survive `TerminateProcess` on the parent.
    // `taskkill /T` tears down the whole tree.
    #[cfg(windows)]
    {
        let pid = child.id();
        let killed_tree = Command::new("taskkill")
            .args(["/F", "/T", "/PID", &pid.to_string()])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        if !killed_tree {
            let _ = child.kill();
        }
    }

    #[cfg(not(windows))]
    {
        let pid = child.id() as libc::pid_t;
        unsafe {
            // Negative PID signals the whole process group when the leader was spawned with `process_group(0)`.
            libc::kill(-pid, libc::SIGKILL);
        }
        let _ = child.kill();
    }

    let _ = child.wait();
}

/// Ties the lifetime of the child process to the current process.
pub fn tie_child_lifetime_to_ours(_child: &Child) -> std::io::Result<()> {
    // On Windows, a child process is not automatically killed when its parent dies.
    // If our process is killed (e.g. the referee calls `TerminateProcess` on us at the
    // end of a game), the wrapped child would otherwise stay alive and pile up.
    //
    // To prevent that, we put the child in a Job Object with `KILL_ON_JOB_CLOSE`.
    // When our process exits for any reason, the OS closes all our handles, including
    // the job's, which then terminates every process in it.
    #[cfg(windows)]
    {
        use std::io::Error;
        use std::os::windows::io::AsRawHandle;
        use win32job::{ExtendedLimitInfo, Job};

        let mut info = ExtendedLimitInfo::new();
        info.limit_kill_on_job_close();

        let job = Job::create_with_limit_info(&info).map_err(Error::other)?;
        job.assign_process(_child.as_raw_handle() as isize)
            .map_err(Error::other)?;

        // Intentionally leak the job handle: we want it to stay open for the life of
        // this process so `KILL_ON_JOB_CLOSE` fires only when we actually die (including
        // via `TerminateProcess`). `into_handle` hands us the raw handle and skips the
        // `Drop` that would otherwise close it immediately and kill the child.
        let _ = job.into_handle();
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn capture_stdout_from_echo() {
        let mut cmd = Command::new("echo");
        cmd.arg("hello");
        let r = cmd.execute(Duration::from_secs(2), None);
        assert!(matches!(r.status, Status::Exited(0)));
        assert!(r.stdout.contains("hello"), "stdout={:?}", r.stdout);
        assert!(r.stderr.is_empty());
        assert!(r.duration <= Duration::from_secs(2));
    }

    #[test]
    fn capture_error_code() {
        let mut cmd = {
            #[cfg(unix)]
            {
                Command::new("false")
            }
            #[cfg(windows)]
            {
                Command::new("exit").arg("1");
            }
        };
        let r = cmd.execute(Duration::from_secs(2), None);
        assert!(matches!(r.status, Status::Exited(1)));
        assert!(r.stdout.is_empty());
        assert!(r.stderr.is_empty());
        assert!(r.duration <= Duration::from_secs(2));
    }

    #[test]
    fn timeout_kills_long_running() {
        let mut cmd = Command::new("sleep");
        cmd.arg("5");
        let r = cmd.execute(Duration::from_millis(500), None);
        assert!(matches!(r.status, Status::Timeout));
        assert!(r.duration < Duration::from_secs(2));
    }

    /// True if `pid` still exists as a process this user can signal (`kill -0`).
    #[cfg(unix)]
    fn unix_pid_exists(pid: u32) -> bool {
        Command::new("/bin/sh")
            .args(["-c", &format!("kill -0 {pid} 2>/dev/null")])
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }

    #[cfg(unix)]
    #[test]
    fn timeout_child_process_is_gone() {
        let mut cmd = Command::new("sleep");
        cmd.arg("999");
        let r = cmd.execute(Duration::from_millis(500), None);
        assert!(matches!(r.status, Status::Timeout));
        let pid = r.pid.expect("spawn should yield direct child pid");
        assert!(
            !unix_pid_exists(pid),
            "timed-out child pid {pid} should no longer exist"
        );
    }

    /*
    /// Parent binary invokes the child via `system(cmd)` (shell); child prints its PID then sleeps.
    /// Built with `zig c++` via [`crate::builder::compile_cpp_source_to_binary`].
    #[cfg(unix)]
    #[test]
    fn timeout_kills_spawned_descendant() {
        use crate::builder::compile_cpp_source_to_binary;
        use tempfile::tempdir;

        const CHILD_CPP: &str = r#"#include <iostream>
#include <unistd.h>

int main() {
    std::cout << getpid() << std::endl;
    std::cout.flush();
    while (true) {
        sleep(999);
    }
}
"#;

        const PARENT_CPP: &str = r#"#include <cstdlib>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        return 1;
    }
    std::string cmd = std::string("\"") + argv[1] + "\"";
    std::system(cmd.c_str());
    while (true) {
        sleep(9999);
    }
}
"#;

        let dir = tempdir().expect("tempdir");
        let child_path = compile_cpp_source_to_binary(dir.path(), "child.cpp", CHILD_CPP, "child")
            .unwrap_or_else(|e| panic!("compile child: {}", e));
        let parent_path =
            compile_cpp_source_to_binary(dir.path(), "parent.cpp", PARENT_CPP, "parent")
                .unwrap_or_else(|e| panic!("compile parent: {}", e));

        let mut cmd = Command::new(parent_path);
        cmd.arg(child_path.as_os_str());

        let r = cmd.execute(Duration::from_millis(800), None);
        assert!(matches!(r.status, Status::Timeout));
        let descendant: u32 = r
            .stdout
            .lines()
            .next()
            .and_then(|line| line.trim().parse().ok())
            .expect("child process should print its pid to stdout");
        assert!(
            descendant != r.pid.unwrap_or(0),
            "printed pid should differ from direct child pid"
        );
        assert!(
            !unix_pid_exists(descendant),
            "forked child pid {descendant} should be gone after timeout"
        );
        assert!(
            !unix_pid_exists(r.pid.expect("direct child pid")),
            "parent pid should also be gone"
        );
    }
    */
}
