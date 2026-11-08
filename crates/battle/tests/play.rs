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
            "cg-2024-summer-olympics",
            "-n",
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
        .expect("battle play");

    assert!(
        output.status.success(),
        "olymbits play failed\n--- status: {}\n--- stdout:\n{}\n--- stderr:\n{}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn play_swap_json_rotates_seats() {
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
            "cg-2024-summer-olympics",
            "-n",
            "1",
            "--seed",
            "-5",
            "--swap",
            "--json",
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
        .expect("battle play");

    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success(),
        "{stdout}\n{}",
        String::from_utf8_lossy(&output.stderr)
    );

    let lines: Vec<serde_json::Value> = stdout
        .lines()
        .map(|l| serde_json::from_str(l).expect("JSON line"))
        .collect();
    assert_eq!(lines.len(), 4, "3 games and the summary:\n{stdout}");

    let mut seats: Vec<String> = lines[..3]
        .iter()
        .map(|g| {
            assert_eq!(g["type"], "game");
            assert_eq!(g["seed"], -5);
            assert!(g["error"].is_null(), "{g}");
            g["seats"].to_string()
        })
        .collect();
    seats.sort();
    assert_eq!(seats, ["[0,1,2]", "[1,2,0]", "[2,0,1]"]);

    let summary = &lines[3];
    assert_eq!(summary["type"], "summary");
    assert_eq!(summary["games"], 3);
    assert_eq!(summary["failed"], 0);
    let points: f64 = summary["agents"]
        .as_array()
        .expect("agents")
        .iter()
        .map(|a| a["points"].as_f64().expect("points"))
        .sum();
    assert!(
        (points - 1.5).abs() < 1e-9,
        "points of 3 agents add up to 1.5"
    );
}
