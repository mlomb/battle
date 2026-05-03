use assert_cmd::Command;
use std::time::Duration;

mod common;

use crate::common::BattleWorker;

#[test]
fn play_one_olymbits_game() {
    let _worker = BattleWorker::spawn();

    let agents_dir = format!("{}/tests/agents", env!("CARGO_MANIFEST_DIR"));
    let left = format!("{agents_dir}/olymbits_left.cpp");
    let random = format!("{agents_dir}/olymbits_random.cpp");
    let up = format!("{agents_dir}/olymbits_up.cpp");

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
            &up,
        ])
        .timeout(Duration::from_secs(10))
        .output()
        .expect("battle play");

    assert!(output.status.success(), "olymbits play failed");
}
