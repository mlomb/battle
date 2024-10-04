use bundler::bundle;
use std::path::Path;

fn main() {
    let source = bundle(Path::new(
        "C:/Users/mlomb/Desktop/bots/projects/Fall Challenge 2023/lib",
    ))
    .unwrap();
    std::fs::write("./out.rs", source).unwrap();
}
