use assert_cmd::Command;
use std::time::Duration;

mod common;

use crate::common::BattleWorker;

#[test]
fn referee_diff_matches_identical_referees_one_olymbits_game() {
    let _worker = BattleWorker::spawn();

    let agents_dir = format!("{}/tests/agents", env!("CARGO_MANIFEST_DIR"));
    let left = format!("{agents_dir}/olymbits_left.cpp");
    let random = format!("{agents_dir}/olymbits_random.cpp");
    let wait = format!("{agents_dir}/olymbits_wait.cpp");

    let olympics = "cg-spring-2024-olympics";

    let output = Command::cargo_bin("battle")
        .expect("cargo_bin battle")
        .args([
            "referee-diff",
            "--reference",
            olympics,
            "--candidate",
            olympics,
            "--max-games",
            "1",
            "-a",
            &random,
            "-a",
            &left,
            "-a",
            &wait,
        ])
        .timeout(Duration::from_secs(30))
        .output()
        .expect("battle referee-diff");

    assert!(
        output.status.success(),
        "2x reference olymbits referee-diff failed"
    );
}
