use std::cmp::Ordering;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;

use clap::Args;
use console::style;
use log::{error, info};
use serde::Serialize;

use crate::exec::Target;
use crate::game::{GameAgentResult, GameResult, GameResultData, GameSetup};
use crate::network::client_node::{GameChannel, NetworkArgs};
use crate::referee::Referee;

#[derive(Debug, Args)]
pub struct PlayArgs {
    /// Number of seeds to play
    #[arg(short, long, default_value_t = 1)]
    pub n: usize,

    /// First seed
    #[arg(long, default_value_t = 1, allow_negative_numbers = true)]
    pub seed: i64,

    /// Play every seed once per seat order
    #[arg(long)]
    pub swap: bool,

    /// Only print the summary
    #[arg(short, long)]
    pub quiet: bool,

    /// Print JSON lines instead of text
    #[arg(long)]
    pub json: bool,

    /// Also write the JSON lines to this file
    #[arg(short, long)]
    pub output: Option<PathBuf>,

    /// Save the game data the referee writes (its log) of every game in this folder
    #[arg(long)]
    pub game_data_dir: Option<PathBuf>,
}

pub fn format_agent_scores(agents: &[GameAgentResult]) -> String {
    agents
        .iter()
        .enumerate()
        .map(|(i, a)| agent_style(i, format_score(a.score)).to_string())
        .collect::<Vec<_>>()
        .join(" ")
}

fn format_score(score: Option<i32>) -> String {
    score.map_or("-".to_string(), |s| s.to_string())
}

fn agent_style<D>(i: usize, d: D) -> console::StyledObject<D> {
    match i {
        0 => style(d).cyan(),
        1 => style(d).magenta(),
        2 => style(d).yellow(),
        3 => style(d).green(),
        _ => style(d).white(),
    }
}

/// Plays the games on the worker pool, calling `on_result` as each one finishes.
pub async fn run_games(
    setups: Vec<GameSetup>,
    network_args: NetworkArgs,
    interrupt_on_ctrl_c: bool,
    mut on_result: impl FnMut(GameSetup, GameResult),
) {
    let total = setups.len();
    let mut pending = setups.into_iter().peekable();
    let mut received = 0;
    let mut game_channel = GameChannel::new(network_args);

    while received < total {
        // select! creates the future of a disabled branch too, so take the game inside it
        let next = pending.peek().cloned();
        let tx = &game_channel.tx;
        let send = async move {
            match next {
                Some(game) => tx.send(game).await,
                None => std::future::pending().await,
            }
        };

        tokio::select! {
            biased; // prioritize receive over send

            _ = tokio::signal::ctrl_c(), if interrupt_on_ctrl_c => {
                info!("Received Ctrl+C, stopping...");
                break;
            }

            item = game_channel.rx.recv() => match item {
                Some((setup, result)) => {
                    received += 1;
                    on_result(setup, result);
                }
                None => break,
            },

            _ = send, if pending.peek().is_some() => {
                pending.next();
            },
        }
    }
}

/// Plays `n` games on sequential seeds; collects each [`GameResultData`] that could be played.
pub async fn play_games(
    referee: Referee,
    agents: Vec<Arc<Target>>,
    n: usize,
    first_seed: i64,
    network_args: NetworkArgs,
    interrupt_on_ctrl_c: bool,
    capture_io: bool,
) -> Vec<(GameSetup, GameResultData)> {
    let setups = (first_seed..first_seed + n as i64)
        .map(|seed| GameSetup {
            referee: referee.clone(),
            agents: agents.clone(),
            seed,
            capture_io,
            capture_game_data: false,
        })
        .collect();

    let mut out = Vec::new();
    run_games(
        setups,
        network_args,
        interrupt_on_ctrl_c,
        |setup, result| {
            let count = out.len() + 1;
            match result {
                Ok(data) => {
                    let scores = format_agent_scores(&data.agents);
                    info!("#{count} {scores} - seed {}", setup.seed);
                    out.push((setup, data));
                }
                Err(err) => error!("#{count} failed - seed {}: {err}", setup.seed),
            }
        },
    )
    .await;
    out
}

#[derive(Serialize)]
#[serde(tag = "type", rename_all = "lowercase")]
enum JsonLine<'a> {
    Game(&'a GameRecord),
    Summary(&'a Summary),
}

#[derive(Serialize)]
struct GameRecord {
    seed: i64,
    seats: Vec<usize>,
    error: Option<String>,
    scores: Vec<Option<i32>>,
    points: Option<Vec<f64>>,
    timeouts: Vec<Option<u32>>,
    end_reason: Option<String>,
    duration_ms: Option<u64>,
    game_data: Option<PathBuf>,
}

#[derive(Serialize)]
struct Summary {
    games: usize,
    failed: usize,
    missing: usize,
    agents: Vec<AgentSummary>,
}

#[derive(Serialize)]
struct AgentSummary {
    name: String,
    path: String,
    wins: usize,
    draws: usize,
    losses: usize,
    points: Option<f64>,
    seeds_won: usize,
    seeds_drawn: usize,
    seeds_lost: usize,
    timeouts: usize,
    avg_score: Option<f64>,
}

struct Output {
    json: bool,
    file: Option<BufWriter<File>>,
}

impl Output {
    fn emit(&mut self, line: JsonLine, text: Option<String>) {
        let json = serde_json::to_string(&line).expect("serializable");
        if self.json {
            println!("{json}");
        } else if let Some(text) = text {
            println!("{text}");
        }
        if let Some(file) = &mut self.file
            && let Err(err) = writeln!(file, "{json}")
        {
            error!("Cannot write the output file: {err}");
        }
    }
}

pub async fn play_main(
    referee: Referee,
    paths: &[String],
    agents: Vec<Arc<Target>>,
    args: PlayArgs,
    network_args: NetworkArgs,
) -> ExitCode {
    if args.quiet {
        log::set_max_level(log::LevelFilter::Warn);
    }

    let names = agent_names(paths);
    let n = agents.len();

    if let Some(dir) = &args.game_data_dir
        && let Err(err) = std::fs::create_dir_all(dir)
    {
        return fail("Cannot create", format!("{}: {err}", dir.display()));
    }
    let file = match args.output.as_ref().map(File::create).transpose() {
        Ok(file) => file.map(BufWriter::new),
        Err(err) => return fail("Cannot create the output file", err.to_string()),
    };
    let mut output = Output {
        json: args.json,
        file,
    };

    // in rotation r, seat s is taken by agent (s + r) % n
    let rotations = if args.swap { n } else { 1 };
    let mut setups = Vec::new();
    for seed in args.seed..args.seed + args.n as i64 {
        for r in 0..rotations {
            setups.push(GameSetup {
                referee: referee.clone(),
                agents: (0..n).map(|s| agents[(s + r) % n].clone()).collect(),
                seed,
                capture_io: false,
                capture_game_data: true,
            });
        }
    }
    let total = setups.len();

    let mut records = Vec::new();
    run_games(setups, network_args, true, |setup, result| {
        // the agents are distinct Arcs (even for the same path), which tells the rotation
        let r = (0..n)
            .find(|&r| Arc::ptr_eq(&setup.agents[0], &agents[r]))
            .expect("the agents of the game");
        let record = record(&setup, r, result, args.game_data_dir.as_deref());
        let text = (!args.quiet).then(|| format_game(records.len() + 1, &record, &names));
        output.emit(JsonLine::Game(&record), text);
        records.push(record);
    })
    .await;

    let summary = summarize(&records, &names, paths, total);
    let text = format_summary(&summary, &records, &names);
    output.emit(JsonLine::Summary(&summary), Some(text));
    if let Some(mut file) = output.file
        && let Err(err) = file.flush()
    {
        error!("Cannot write the output file: {err}");
    }

    if summary.games > summary.failed {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

fn fail(what: &str, err: impl std::fmt::Display) -> ExitCode {
    eprintln!("{} {err}", style(what).red().bold());
    ExitCode::FAILURE
}

fn record(setup: &GameSetup, r: usize, result: GameResult, dir: Option<&Path>) -> GameRecord {
    let n = setup.agents.len();
    let seats: Vec<usize> = (0..n).map(|a| (a + n - r) % n).collect();
    let unplayed = GameRecord {
        seed: setup.seed,
        seats: seats.clone(),
        error: None,
        scores: vec![None; n],
        points: None,
        timeouts: vec![None; n],
        end_reason: None,
        duration_ms: None,
        game_data: None,
    };
    let data = match result {
        Ok(data) => data,
        Err(err) => {
            return GameRecord {
                error: Some(err),
                ..unplayed
            };
        }
    };

    let agent = |a: usize| &data.agents[seats[a]];
    let scores: Vec<Option<i32>> = (0..n).map(|a| agent(a).score).collect();
    let points = match data.failure {
        None if n >= 2 => scores.iter().copied().collect::<Option<Vec<_>>>(),
        _ => None,
    };
    let game_data = dir.zip(data.game_data.as_ref()).and_then(|(dir, content)| {
        let path = dir.join(format!("seed{}-r{r}.log", setup.seed));
        std::fs::write(&path, content)
            .inspect_err(|err| error!("Cannot write {}: {err}", path.display()))
            .ok()
            .map(|_| path)
    });

    GameRecord {
        error: data.failure,
        points: points.map(|scores| self::points(&scores)),
        timeouts: (0..n).map(|a| agent(a).timeout).collect(),
        end_reason: data.end_reason,
        duration_ms: Some(data.exec_res.duration.as_millis() as u64),
        game_data,
        scores,
        ..unplayed
    }
}

fn by_seat(record: &GameRecord) -> Vec<usize> {
    let mut agents: Vec<usize> = (0..record.seats.len()).collect();
    agents.sort_by_key(|&a| record.seats[a]);
    agents
}

fn format_game(count: usize, record: &GameRecord, names: &[String]) -> String {
    let by_seat = by_seat(record);
    let mut line = format!("#{count:<4} seed {:<6}", record.seed);
    for &a in &by_seat {
        let score = format_score(record.scores[a]);
        line += &format!(" {}", agent_style(a, format!("{}:{score}", names[a])));
    }
    let timed_out: Vec<String> = by_seat
        .iter()
        .filter_map(|&a| Some(format!("{} T{}", names[a], record.timeouts[a]?)))
        .collect();
    if !timed_out.is_empty() {
        line += &format!("  {} {}", style("timeout").yellow(), timed_out.join(", "));
    }
    if let Some(err) = &record.error {
        line += &format!("  {} {err}", style("FAILED").red().bold());
    }
    line
}

fn agent_names(paths: &[String]) -> Vec<String> {
    let stems: Vec<String> = paths
        .iter()
        .map(|p| match Path::new(p).file_stem() {
            Some(stem) => stem.to_string_lossy().to_string(),
            None => p.clone(),
        })
        .collect();
    stems
        .iter()
        .enumerate()
        .map(|(i, s)| match stems.iter().filter(|o| *o == s).count() {
            1 => s.clone(),
            _ => format!("{s}#{}", stems[..=i].iter().filter(|o| *o == s).count()),
        })
        .collect()
}

fn points(scores: &[i32]) -> Vec<f64> {
    let others = (scores.len() - 1) as f64;
    scores
        .iter()
        .map(|&s| {
            let beaten = scores.iter().filter(|&&o| s > o).count();
            let tied = scores.iter().filter(|&&o| s == o).count() - 1;
            (beaten as f64 + tied as f64 / 2.0) / others
        })
        .collect()
}

fn summarize(records: &[GameRecord], names: &[String], paths: &[String], total: usize) -> Summary {
    let ok: Vec<&GameRecord> = records.iter().filter(|r| r.error.is_none()).collect();

    let mut by_seed: BTreeMap<i64, Vec<Vec<f64>>> = BTreeMap::new();
    for r in &ok {
        if let Some(points) = &r.points {
            by_seed.entry(r.seed).or_default().push(points.clone());
        }
    }

    let agents = (0..names.len())
        .map(|a| {
            let score = |r: &GameRecord| r.scores[a].expect("succeeded");
            let best_other = |r: &GameRecord| {
                (0..names.len())
                    .filter(|&o| o != a)
                    .map(|o| r.scores[o].expect("succeeded"))
                    .max()
            };
            let (mut wins, mut draws, mut losses) = (0, 0, 0);
            for r in &ok {
                match best_other(r).map(|best| score(r).cmp(&best)) {
                    Some(Ordering::Greater) => wins += 1,
                    Some(Ordering::Equal) => draws += 1,
                    Some(Ordering::Less) => losses += 1,
                    None => {}
                }
            }

            let seed_points: Vec<f64> = by_seed
                .values()
                .map(|games| games.iter().map(|p| p[a]).sum::<f64>() / games.len() as f64)
                .collect();
            let seeds = |f: fn(f64) -> bool| seed_points.iter().filter(|&&p| f(p)).count();
            const EPS: f64 = 1e-9;

            AgentSummary {
                name: names[a].clone(),
                path: paths[a].clone(),
                wins,
                draws,
                losses,
                points: mean(&seed_points),
                seeds_won: seeds(|p| p > 0.5 + EPS),
                seeds_drawn: seeds(|p| (p - 0.5).abs() <= EPS),
                seeds_lost: seeds(|p| p < 0.5 - EPS),
                timeouts: ok.iter().filter(|r| r.timeouts[a].is_some()).count(),
                avg_score: mean(&ok.iter().map(|r| score(r) as f64).collect::<Vec<_>>()),
            }
        })
        .collect();

    Summary {
        games: records.len(),
        failed: records.len() - ok.len(),
        missing: total - records.len(),
        agents,
    }
}

fn mean(values: &[f64]) -> Option<f64> {
    (!values.is_empty()).then(|| values.iter().sum::<f64>() / values.len() as f64)
}

fn format_summary(summary: &Summary, records: &[GameRecord], names: &[String]) -> String {
    let mut out = format!("\n{} games", summary.games);
    if summary.failed > 0 {
        out += &format!(
            ", {}",
            style(format!("{} failed", summary.failed)).red().bold()
        );
    }
    if summary.missing > 0 {
        out += &format!(
            ", {}",
            style(format!("{} not played", summary.missing)).yellow()
        );
    }
    out += "\n";

    const SHOWN_FAILURES: usize = 5;
    let failed: Vec<&GameRecord> = records.iter().filter(|r| r.error.is_some()).collect();
    for r in failed.iter().take(SHOWN_FAILURES) {
        let err = r.error.as_deref().unwrap_or_default();
        let seats: Vec<&str> = by_seat(r).iter().map(|&a| names[a].as_str()).collect();
        out += &format!(
            "  failed: seed {} ({}): {err}\n",
            r.seed,
            seats.join(" vs ")
        );
    }
    if failed.len() > SHOWN_FAILURES {
        out += &format!("  ... and {} more\n", failed.len() - SHOWN_FAILURES);
    }

    let width = names.iter().map(|n| n.len()).max().unwrap_or(0).max(5);
    let row = |cells: [&str; 8]| {
        format!(
            "{:<width$}  {:>5} {:>5} {:>6}  {:>6}  {:>11}  {:>8}  {:>9}\n",
            cells[0], cells[1], cells[2], cells[3], cells[4], cells[5], cells[6], cells[7]
        )
    };
    out += "\n";
    out += &row([
        "agent",
        "wins",
        "draws",
        "losses",
        "points",
        "seeds +/=/-",
        "timeouts",
        "avg score",
    ]);
    let pct = |p: f64| format!("{:.1}%", p * 100.0);
    let or_dash = |v: Option<String>| v.unwrap_or_else(|| "-".to_string());
    for (i, a) in summary.agents.iter().enumerate() {
        let line = row([
            &a.name,
            &a.wins.to_string(),
            &a.draws.to_string(),
            &a.losses.to_string(),
            &or_dash(a.points.map(pct)),
            &format!("{}/{}/{}", a.seeds_won, a.seeds_drawn, a.seeds_lost),
            &a.timeouts.to_string(),
            &or_dash(a.avg_score.map(|s| format!("{s:.1}"))),
        ]);
        out += &agent_style(i, line).to_string();
    }
    out.trim_end().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn points_count_draws_half() {
        assert_eq!(points(&[3, 1]), vec![1.0, 0.0]);
        assert_eq!(points(&[2, 2]), vec![0.5, 0.5]);
        assert_eq!(points(&[5, 5, 1]), vec![0.75, 0.75, 0.0]);
    }

    #[test]
    fn repeated_names_are_numbered() {
        let paths = ["a/v1.cpp", "b/v1.cpp", "v2"].map(String::from);
        assert_eq!(agent_names(&paths), vec!["v1#1", "v1#2", "v2"]);
    }
}
