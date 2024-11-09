use crate::{agent::Agent, referee::Referee};
use std::path::PathBuf;
use yaml_rust2::{Yaml, YamlLoader};

#[derive(Debug)]
pub struct AgentDef {
    name: String,
    src: Option<PathBuf>,
    win_bin: Option<PathBuf>,
    linux_bin: Option<PathBuf>,
}

#[derive(Debug)]
pub struct Env {
    referee: Referee,

    min_agents: u8,
    max_agents: u8,

    agents: Vec<AgentDef>,
}

#[derive(Debug)]
pub enum EnvError {
    /// The .yml file was not found
    NotFound,
    /// Error parsing the YAML file
    ParseError(yaml_rust2::ScanError),
    /// The referee could not be obtained
    BadReferee(String),
    /// Missing or invalid field
    BadField(String),
}

struct EnvParser {
    /// The path to the .yml file
    /// This is used to resolve relative paths
    env_path: PathBuf,

    /// The parsed YAML document
    doc: Yaml,
}

impl Env {
    pub fn from_file(env_path: &PathBuf) -> Result<Env, EnvError> {
        EnvParser::from_file(env_path.clone())?.parse()
    }
}

impl EnvParser {
    fn from_file(mut env_path: PathBuf) -> Result<EnvParser, EnvError> {
        // must be absolute to resolve relative paths
        env_path = env_path.canonicalize().expect("to canonicalize path");

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
        Ok(Env {
            referee: self.parse_referee()?,
            min_agents: self.parse_agent_number("min_agents")?,
            max_agents: self.parse_agent_number("max_agents")?,
            agents: self.parse_agents()?,
        })
    }

    pub fn parse_referee(&self) -> Result<Referee, EnvError> {
        let referee_preset = self.doc["referee"]
            .as_str()
            .ok_or(EnvError::BadReferee("Referee is missing".to_owned()))?;

        Referee::from_preset(referee_preset).map_err(|e| EnvError::BadReferee(e))
    }

    pub fn parse_agents(&self) -> Result<Vec<AgentDef>, EnvError> {
        if let Some(agents) = self.doc["agents"].as_hash() {
            return agents
                .iter()
                .map(|(name, def)| self.parse_agent(name.as_str().unwrap(), def))
                .collect();
        }
        // no agents provided
        Ok(vec![])
    }

    pub fn parse_agent(&self, name: &str, a: &Yaml) -> Result<AgentDef, EnvError> {
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

        Ok(AgentDef {
            name: name.to_owned(),
            src: parse_optional_path("src"),
            win_bin: parse_optional_path("win_bin"),
            linux_bin: parse_optional_path("linux_bin"),
        })
    }

    pub fn parse_agent_number(&self, field: &str) -> Result<u8, EnvError> {
        let n = self.doc[field]
            .as_i64()
            .ok_or(EnvError::BadField(format!("'{}' is missing", field)))?;

        if n <= 0 {
            return Err(EnvError::BadField(format!("'{}' must be positive", field)));
        }

        Ok(n as u8)
    }
}
