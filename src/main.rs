mod cgi;
mod config;
mod http;
mod server;
mod util;

use config::load_config;
use server::HttpServer;
use std::path::PathBuf;

fn main() {
    let mut args = std::env::args().skip(1);
    let mut config_path = PathBuf::from("localhost.conf");
    let mut check_only = false;

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-c" | "--config" => {
                let Some(path) = args.next() else {
                    eprintln!("missing value after {arg}");
                    std::process::exit(2);
                };
                config_path = PathBuf::from(path);
            }
            "--check-config" => check_only = true,
            "-h" | "--help" => {
                println!("localhost [-c|--config FILE] [--check-config]");
                return;
            }
            other => {
                eprintln!("unknown argument: {other}");
                std::process::exit(2);
            }
        }
    }

    let configs = match load_config(&config_path, check_only) {
        Ok(configs) => configs,
        Err(err) => {
            eprintln!("configuration error: {err}");
            std::process::exit(1);
        }
    };
    if check_only {
        println!("configuration OK: {} valid server block(s)", configs.len());
        return;
    }

    let mut server = match HttpServer::new(configs) {
        Ok(server) => server,
        Err(err) => {
            eprintln!("server initialization failed: {err}");
            std::process::exit(1);
        }
    };
    if let Err(err) = server.run() {
        eprintln!("server stopped: {err}");
        std::process::exit(1);
    }
}
