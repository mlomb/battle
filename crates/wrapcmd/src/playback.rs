use crate::transcript::{Event, Transcript};
use std::{
    io::{BufRead, Write},
    process::ExitCode,
};

pub fn run_replay(transcript: &Transcript) -> ExitCode {
    let mut stdin = std::io::stdin().lock();
    let mut stdout = std::io::stdout().lock();
    let mut stderr = std::io::stderr().lock();

    for event in &transcript.events {
        match event {
            Event::In(expected) => {
                let mut got = String::new();
                stdin.read_line(&mut got).ok();

                if got.trim_end_matches(['\r', '\n']) != expected {
                    eprintln!("stdin mismatch, expected: {expected}, got: {got}");
                    return ExitCode::FAILURE;
                }
            }
            Event::Out(line) => writeln!(stdout, "{line}").expect("write stdout"),
            Event::Err(line) => writeln!(stderr, "{line}").expect("write stderr"),
        }
    }

    // flush to ensure all output is written before exiting
    let _ = stdout.flush();
    let _ = stderr.flush();

    ExitCode::SUCCESS
}
