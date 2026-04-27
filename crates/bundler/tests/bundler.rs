use std::path::{Path, PathBuf};
use std::process::Command;

use assert_cmd::cargo_bin;

fn case(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("test_cases")
        .join(name)
}

fn bundle_stdout(entry: impl AsRef<Path>) -> String {
    let output = Command::new(cargo_bin!("bundler"))
        .arg(entry.as_ref())
        .output()
        .expect("run bundler");
    assert!(output.status.success());
    String::from_utf8(output.stdout).expect("stdout utf-8")
}

#[test]
fn cpp_matches_test_case_tree() {
    let code = bundle_stdout(case("cpp"));

    assert!(code.contains("struct Point"));
    assert_eq!(code.matches("struct Point").count(), 1);
    assert!(!code.contains("#pragma once"));
    assert_eq!(code.matches("already included").count(), 2);
}

#[test]
fn rust_main_matches_test_case_tree() {
    let code = bundle_stdout(case("rust_main"));

    assert!(!code.contains("mod point;"));
    assert!(!code.contains("mod submod;"));
    assert!(code.contains("struct Point"));
    assert!(code.contains("struct TestStruct"));
    assert!(!code.contains("#[test]"));
    assert!(!code.contains("#[cfg(test)]"));
    assert!(!code.contains("#[doc"));
}

#[test]
fn rust_bin_matches_test_case_tree() {
    let code = bundle_stdout(case("rust_bin"));

    assert!(code.contains("fn hello"));
    assert!(code.contains("fn main"));
    assert!(code.contains("HELLO"));
}
