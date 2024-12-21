use crate::{
    agent::Agent,
    exec::{executable::Executable, executable_command::ExecutableCommand},
    referee::Referee,
};
use bundler::{
    bundle,
    source::{Language, Source},
    BundlerArgs,
};
use console::style;
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, error::Error, fmt::Display, path::PathBuf};
use yaml_rust2::{Yaml, YamlLoader};

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Env {
    pub referee: Referee,

    pub agents: Vec<Agent>,
}

#[derive(Debug)]
pub enum EnvError {
    /// The .yaml file was not found
    NotFound,
    /// Error parsing the YAML file
    ParseError(yaml_rust2::ScanError),
    /// No agents provided
    NoAgents,
    /// The referee could not be obtained
    BadReferee(String),
    /// The agent definition is invalid
    BadAgent(String),
    /// Missing or invalid field
    BadField(String),

    BundleError {
        agent: String,
        src_path: PathBuf,
        error: Box<dyn Error>,
    },
    // TODO: agent error, y que tenga el string de agent y un error especifico :)
}

struct EnvParser {
    /// The path to the YAML file.
    /// This is used to resolve relative paths
    env_path: PathBuf,

    /// The parsed YAML document
    doc: Yaml,
}

impl EnvParser {
    fn from_file(mut env_path: PathBuf) -> Result<EnvParser, EnvError> {
        // must be absolute to resolve relative paths
        env_path = env_path.canonicalize().map_err(|_| EnvError::NotFound)?;

        let content = std::fs::read_to_string(env_path.clone()).map_err(|_| EnvError::NotFound)?;
        let docs = YamlLoader::load_from_str(&content).map_err(EnvError::ParseError)?;
        let doc = &docs[0];
        assert!(!doc.is_badvalue());

        Ok(EnvParser {
            env_path: env_path.clone(),
            doc: doc.clone(),
        })
    }

    fn parse(self) -> Result<Env, EnvError> {
        let referee = self.parse_referee()?;
        let agents = self.parse_agents()?;

        if agents.len() == 0 {
            return Err(EnvError::NoAgents);
        }

        Ok(Env { referee, agents })
    }

    fn parse_referee(&self) -> Result<Referee, EnvError> {
        let referee_preset = self.doc["referee"]
            .as_str()
            .ok_or(EnvError::BadReferee("'referee' is missing".to_owned()))?;

        Referee::from_preset(referee_preset).map_err(|e| EnvError::BadReferee(e))
    }

    fn parse_agents(&self) -> Result<Vec<Agent>, EnvError> {
        if let Some(agents) = self.doc["agents"].as_hash() {
            return agents
                .iter()
                .map(|(name, def)| self.parse_agent(name.as_str().unwrap(), def))
                .collect();
        }
        // no agents provided
        Ok(vec![])
    }

    fn parse_agent(&self, name: &str, a: &Yaml) -> Result<Agent, EnvError> {
        let files = self.parse_agent_files(a)?;
        let src = self.parse_src(&a["src"], name, &files)?;
        let cmd = self.parse_cmd(&a["cmd"], name, &files)?;

        if src.is_some() && cmd.is_some() {
            return Err(EnvError::BadAgent(format!(
                "Agent '{}' cannot provide both 'src' and 'cmd'",
                name
            )));
        }

        let executable = src.or(cmd).ok_or(EnvError::BadAgent(format!(
            "Agent '{}' must provide either 'src' or 'cmd'",
            name
        )))?;

        Ok(Agent::new(name, executable))
    }

    fn parse_path(&self, path: &Yaml) -> Option<PathBuf> {
        path.as_str().map(|path| PathBuf::from(path)).map(|path| {
            if path.is_absolute() {
                path
            } else {
                self.env_path.parent().unwrap().join(path)
            }
        })
    }

    fn parse_agent_files(&self, a: &Yaml) -> Result<HashMap<String, Vec<u8>>, EnvError> {
        match &a["files"] {
            Yaml::BadValue => Ok(HashMap::new()), // missing means empty
            Yaml::Hash(linked_hash_map) => {
                let files = linked_hash_map
                    .iter()
                    .map(|(name, path)| {
                        let name = name
                            .as_str()
                            .ok_or(EnvError::BadAgent(format!("Invalid file name {:?}", name)))?
                            .to_owned();
                        let path = self
                            .parse_path(path)
                            .ok_or(EnvError::BadAgent(format!("Invalid file path {:?}", path)))?;

                        let content = std::fs::read(&path).map_err(|_| {
                            EnvError::BadAgent(format!("Failed to read file {:?}", path))
                        })?;

                        Ok((name, content))
                    })
                    .collect::<Result<_, EnvError>>()?;

                Ok(files)
            }
            _ => Err(EnvError::BadAgent(
                "Invalid 'files' field, expected mapping".to_owned(),
            )),
        }
    }

    fn parse_src(
        &self,
        src: &Yaml,
        name: &str,
        files: &HashMap<String, Vec<u8>>,
    ) -> Result<Option<Executable>, EnvError> {
        if let Some(src_path) = self.parse_path(src) {
            let source = if src_path.extension() == Some("rs".as_ref()) {
                // we assume that Rust source is already bundled
                std::fs::read_to_string(src_path.clone())
                    .map(|src| Source {
                        code: src,
                        language: Language::Rust,
                    })
                    .map_err(|err| EnvError::BundleError {
                        agent: name.to_owned(),
                        src_path: src_path.clone(),
                        error: Box::new(err),
                    })?
            } else {
                let bundle = bundle(&BundlerArgs::default_from_entry(src_path.to_path_buf()))
                    .map_err(|e| EnvError::BundleError {
                        agent: name.to_owned(),
                        src_path: src_path.clone(),
                        error: e.into(),
                    })?;

                bundle.source
            };

            Ok(Some(Executable::from_source(source, files.clone())))
        } else {
            Ok(None)
        }
    }

    fn parse_cmd(
        &self,
        cmd: &Yaml,
        _name: &str,
        files: &HashMap<String, Vec<u8>>,
    ) -> Result<Option<Executable>, EnvError> {
        match cmd {
            Yaml::BadValue => Ok(None), // can be missing
            Yaml::Hash(hash) => {
                let parse_platform_cmd = |platform: &str| {
                    hash.get(&Yaml::String(platform.to_owned()))
                        .map(|cmd| self.parse_cmd_line(cmd))
                        .transpose()?
                        .map(|parts| ExecutableCommand::from_cmd(parts, files.clone()))
                        .transpose()
                        .map_err(|e| {
                            EnvError::BadAgent(format!(
                                "Failed to create {} command: {:?}",
                                platform, e
                            ))
                        })
                };

                let windows = parse_platform_cmd("win")?;
                let unix = parse_platform_cmd("unix")?;

                Ok(Some(Executable::from_platform_command(windows, unix)))
            }
            Yaml::Array(_) => self.parse_cmd_line(cmd).and_then(|parts| {
                ExecutableCommand::from_cmd(parts, files.clone())
                    .map(Executable::from_command)
                    .map(Some)
                    .map_err(|e| {
                        EnvError::BadAgent(format!("Failed to create generic command: {:?}", e))
                    })
            }),
            _ => Err(EnvError::BadAgent(
                "Invalid 'cmd' field, expected array".to_owned(),
            )),
        }
    }

    fn parse_cmd_line(&self, cmd: &Yaml) -> Result<Vec<String>, EnvError> {
        if let Some(parts) = cmd.as_vec() {
            parts
                .iter()
                .map(|part| match part {
                    Yaml::Real(f) => Ok(f.clone()),
                    Yaml::Integer(n) => Ok(n.to_string()),
                    Yaml::String(s) => Ok(s.clone()),
                    _ => Err(EnvError::BadField(format!(
                        "Invalid command line part {:?}",
                        part
                    ))),
                })
                .collect()
        } else {
            return Err(EnvError::BadField(
                "Invalid cmd line, expected array".to_owned(),
            ));
        }
    }
}

impl Env {
    pub fn from_file(env_path: &PathBuf) -> Result<Env, EnvError> {
        EnvParser::from_file(env_path.clone())?.parse()
    }

    pub fn get_agent<T: ToString>(&mut self, name: T) -> Option<&mut Agent> {
        self.agents.iter_mut().find(|a| a.name == name.to_string())
    }
}

impl Display for Env {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        writeln!(f, "{}", style("Referee").bold())?;
        writeln!(f, "  protocol: {:?}", self.referee.protocol)?;
        writeln!(f, "  executable: {:?}", self.referee.exe)?;
        writeln!(f, "  min_agents: {}", self.referee.min_agents)?;
        writeln!(f, "  max_agents: {}", self.referee.max_agents)?;
        writeln!(f, "")?;

        for agent in &self.agents {
            writeln!(
                f,
                "{}({})",
                style("Agent").bold(),
                style(agent.name.clone()).yellow()
            )?;
            writeln!(f, "  executable: {:?}", agent.executable)?;
        }
        Ok(())
    }
}
