mod ipc;

use std::{
    env,
    ffi::{CString, OsStr, OsString},
    fs, io,
    os::unix::{ffi::OsStrExt, fs::MetadataExt, process::CommandExt},
    path::{Path, PathBuf},
    process::Command,
};

const USAGE: &str = "usage: hyprspace-launch [--context TOKEN | --launch-token TOKEN] -- COMMAND [ARG ...]\n       hyprspace-launch --capture | --status";

fn valid_token(token: &OsStr) -> bool {
    let bytes = token.as_bytes();
    (1..=128).contains(&bytes.len())
        && bytes
            .iter()
            .all(|c| c.is_ascii_alphanumeric() || *c == b'-')
}

fn launch_paths(helper: &Path) -> io::Result<OsString> {
    let shim = helper
        .parent()
        .ok_or_else(|| io::Error::other("Cannot locate launch shims"))?
        .join("launch-bin");
    let current = env::var_os("PATH").unwrap_or_else(|| "/bin:/usr/bin".into());
    let paths =
        std::iter::once(shim.clone()).chain(env::split_paths(&current).filter(|p| p != &shim));
    env::join_paths(paths).map_err(io::Error::other)
}

fn launch_command(args: &[OsString], token: &OsStr, helper: &Path) -> io::Result<Command> {
    let mut command = Command::new(&args[0]);
    command.args(&args[1..]);
    if valid_token(token) {
        command
            .env("HYPRSPACE_LAUNCH_TOKEN", token)
            .env("XDG_ACTIVATION_TOKEN", token)
            .env("HL_INITIAL_WORKSPACE_TOKEN", "")
            .env("PATH", launch_paths(helper)?);
    } else {
        if env::var_os("HYPRSPACE_LAUNCH_TOKEN")
            .filter(|s| !s.is_empty())
            .is_some_and(|previous| env::var_os("XDG_ACTIVATION_TOKEN") == Some(previous))
        {
            command.env_remove("XDG_ACTIVATION_TOKEN");
        }
        command.env_remove("HYPRSPACE_LAUNCH_TOKEN");
    }
    Ok(command)
}

fn original_service(name: &OsStr, helper: &Path) -> io::Result<PathBuf> {
    let paths = env::var_os("PATH").unwrap_or_else(|| "/bin:/usr/bin".into());
    let cwd = env::current_dir().ok();
    let helper_identity = fs::metadata(helper).ok();
    for directory in env::split_paths(&paths) {
        let candidate = if directory.is_absolute() {
            directory.join(name)
        } else if let Some(cwd) = &cwd {
            cwd.join(directory).join(name)
        } else {
            continue;
        };
        let Some(resolved) = fs::canonicalize(&candidate).ok().filter(|p| {
            // Another installation's shims must not forward back into this one.
            p != helper && p.file_name() != Some(OsStr::new("hyprspace-launch"))
        }) else {
            continue;
        };
        let Ok(identity) = fs::metadata(resolved) else {
            continue;
        };
        if !identity.is_file()
            || helper_identity.as_ref().is_some_and(|helper| {
                (identity.dev(), identity.ino()) == (helper.dev(), helper.ino())
            })
        {
            continue;
        }
        let Ok(path) = CString::new(candidate.as_os_str().as_bytes()) else {
            continue;
        };
        if unsafe { libc::access(path.as_ptr(), libc::X_OK) } == 0 {
            return Ok(candidate);
        }
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!("cannot find {}", name.to_string_lossy()),
    ))
}

fn desktop_entry(value: &OsStr) -> bool {
    let bytes = value.as_bytes();
    bytes.ends_with(b".desktop") || bytes.windows(9).any(|s| s == b".desktop:")
}

fn payload_offset(name: &OsStr, args: &[OsString]) -> Option<usize> {
    if name == "uwsm" && args.first().map(OsString::as_os_str) != Some(OsStr::new("app")) {
        return None;
    }
    let offset = usize::from(name == "uwsm");
    let split = if args
        .get(offset)
        .is_some_and(|arg| !arg.as_bytes().starts_with(b"-"))
    {
        if name != "app2unit" {
            // UWSM parses options even after the command until its first --.
            return None;
        }
        // An app2unit payload's later -- belongs to that payload.
        0
    } else {
        args.iter().position(|s| s == "--")? + 1
    };
    let command = args.get(split)?;
    (!desktop_entry(command)).then_some(split)
}

fn forward_service(name: &OsStr, args: Vec<OsString>, helper: &Path) -> io::Result<()> {
    let original = original_service(name, helper)?;
    let token = env::var_os("HYPRSPACE_LAUNCH_TOKEN").unwrap_or_default();
    let mut command = Command::new(original);
    if let Some(split) = valid_token(&token)
        .then(|| payload_offset(name, &args))
        .flatten()
    {
        command
            .args(&args[..split])
            .arg(helper)
            .arg("--launch-token")
            .arg(token)
            .arg("--")
            .args(&args[split..]);
    } else {
        command.args(args);
    }
    Err(command.exec())
}

fn consume_context(context: &OsStr) -> OsString {
    if valid_token(context) {
        ipc::request(&format!("consume {}", context.to_string_lossy())).into()
    } else {
        OsString::new()
    }
}

fn run() -> io::Result<()> {
    let mut argv = env::args_os();
    let invoked = argv.next().unwrap_or_default();
    let args = argv.collect::<Vec<_>>();
    let helper = env::current_exe()?;
    let name = Path::new(&invoked).file_name().unwrap_or_default();
    if matches!(name.to_str(), Some("uwsm-app" | "uwsm" | "app2unit")) {
        return forward_service(name, args, &helper);
    }
    if args.len() == 1 {
        match args[0].to_str() {
            Some("--capture") => {
                println!("{}", ipc::request("capture"));
                return Ok(());
            }
            Some("--status") => {
                println!("{}", ipc::request("status"));
                return Ok(());
            }
            Some("-h" | "--help") => {
                println!("{USAGE}");
                return Ok(());
            }
            _ => (),
        }
    }
    // Reject malformed command lines before capturing or consuming a one-shot
    // destination. Failed usage must not alter a launcher's pending context.
    let payload = if args
        .first()
        .is_some_and(|arg| arg == "--context" || arg == "--launch-token")
    {
        args.get(2..).unwrap_or_default()
    } else {
        &args[..]
    };
    if payload.len() < 2 || payload[0] != "--" || payload[1].is_empty() {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, USAGE));
    }
    let token = if args.len() >= 2 && args[0] == "--context" {
        consume_context(&args[1])
    } else if args.len() >= 2 && args[0] == "--launch-token" {
        args[1].clone()
    } else if args.first().is_some_and(|s| s == "--") {
        consume_context(OsStr::new(&ipc::request("capture")))
    } else {
        OsString::new()
    };
    Err(launch_command(&payload[1..], &token, &helper)?.exec())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("hyprspace-launch: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_bounded_ascii_tokens_are_accepted() {
        for token in ["token-0123", "A", &"a".repeat(128)] {
            assert!(valid_token(OsStr::new(token)));
        }
        for token in ["", "has space", "new\nline", "é", &"a".repeat(129)] {
            assert!(!valid_token(OsStr::new(token)));
        }
    }

    #[test]
    fn launcher_payload_boundaries_remain_native() {
        let offset = |name: &str, args: &[&str]| {
            payload_offset(
                OsStr::new(name),
                &args.iter().map(OsString::from).collect::<Vec<_>>(),
            )
        };
        assert_eq!(offset("uwsm-app", &["-s", "a", "--", "app", "--"]), Some(3));
        assert_eq!(offset("uwsm", &["app", "--", "app"]), Some(2));
        assert_eq!(offset("uwsm", &["stop", "--", "app"]), None);
        assert_eq!(offset("uwsm-app", &["app", "--", "arg"]), None);
        assert_eq!(offset("app2unit", &["app", "--", "arg"]), Some(0));
        assert_eq!(offset("app2unit", &["--", "app"]), Some(1));
        assert_eq!(offset("app2unit", &["--", "app.desktop:NewWindow"]), None);
        assert_eq!(offset("app2unit", &["--unknown"]), None);
        assert_eq!(offset("app2unit", &["--"]), None);
    }
}
