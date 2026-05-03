use std::io::Write;
use std::process::{Command, Stdio};

use assert_cmd::prelude::*;
use tempfile::tempdir;
use wrapcmd::{Event, Transcript};

fn save_transcript(events: Vec<Event>) -> (tempfile::TempDir, std::path::PathBuf) {
    let dir = tempdir().expect("tempdir");
    let path = dir.path().join("t.io");
    Transcript { events }.save(&path).expect("save");
    (dir, path)
}

fn run_playback(path: &std::path::Path, stdin: &[u8]) -> std::process::Output {
    let mut child = Command::cargo_bin("wrapcmd")
        .expect("cargo_bin wrapcmd")
        .arg("playback")
        .arg(path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn");

    child
        .stdin
        .as_mut()
        .unwrap()
        .write_all(stdin)
        .expect("stdin write");
    drop(child.stdin.take());
    child.wait_with_output().expect("wait_with_output")
}

/// Matching stdin → exit 0, stdout/stderr replayed in order.
#[test]
fn playback_matching_stdin() {
    let (_dir, path) = save_transcript(vec![
        Event::In("hello".into()),
        Event::Out("world".into()),
        Event::Err("oops".into()),
        Event::In("bye".into()),
        Event::Out("goodbye".into()),
    ]);

    let output = run_playback(&path, b"hello\nbye\n");

    assert!(output.status.success(), "expected exit 0");
    assert_eq!(output.stdout, b"world\ngoodbye\n");
    assert_eq!(output.stderr, b"oops\n");
}

/// Wrong stdin on first In event → non-zero exit with mismatch message.
#[test]
fn playback_stdin_mismatch() {
    let (_dir, path) = save_transcript(vec![Event::In("hello".into()), Event::Out("world".into())]);

    let output = run_playback(&path, b"wrong\n");

    assert!(!output.status.success(), "expected non-zero exit");
    assert!(
        String::from_utf8_lossy(&output.stderr).contains("mismatch"),
        "expected mismatch message"
    );
}

/// Transcript with no In events — stdin is ignored, stdout/stderr replayed.
#[test]
fn playback_no_stdin_events() {
    let (_dir, path) = save_transcript(vec![
        Event::Out("line one".into()),
        Event::Err("err one".into()),
        Event::Out("line two".into()),
    ]);

    let output = Command::cargo_bin("wrapcmd")
        .expect("cargo_bin wrapcmd")
        .arg("playback")
        .arg(&path)
        .stdin(Stdio::null())
        .output()
        .expect("output");

    assert!(output.status.success());
    assert_eq!(output.stdout, b"line one\nline two\n");
    assert_eq!(output.stderr, b"err one\n");
}

/// Empty transcript → exit 0, no output.
#[test]
fn playback_empty_transcript() {
    let (_dir, path) = save_transcript(vec![]);

    let output = run_playback(&path, b"");

    assert!(output.status.success());
    assert!(output.stdout.is_empty());
    assert!(output.stderr.is_empty());
}
