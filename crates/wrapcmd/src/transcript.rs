use std::fmt;
use std::str::FromStr;

#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    /// A line read from stdin.
    In(String),
    /// A line written to stdout.
    Out(String),
    /// A line written to stderr.
    Err(String),
}

impl Event {
    pub fn content(&self) -> &str {
        match self {
            Event::In(s) | Event::Out(s) | Event::Err(s) => s,
        }
    }

    pub fn tag(&self) -> char {
        match self {
            Event::In(_) => '<',
            Event::Out(_) => '>',
            Event::Err(_) => '!',
        }
    }

    pub fn parse_line(line: &str) -> Result<Self, String> {
        let (tag, content) = line
            .split_once(' ')
            .ok_or_else(|| format!("bad transcript line: {line:?}"))?;
        let ctors: &[fn(String) -> Event] = &[Event::In, Event::Out, Event::Err];
        ctors
            .iter()
            .find(|ctor| ctor(String::new()).tag().to_string() == tag)
            .map(|ctor| ctor(content.to_owned()))
            .ok_or_else(|| format!("unknown stream `{tag}`"))
    }
}

impl fmt::Display for Event {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} {}", self.tag(), self.content())
    }
}

/// An ordered sequence of I/O events.
#[derive(Debug, Default, Clone)]
pub struct Transcript {
    pub events: Vec<Event>,
}

impl Transcript {
    pub fn stdin(&self) -> String {
        self.collect_stream(|e| matches!(e, Event::In(_)))
    }

    pub fn stdout(&self) -> String {
        self.collect_stream(|e| matches!(e, Event::Out(_)))
    }

    pub fn stderr(&self) -> String {
        self.collect_stream(|e| matches!(e, Event::Err(_)))
    }

    pub fn save(&self, path: &std::path::Path) -> std::io::Result<()> {
        std::fs::write(path, self.to_string())
    }

    fn collect_stream(&self, pred: impl Fn(&Event) -> bool) -> String {
        self.events
            .iter()
            .filter(|e| pred(e))
            .map(|e| format!("{}\n", e.content()))
            .collect()
    }
}

/// Serialize: one `<tag> <content>` line per event.
impl fmt::Display for Transcript {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for event in &self.events {
            writeln!(f, "{event}")?;
        }
        Ok(())
    }
}

/// Deserialize from the text format produced by `Display`.
impl FromStr for Transcript {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let events = s
            .lines()
            .map(Event::parse_line)
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Transcript { events })
    }
}
