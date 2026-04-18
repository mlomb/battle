use std::ffi::OsString;
use std::io::{self, BufRead, BufReader, Write};
use std::process::{Command, ExitCode, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;

use crate::transcript::{Event, Transcript};

type Log = Arc<Mutex<Transcript>>;

fn record(log: &Log, event: Event, passthrough: &mut impl Write) -> io::Result<()> {
    let content = event.content().to_owned();
    log.lock()
        .map_err(|e| io::Error::new(io::ErrorKind::Other, e.to_string()))?
        .events
        .push(event);
    writeln!(passthrough, "{content}")?;
    passthrough.flush()
}

fn forward<R: io::Read, W: io::Write>(
    log: Log,
    to_event: fn(String) -> Event,
    reader: R,
    mut writer: W,
) -> io::Result<()> {
    let mut r = BufReader::new(reader);
    let mut line = String::new();
    loop {
        line.clear();
        match r.read_line(&mut line) {
            Ok(0) => break,
            Ok(_) => {
                let content = line.trim_end_matches(['\n', '\r']).to_owned();
                record(&log, to_event(content), &mut writer)?;
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn forward_stdin(log: Log, mut child_in: std::process::ChildStdin) -> io::Result<()> {
    let stdin = io::stdin();
    let mut r = stdin.lock();
    let mut line = String::new();
    loop {
        line.clear();
        match r.read_line(&mut line) {
            Ok(0) => break,
            Ok(_) => {
                let content = line
                    .trim_end_matches('\n')
                    .trim_end_matches('\r')
                    .to_owned();
                log.lock()
                    .map_err(|e| io::Error::new(io::ErrorKind::Other, e.to_string()))?
                    .events
                    .push(Event::In(content));
                child_in.write_all(line.as_bytes())?;
                child_in.flush()?;
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

pub fn run_capture(cmd: &[OsString]) -> (Transcript, ExitCode) {
    let cmd = if cmd.first().is_some_and(|a| a == "--") {
        &cmd[1..]
    } else {
        cmd
    };
    if cmd.is_empty() {
        eprintln!(
            "wrapcmd capture: missing command (use: wrapcmd capture <out> -- <cmd> [args...])"
        );
        return (Transcript::default(), ExitCode::from(1));
    }

    let log: Log = Arc::new(Mutex::new(Transcript::default()));

    let mut child = match Command::new(&cmd[0])
        .args(&cmd[1..])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            eprintln!("wrapcmd capture: spawn: {e}");
            return (Transcript::default(), ExitCode::from(1));
        }
    };

    let child_in = child.stdin.take().expect("stdin");
    let child_out = child.stdout.take().expect("stdout");
    let child_err = child.stderr.take().expect("stderr");

    // Stdin thread is not joined: it may block waiting for TTY EOF after the child exits.
    let log_in = Arc::clone(&log);
    let _stdin = thread::spawn(move || forward_stdin(log_in, child_in));

    let log_out = Arc::clone(&log);
    let h_out = thread::spawn(move || {
        let stdout = io::stdout();
        forward(log_out, Event::Out, child_out, stdout.lock())
    });

    let log_err = Arc::clone(&log);
    let h_err = thread::spawn(move || {
        let stderr = io::stderr();
        forward(log_err, Event::Err, child_err, stderr.lock())
    });

    let r_out = h_out
        .join()
        .unwrap_or_else(|_| Err(io::Error::new(io::ErrorKind::Other, "thread panic")));
    let r_err = h_err
        .join()
        .unwrap_or_else(|_| Err(io::Error::new(io::ErrorKind::Other, "thread panic")));

    if r_out.is_err() || r_err.is_err() {
        if let Err(e) = r_out {
            eprintln!("wrapcmd capture: stdout: {e}");
        }
        if let Err(e) = r_err {
            eprintln!("wrapcmd capture: stderr: {e}");
        }
        let _ = child.kill();
        let _ = child.wait();
        let transcript = log.lock().unwrap().clone();
        return (transcript, ExitCode::from(1));
    }

    let transcript = log.lock().unwrap().clone();
    let code = match child.wait() {
        Ok(st) => ExitCode::from(st.code().unwrap_or(1) as u8),
        Err(e) => {
            eprintln!("wrapcmd capture: wait: {e}");
            ExitCode::from(1)
        }
    };

    (transcript, code)
}
