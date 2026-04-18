use std::io::{self, BufRead, Write};

/// Used by integration tests

fn main() -> io::Result<()> {
    let mut line = String::new();
    io::stdin().lock().read_line(&mut line)?;

    println!("hello from stdout");
    eprintln!("hello from stderr");

    io::stderr().flush()?;
    io::stdout().flush()?;

    print!("{line}");
    eprint!("{line}");

    io::stderr().flush()?;
    io::stdout().flush()?;

    println!("goodbye from stdout");
    eprintln!("goodbye from stderr");

    Ok(())
}
