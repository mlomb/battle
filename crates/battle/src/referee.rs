use std::{
    path::{Path, PathBuf},
    process::Command,
    sync::Arc,
};

use include_dir::{Dir, include_dir};
use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;

use crate::exec::{CommandExt, Executable, Target};

static REFEREES_CG_CPP: Dir<'_> = include_dir!("$CARGO_MANIFEST_DIR/assets/cg-cpp");

/// The protocol used by the referee.
/// It defines how the agents are passed to the referee, how logs are collected, etc.
#[derive(Debug, Serialize, Deserialize, Clone)]
pub enum Protocol {
    /// CodinGame referee protocol compatible with cg-brutaltester.
    /// See: https://github.com/dreignier/cg-brutaltester
    ///
    /// Basically: `-p1 "./agent1" -p2 "./agent2"`
    CodinGame,
}

/// A referee (a program) that runs a match between agents (other programs) and collects the results.
#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Referee<T = Arc<Target>> {
    /// The protocol used by the referee
    pub protocol: Protocol,

    /// The target to execute
    pub target: T,

    /// The minimum number of agents required by the referee
    pub min_agents: usize,

    /// The maximum number of agents accepted by the referee
    pub max_agents: usize,

    /// Overrides the per-turn time limit of the agents, in milliseconds.
    /// The first turn keeps the game's own limit if it is longer. `None` keeps the game's limits.
    pub turn_timeout: Option<u64>,
}

impl Referee<Arc<Target>> {
    /// Creates a new referee from a curated list of referess available in the `referees` directory.
    /// For now, only CodinGame referees are supported.
    pub fn from_preset(preset: &str) -> Result<Self, String> {
        let target = if preset.ends_with("-ref") {
            // try to look up Jar files locally
            let path = Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../referees/cg-jar")
                .join(format!("{}.jar", preset.trim_end_matches("-ref")));

            if !path.is_file() {
                return Err(format!(
                    "Jar referee not found: {}.\n\nNote that Jar referees are not distributed via crates.io.",
                    path.to_string_lossy()
                ));
            }

            Target::from_executable(Executable::from_jar(path.clone()).expect("read success"))
        } else {
            // look up C++ referees from REFEREES_CG_CPP
            let path = PathBuf::from(format!("{}.cpp", preset));

            if !REFEREES_CG_CPP.contains(&path) {
                return Err(format!("Referee not found: {preset}"));
            }

            // extract the folder
            let tmp = tempfile::tempdir().expect("failed to create temp dir");
            REFEREES_CG_CPP
                .extract(tmp.path())
                .expect("extract success");

            Target::from_entrypoint(tmp.path().join(&path)).expect("correct bundle")
        };

        let (min_agents, max_agents) = match preset.trim_end_matches("-ref").to_string().as_str() {
            "cg-2015-back-to-the-code" => (2, 4),
            "cg-2018-botters-of-the-galaxy" => (2, 2),
            "cg-2019-code-a-la-mode" => (3, 3),
            "cg-2017-code4life" => (2, 2),
            "cg-2016-codebusters" => (2, 2),
            "cg-2016-codebusters-contest" => (2, 2),
            "cg-2018-code-of-kutulu" => (4, 4),
            "cg-2018-code-royale" => (2, 2),
            "cg-2017-coders-of-the-caribbean" => (2, 2),
            "cg-2016-coders-strike-back" => (2, 4),
            "cg-2016-coders-strike-back-original" => (2, 4),
            "cg-2019-crystal-rush" => (2, 2),
            "cg-2020-fall-witches-brew" => (2, 2),
            "cg-2023-fall-fish" => (2, 2),
            "cg-2022-fall-keep-off-the-grass" => (2, 2),
            "cg-2016-fantastic-bits" => (2, 2),
            "cg-2016-fantastic-bits-contest" => (2, 2),
            "cg-2014-game-of-drones" => (2, 4),
            "cg-2014-game-of-drones-original" => (2, 4),
            "cg-2017-ghost-in-the-cell" => (2, 2),
            "cg-2015-great-escape" => (2, 3),
            "cg-2016-hypersonic" => (2, 4),
            "cg-2018-legends-of-code-and-magic" => (2, 2),
            "cg-2017-mean-max" => (3, 3),
            "cg-2020-ocean-of-code" => (2, 2),
            "cg-2014-platinum-rift-episode-1" => (2, 4),
            "cg-2014-platinum-rift-episode-2" => (2, 2),
            "cg-2014-platinum-rift-episode-2-original" => (2, 2),
            "cg-2014-poker-chip-race" => (2, 4),
            "cg-2016-smash-the-code" => (2, 2),
            "cg-2022-spring-spider-attack" => (2, 2),
            "cg-2020-spring-pac-man" => (2, 2),
            "cg-2021-spring-photosynthesis" => (2, 2),
            "cg-2023-spring-ants" => (2, 2),
            "cg-2024-summer-olympics" => (3, 3),
            "cg-2026-spring-troll-farm" => (2, 2),
            "cg-2025-summer-soak-overflow" => (2, 2),
            "cg-2026-summer-back-track-king" => (2, 2),
            "cg-2014-tron-battle" => (2, 4),
            "cg-2018-ultimate-tic-tac-toe" => (2, 2),
            "cg-2024-winter-sprawl" => (2, 2),
            "cg-2026-winter-snakebyte" => (2, 2),
            "cg-2017-wondev-woman" => (2, 2),
            "cg-2018-xmas-rush" => (2, 2),
            _ => {
                log::warn!(
                    "Referee file available '{}', but min/max agents is unknown. Assuming min=2 max=4.",
                    preset
                );
                (2, 4)
            }
        };

        assert!(min_agents >= 1);
        assert!(min_agents <= max_agents);

        Ok(Self {
            protocol: Protocol::CodinGame,
            target: Arc::new(target),
            min_agents,
            max_agents,
            turn_timeout: None,
        })
    }

    pub fn from_target(target: Target) -> Self {
        Self {
            protocol: Protocol::CodinGame,
            target: Arc::new(target),
            min_agents: 1,
            max_agents: 4,
            turn_timeout: None,
        }
    }

    /// Sets [`Referee::turn_timeout`].
    pub fn with_turn_timeout(mut self, turn_timeout: Option<u64>) -> Self {
        self.turn_timeout = turn_timeout;
        self
    }

    // TODO: return Result
    pub fn from_string(str: &str) -> Self {
        match Referee::from_preset(str) {
            Ok(referee) => referee,
            Err(_) => {
                // log::warn!("Failed to load referee from preset: {err}");

                Referee::from_target(
                    Target::from_entrypoint(PathBuf::from(str.to_string()))
                        .expect("to compile referee"),
                )
            }
        }
    }
}

impl Referee<Arc<Mutex<Executable>>> {
    pub fn command(
        &self,
        agent_cmds: &[Command],
        seed: u64,
        output_logs: Option<PathBuf>,
    ) -> Command {
        let mut exe = self.target.blocking_lock();
        let mut cmd = exe.command();

        match self.protocol {
            Protocol::CodinGame => {
                for (i, agent) in agent_cmds.iter().enumerate() {
                    cmd.arg(format!("-p{}", i + 1));
                    cmd.arg(agent.command_line_string());
                }

                cmd.arg("-seed").arg(seed.to_string());

                if let Some(logs_path) = output_logs {
                    cmd.arg("-l").arg(logs_path);
                }
                if let Some(turn_timeout) = self.turn_timeout {
                    cmd.arg("-timeout").arg(turn_timeout.to_string());
                }
            }
        }

        cmd
    }
}
