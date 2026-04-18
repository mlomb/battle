//! Integration tests for `wrapcmd capture`.

use std::fs;
use std::io::Write;
use std::process::{Command, Stdio};

use assert_cmd::prelude::*;
use tempfile::tempdir;
use wrapcmd::transcript::{Event, Transcript};

#[test]
fn capture_stdin_stdout_stderr() {
    let dir = tempdir().expect("tempdir");
    let transcript_path = dir.path().join("t.io");

    let dummy = Command::cargo_bin("dummy")
        .expect("cargo_bin dummy")
        .get_program()
        .to_owned();

    let mut child = Command::cargo_bin("wrapcmd")
        .expect("cargo_bin wrapcmd")
        .arg("capture")
        .arg(&transcript_path)
        .arg(dummy)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn");

    child
        .stdin
        .as_mut()
        .expect("stdin")
        .write_all(b"a\n")
        .expect("stdin write");

    drop(child.stdin.take());
    let output = child.wait_with_output().expect("wait_with_output");
    assert!(output.status.success());

    assert_eq!(
        output.stdout, b"hello from stdout\na\ngoodbye from stdout\n",
        "stdout passthrough mismatch"
    );
    assert_eq!(
        output.stderr, b"hello from stderr\na\ngoodbye from stderr\n",
        "stderr passthrough mismatch"
    );

    let text = fs::read_to_string(&transcript_path).expect("read transcript");

    assert!(text.contains("< a\n"), "stdin chunk");

    let t: Transcript = text.parse().expect("parse transcript");

    assert_eq!(t.stdin(), "a\n");
    assert_eq!(t.stdout(), "hello from stdout\na\ngoodbye from stdout\n");
    assert_eq!(t.stderr(), "hello from stderr\na\ngoodbye from stderr\n");

    // events are typed — spot-check a few
    assert!(t.events.iter().any(|e| e == &Event::In("a".into())));
    assert!(t
        .events
        .iter()
        .any(|e| e == &Event::Out("hello from stdout".into())));
    assert!(t
        .events
        .iter()
        .any(|e| e == &Event::Err("hello from stderr".into())));
}
