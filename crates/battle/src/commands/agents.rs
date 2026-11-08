use std::path::{Path, PathBuf};
use std::sync::Arc;

use battle_bundler::BundlerArgs;
use clap::error::ErrorKind;
use clap::{ArgMatches, Args, Command, FromArgMatches};

use crate::exec::Target;

#[derive(Debug, PartialEq)]
pub struct AgentDef {
    pub path: String,
    pub defines: Vec<String>,
}

#[derive(Debug)]
pub struct AgentArgs {
    pub agents: Vec<AgentDef>,
}

impl AgentDef {
    pub fn name(&self) -> String {
        let stem = match Path::new(&self.path).file_stem() {
            Some(stem) => stem.to_string_lossy().to_string(),
            None => self.path.clone(),
        };
        match self.defines.is_empty() {
            true => stem,
            false => format!("{stem}({})", self.defines.join(",")),
        }
    }
}

impl AgentArgs {
    pub fn names(&self) -> Vec<String> {
        self.agents.iter().map(AgentDef::name).collect()
    }

    /// Bundles every agent with its defines.
    pub fn targets(&self) -> Vec<Arc<Target>> {
        self.agents
            .iter()
            .map(|agent| {
                let bundler_args = BundlerArgs {
                    entry: PathBuf::from(&agent.path),
                    defines: agent.defines.clone(),
                };
                Arc::new(Target::from_bundler_args(&bundler_args).expect("correct bundle"))
            })
            .collect()
    }
}

#[derive(Args)]
struct RawAgentArgs {
    /// List of agents participating in the game
    #[arg(short, long, required = true)]
    agent: Vec<String>,

    /// Define `NAME` for the agent before it (a `#define` in C++, the value of `const NAME` in Rust)
    #[arg(short = 'D', long = "define", value_name = "NAME[=VALUE]")]
    define: Vec<String>,
}

impl Args for AgentArgs {
    fn augment_args(cmd: Command) -> Command {
        RawAgentArgs::augment_args(cmd)
    }

    fn augment_args_for_update(cmd: Command) -> Command {
        RawAgentArgs::augment_args_for_update(cmd)
    }
}

impl FromArgMatches for AgentArgs {
    fn from_arg_matches(matches: &ArgMatches) -> Result<Self, clap::Error> {
        let agent_indices: Vec<usize> = matches.indices_of("agent").into_iter().flatten().collect();
        let mut agents: Vec<AgentDef> = matches
            .get_many::<String>("agent")
            .into_iter()
            .flatten()
            .map(|path| AgentDef {
                path: path.clone(),
                defines: vec![],
            })
            .collect();

        let indices = matches.indices_of("define").into_iter().flatten();
        let values = matches.get_many::<String>("define").into_iter().flatten();
        for (index, value) in indices.zip(values) {
            match agent_indices.iter().rposition(|&agent| agent < index) {
                Some(agent) => agents[agent].defines.push(value.clone()),
                None => {
                    return Err(clap::Error::raw(
                        ErrorKind::ArgumentConflict,
                        format!("-D {value} must come after the -a it applies to"),
                    ));
                }
            }
        }

        Ok(Self { agents })
    }

    fn update_from_arg_matches(&mut self, matches: &ArgMatches) -> Result<(), clap::Error> {
        *self = Self::from_arg_matches(matches)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn agent(path: &str, defines: &[&str]) -> AgentDef {
        AgentDef {
            path: path.to_string(),
            defines: defines.iter().map(|d| d.to_string()).collect(),
        }
    }

    fn parse(args: &[&str]) -> Result<AgentArgs, clap::Error> {
        let matches = AgentArgs::augment_args(Command::new("battle")).try_get_matches_from(args)?;
        AgentArgs::from_arg_matches(&matches)
    }

    #[test]
    fn defines_follow_their_agent() {
        let args = [
            "battle", "-a", "a.cpp", "-D", "A=1", "-D", "B", "-a", "b.cpp", "-a", "c.cpp", "-D",
            "C=2",
        ];
        assert_eq!(
            parse(&args).expect("valid").agents,
            vec![
                agent("a.cpp", &["A=1", "B"]),
                agent("b.cpp", &[]),
                agent("c.cpp", &["C=2"])
            ]
        );
    }

    #[test]
    fn names_show_the_defines() {
        assert_eq!(agent("a/v1.cpp", &[]).name(), "v1");
        assert_eq!(agent("v1.cpp", &["A=5", "B"]).name(), "v1(A=5,B)");
    }

    #[test]
    fn define_before_any_agent_fails() {
        assert!(parse(&["battle", "-D", "A=1", "-a", "a.cpp"]).is_err());
    }
}
