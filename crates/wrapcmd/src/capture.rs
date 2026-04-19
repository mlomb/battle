use std::ffi::OsString;
use std::fs::File;
use std::io::{self, BufRead, BufReader, BufWriter, Write};
use std::path::Path;
use std::process::{ChildStdin, Command, ExitCode, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;

use crate::transcript::Event;

type FileLog = Arc<Mutex<BufWriter<File>>>;

fn write_event(file_log: &FileLog, event: &Event) -> io::Result<()> {
    let mut f = file_log.lock().expect("lock file log");
    writeln!(f, "{event}")?;

    // It is important to flush every time, because we don't know if the parent process
    // may kills us at any time.
    // For example, at the end of a game, the referee kills all agent processes.
    f.flush()
}

fn forward_out<R: io::Read, W: io::Write>(
    file_log: FileLog,
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
                let event = to_event(content);

                // write to disk
                write_event(&file_log, &event)?;

                // passthrough to the parent process
                writeln!(writer, "{}", event.content())?;
                writer.flush()?;
            }
            Err(e) => return Err(e),
        }
    }

    Ok(())
}

fn forward_stdin(file_log: FileLog, mut child_in: ChildStdin) -> io::Result<()> {
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
                let event = Event::In(content);
                write_event(&file_log, &event)?;
                child_in.write_all(line.as_bytes())?;
                child_in.flush()?;
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

pub fn run_capture(cmd: &[OsString], out_path: &Path) -> ExitCode {
    let cmd = if cmd.first().is_some_and(|a| a == "--") {
        &cmd[1..]
    } else {
        cmd
    };
    assert!(!cmd.is_empty(), "missing command");

    let file = File::create(out_path).expect("create transcript file");
    let file_log: FileLog = Arc::new(Mutex::new(BufWriter::new(file)));

    let mut child = Command::new(&cmd[0])
        .args(&cmd[1..])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn process");

    let child_in = child.stdin.take().expect("stdin");
    let child_out = child.stdout.take().expect("stdout");
    let child_err = child.stderr.take().expect("stderr");

    // Stdin thread is not joined: it may block waiting for TTY EOF after the child exits.
    let file_log_in = Arc::clone(&file_log);
    let _stdin = thread::spawn(move || forward_stdin(file_log_in, child_in));

    let file_log_out = Arc::clone(&file_log);
    let h_out = thread::spawn(move || {
        let stdout = io::stdout();
        forward_out(file_log_out, Event::Out, child_out, stdout.lock())
    });

    let file_log_err = Arc::clone(&file_log);
    let h_err = thread::spawn(move || {
        let stderr = io::stderr();
        forward_out(file_log_err, Event::Err, child_err, stderr.lock())
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
        return ExitCode::from(1);
    }

    match child.wait() {
        Ok(st) => ExitCode::from(st.code().unwrap_or(1) as u8),
        Err(e) => {
            eprintln!("wrapcmd capture: wait: {e}");
            ExitCode::from(1)
        }
    }
}
