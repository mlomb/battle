use log::info;
use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, Read},
    process::{Child, Command, Stdio},
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

    // Elsewhere we keep the standard `kill` + `wait`.
    #[cfg(not(windows))]
    {
        let _ = child.kill();
    }

    let _ = child.wait();
}

/// Ties the lifetime of the child process to the current process.
pub fn tie_child_lifetime_to_ours(child: &Child) -> std::io::Result<()> {
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
        job.assign_process(child.as_raw_handle() as isize)
            .map_err(Error::other)?;

        // Intentionally leak the job handle: we want it to stay open for the life of
        // this process so `KILL_ON_JOB_CLOSE` fires only when we actually die (including
        // via `TerminateProcess`). `into_handle` hands us the raw handle and skips the
        // `Drop` that would otherwise close it immediately and kill the child.
        let _ = job.into_handle();
    }

    // On Unix, the child is in our process group by default and will typically
    // receive `SIGHUP`/`SIGTERM` when we die; nothing extra to do here.
    Ok(())
}
