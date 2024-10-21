use std::path::PathBuf;

pub struct Agent {
    path: PathBuf,
    params: Vec<String>,
}

impl Agent {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            params: vec![],
        }
    }

    pub fn command(&self) -> String {
        self.path.to_str().unwrap().to_string()
    }
}
