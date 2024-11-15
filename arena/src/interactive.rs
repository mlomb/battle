use console::style;
use inquire::{Confirm, Select};

use crate::{env::Env, tournament};
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
            cmd.push("tournament".to_owned());

            for agent in prompt_agents(&env) {
                cmd.push("-a".to_owned());
                cmd.push(agent);
            }
        }
        Options::Tournament => {
            cmd.push("tournament".to_owned());

            let formats = vec![
                tournament::format::Format::RoundRobin,
                tournament::format::Format::Gauntlet,
            ];
            let format = Select::new("Select tournament format", formats)
                .prompt()
                .expect("a valid format");

            cmd.push("--format".to_owned());
            cmd.push(match format {
                tournament::format::Format::RoundRobin => "round-robin".to_owned(),
                tournament::format::Format::Gauntlet => "gauntlet".to_owned(),
                tournament::format::Format::Matchmaking => todo!(),
            });

            for agent in prompt_agents(&env) {
                cmd.push("-a".to_owned());
                cmd.push(agent);
            }

            // TODO: esta mal porque hay que elegir el pool de agents y no así
        }
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
        println!("========================================");
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

fn prompt_agents(env: &Env) -> Vec<String> {
    let agent_none: String = style("None").red().to_string();
    let mut agents = vec![];

    for i in 0..env.max_agents {
        let mut options: Vec<String> = vec![]; // env.agents.iter().map(|agent| agent.name.clone()).collect();
        assert!(options.len() > 0);

        // the user may be confused by this
        // if options.len() == 1 {
        //     // if there is only one agent, select it by default
        //     agents.push(options[0].clone());
        //     break;
        // }

        if i + 1 > env.min_agents {
            options.insert(0, agent_none.clone());
        }

        let agent = Select::new(
            &format!("Select agent #{} ({} max)", i + 1, env.max_agents),
            options,
        )
        .prompt()
        .expect("an agent to be selected");

        if agent == agent_none {
            // once the user selects None, stop asking
            break;
        }

        agents.push(agent);
    }

    agents
}

enum AgentPrompt {
    None,
    Random,
    Agent(String),
}
