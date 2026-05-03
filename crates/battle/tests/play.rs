use assert_cmd::cargo::CommandCargoExt;
use send_ctrlc::InterruptibleCommand;
use std::process::Command;
use std::time::Duration;

mod common;

use common::ExecGuard;

#[test]
fn play_one_olymbits_game() {
    let worker = Command::cargo_bin("battle")
        .expect("cargo_bin battle")
        .args(["worker", "--threads", "1"])
        .spawn_interruptible()
        .expect("spawn battle worker");
    let _guard = ExecGuard(worker);

    std::thread::sleep(Duration::from_millis(500));

    let agents_dir = format!("{}/tests/agents", env!("CARGO_MANIFEST_DIR"));
    let left = format!("{agents_dir}/olymbits_left.cpp");
    let random = format!("{agents_dir}/olymbits_random.cpp");
    let wait = format!("{agents_dir}/olymbits_wait.cpp");

    let output = Command::cargo_bin("battle")
        .expect("cargo_bin battle")
        .args([
            "play",
            "--referee",
            "cg-spring-2024-olympics",
            "-n",
            "1",
            "-a",
            &random,
            "-a",
            &left,
            "-a",
            &wait,
        ])
        .output()
        .expect("battle play");

    assert!(output.status.success(), "olymbits play failed");
}
