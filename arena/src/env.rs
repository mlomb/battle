use crate::{
    agent::Agent,
    exec::{executable::Executable, executable_command::ExecutableCommand},
    referee::Referee,
};
use bundler::{bundle, BundlerArgs};
use serde::{Deserialize, Serialize};
use std::{error::Error, path::PathBuf};
use yaml_rust2::{Yaml, YamlLoader};

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Env {
    pub referee: Referee,

    pub min_agents: u8,
    pub max_agents: u8,

    pub agents: Vec<Agent>,
}

#[derive(Debug)]
pub enum EnvError {
    /// The .yml file was not found
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
        let min_agents = self.parse_agent_number("min_agents")?;
        let max_agents = self.parse_agent_number("max_agents")?;
        let agents = self.parse_agents()?;

        if min_agents > max_agents {
            return Err(EnvError::BadField(
                "'min_agents' must be less than or equal to 'max_agents'".to_owned(),
            ));
        }

        if agents.len() == 0 {
            return Err(EnvError::NoAgents);
        }

        Ok(Env {
            referee,
            min_agents,
            max_agents,
            agents,
        })
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
        let parse_optional_path = |field: &str| -> Option<PathBuf> {
            a[field]
                .as_str()
                .map(|path| PathBuf::from(path))
                .map(|path| {
                    if path.is_absolute() {
                        path
                    } else {
                        self.env_path.parent().unwrap().join(path)
                    }
                })
        };

        let src = parse_optional_path("src");
        let win_bin = parse_optional_path("win_bin");
        let linux_bin = parse_optional_path("linux_bin");

        let (s, w, l) = (src.is_some(), win_bin.is_some(), linux_bin.is_some());

        // either src or win_bin/linux_bin must be provided
        if (!s && !w && !l) || (s && (w || l)) {
            return Err(EnvError::BadAgent(format!(
                "Agent '{}' must provide either 'src' or 'win_bin'/'linux_bin'",
                name
            )));
        }

        let executable =
            if let Some(src_path) = src.as_ref() {
                let bundle = bundle(&BundlerArgs::default_from_entry(src_path.to_path_buf()))
                    .map_err(|e| EnvError::BundleError {
                        agent: name.to_owned(),
                        src_path: src_path.clone(),
                        error: e.into(),
                    })?;

                Executable::from_source(bundle.source)
            } else {
                Executable::from_platform_command(
                    win_bin.map(|path| ExecutableCommand::from_binary(path).unwrap()),
                    linux_bin.map(|path| ExecutableCommand::from_binary(path).unwrap()),
                )
            };

        Ok(Agent::new(name, executable))
    }

    fn parse_agent_number(&self, field: &str) -> Result<u8, EnvError> {
        let n = self.doc[field]
            .as_i64()
            .ok_or(EnvError::BadField(format!("'{}' is missing", field)))?;

        if n <= 0 {
            return Err(EnvError::BadField(format!("'{}' must be positive", field)));
        }

        Ok(n as u8)
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
