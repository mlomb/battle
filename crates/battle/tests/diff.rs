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
    let up = format!("{agents_dir}/olymbits_up.cpp");

    let olympics = "cg-2024-summer-olympics-ref";

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
            "--turn-timeout",
            "1000",
            "-a",
            &random,
            "-a",
            &left,
            "-a",
            &up,
        ])
        .timeout(Duration::from_secs(180))
        .output()
        .expect("battle referee-diff");

    assert!(
        output.status.success(),
        "2x reference olymbits referee-diff failed\n--- status: {}\n--- stdout:\n{}\n--- stderr:\n{}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}
