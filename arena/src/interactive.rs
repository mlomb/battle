use console::style;
use inquire::{Confirm, Select};

use crate::env::Env;
use std::{
    fmt::{Display, Formatter},
    path::PathBuf,
};

pub fn build_command_interactive(env_file: PathBuf, env: Env) -> Vec<String> {
    let mut cmd = vec![];

    // push binary name
    cmd.push(std::env::args().next().unwrap());

    // push env file, if not default
    if env_file.to_string_lossy() != "env.yml" {
        cmd.push("--env".to_owned());
        cmd.push(env_file.to_str().unwrap().to_owned());
    }

    let options: Vec<Options> = vec![
        Options::SingleMatch,
        Options::Tournament,
        Options::Optimize,
        //
    ];

    match Select::new("What do you want to do?", options)
        .prompt()
        .expect("a valid option")
    {
        Options::SingleMatch => {
            cmd.push("run".to_owned());

            for i in 0..env.max_agents {
                if let Some(agent) = request_agent(
                    &format!("Select agent #{} ({} max)", i + 1, env.max_agents),
                    i + 1 > env.min_agents,
                    &env,
                ) {
                    cmd.push("-a".to_owned());
                    cmd.push(agent);
                } else {
                    // once the user selects None, stop asking
                    break;
                }
            }
        }
        Options::Tournament => todo!(),
        Options::Optimize => todo!(),
    }

    // print final command
    println!("\n    {}\n", style("Command:").cyan().bold());
    println!("    {}\n", cmd.join(" "));

    if Confirm::new("Run the command now?")
        .with_default(true)
        .prompt()
        .expect("a valid confirmation")
    {
        cmd
    } else {
        std::process::exit(0)
    }
}

enum Options {
    SingleMatch,
    Tournament,
    Optimize,
}

impl Display for Options {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Options::SingleMatch => write!(f, "Run a single match"),
            Options::Tournament => write!(f, "Run a tournament"),
            Options::Optimize => write!(f, "Optimize parameters"),
        }
    }
}

fn request_agent(message: &str, optional: bool, env: &Env) -> Option<String> {
    let mut options: Vec<String> = env.agents.iter().map(|agent| agent.name.clone()).collect();

    if optional {
        options.insert(0, style("None").red().to_string());
    }

    let ans = Select::new(message, options.clone())
        .prompt()
        .expect("an agent to be selected");

    if optional && ans == options[0] {
        None
    } else {
        Some(ans.to_owned())
    }
}
