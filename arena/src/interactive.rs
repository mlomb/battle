use crate::{env::Env, tournament};
use console::style;
use inquire::{Confirm, MultiSelect, Select};
use std::{
    fmt::{Display, Formatter},
    path::PathBuf,
};

pub fn build_command_interactive(env_file: PathBuf, env: &Env) -> Vec<String> {
    let mut cmd = vec![];

    // push binary name
    cmd.push(std::env::args().next().unwrap());

    // push env file, if not default
    if env_file.to_string_lossy() != "env.yaml" {
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
            todo!("Single match not implemented yet");

            cmd.push("tournament".to_owned());

            for agent in prompt_agents(&env) {
                cmd.push("-a".to_owned());
                cmd.push(agent);
            }
        }
        Options::Tournament => {
            cmd.push("tournament".to_owned());

            let formats = vec![tournament::Format::RoundRobin, tournament::Format::Gauntlet];
            let format = Select::new("Select tournament format", formats)
                .prompt()
                .expect("a valid format");

            cmd.push("--format".to_owned());
            cmd.push(match format {
                tournament::Format::RoundRobin => "round-robin".to_owned(),
                tournament::Format::Gauntlet => "gauntlet".to_owned(),
                tournament::Format::Matchmaking => todo!(),
            });

            for agent in prompt_multi_agents(&env) {
                cmd.push("-a".to_owned());
                cmd.push(agent);
            }
        }
        Options::Optimize => todo!(),
    }

    prompt_execution_mode(&mut cmd);

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

    for i in 0..env.referee.max_agents {
        let mut options: Vec<String> = env.get_agent_names();
        assert!(options.len() > 0);

        // the user may be confused by this
        // if options.len() == 1 {
        //     // if there is only one agent, select it by default
        //     agents.push(options[0].clone());
        //     break;
        // }

        if i + 1 > env.referee.min_agents {
            options.insert(0, agent_none.clone());
        }

        let agent = Select::new(
            &format!("Select agent #{} ({} max)", i + 1, env.referee.max_agents),
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

fn prompt_multi_agents(env: &Env) -> Vec<String> {
    MultiSelect::new("Select agents", env.get_agent_names())
        .with_help_message("Use space to select, enter to confirm")
        .prompt()
        .expect("agents to be selected")
}

fn prompt_execution_mode(cmd: &mut Vec<String>) {
    enum ExecutionMode {
        Local,
        Network,
    }

    impl Display for ExecutionMode {
        fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
            match self {
                ExecutionMode::Local => write!(f, "Run on this machine (threaded)"),
                ExecutionMode::Network => {
                    write!(
                        f,
                        "Run on P2P nodes in the network {}",
                        style("(requires running nodes)").bold().to_string()
                    )
                }
            }
        }
    }

    let runner = Select::new(
        "Select where to run matches",
        vec![ExecutionMode::Local, ExecutionMode::Network],
    )
    .prompt()
    .expect("a runner to be selected");

    match runner {
        ExecutionMode::Local => {
            // ask for the number of threads
            let threads = inquire::Text::new("How many threads?")
                .with_default("-1")
                .with_help_message("Use -1 to use physical cores - 2")
                .prompt()
                .expect("a valid number")
                .parse::<i32>()
                .expect("a valid number");

            cmd.push("--threads".to_owned());
            cmd.push(threads.to_string());
        }
        ExecutionMode::Network => cmd.push("--network".to_owned()),
    }

    println!("Selected runner: {}", runner);
}
