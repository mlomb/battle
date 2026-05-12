use battle_bundler::bundler_main;
use battle_bundler::BundlerCli;
use clap::Parser;

fn main() {
    let args = BundlerCli::parse();
    bundler_main(args);
}
