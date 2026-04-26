use bundler::bundler_main;
use bundler::BundlerCli;
use clap::Parser;

fn main() {
    let args = BundlerCli::parse();
    bundler_main(args);
}
