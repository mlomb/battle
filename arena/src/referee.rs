use crate::Agent;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct Referee {
    path: PathBuf,
}

impl Referee {
    // TODO: remove
    pub fn new(path: PathBuf) -> Self {
        Self { path }
    }

    pub fn from_preset(preset: &str) -> Result<Self, String> {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("referees")
            .join(format!("{}.jar", preset));

        if !path.is_file() {
            return Err(format!("Referee not found: {}", path.to_string_lossy()));
        }

        Ok(Self { path })
    }

    pub fn command(&self, agents: &Vec<Agent>) -> Vec<String> {
        let mut args: Vec<String> = vec![
            "java".to_string(),
            "-jar".to_string(),
            self.path.to_str().unwrap().to_string(),
        ];

        for (i, agent) in agents.iter().enumerate() {
            args.push(format!("-p{}", i + 1));
            args.push(agent.command());
        }

        // args.push("-l pepito.txt".to_string());

        args
    }
}
