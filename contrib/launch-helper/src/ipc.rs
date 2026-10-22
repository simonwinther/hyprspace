use std::{
    env, io, mem,
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd},
        unix::ffi::OsStrExt,
    },
    path::{Component, PathBuf},
    time::{Duration, Instant},
};

const LIMIT: usize = 1_048_576;

fn wait(fd: &OwnedFd, events: i16, end: Instant) -> io::Result<()> {
    loop {
        let remaining = end
            .checked_duration_since(Instant::now())
            .ok_or_else(|| io::Error::from(io::ErrorKind::TimedOut))?;
        let mut descriptor = libc::pollfd {
            fd: fd.as_raw_fd(),
            events,
            revents: 0,
        };
        let millis = remaining.as_millis().max(1) as i32;
        // Only pollfd's one initialized entry is exposed to libc.
        let result = unsafe { libc::poll(&mut descriptor, 1, millis) };
        if result > 0 {
            return Ok(());
        }
        if result == 0 {
            return Err(io::ErrorKind::TimedOut.into());
        }
        let error = io::Error::last_os_error();
        if error.kind() != io::ErrorKind::Interrupted {
            return Err(error);
        }
    }
}

fn exchange(message: &[u8]) -> io::Result<String> {
    if message.len() > LIMIT {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    let runtime = env::var_os("XDG_RUNTIME_DIR")
        .filter(|v| !v.is_empty())
        .ok_or_else(|| io::Error::from(io::ErrorKind::NotFound))?;
    let instance = env::var_os("HYPRLAND_INSTANCE_SIGNATURE")
        .filter(|v| !v.is_empty())
        .ok_or_else(|| io::Error::from(io::ErrorKind::NotFound))?;
    let mut components = std::path::Path::new(&instance).components();
    if !matches!(components.next(), Some(Component::Normal(_)))
        || components.next().is_some()
        || instance.as_bytes().contains(&b'/')
    {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    let path = PathBuf::from(runtime)
        .join("hypr")
        .join(instance)
        .join("hyprspace.sock");
    // sockaddr_un is a C POD; zero initialization supplies the trailing NUL.
    let mut address: libc::sockaddr_un = unsafe { mem::zeroed() };
    address.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let path = path.as_os_str().as_bytes();
    if path.len() >= address.sun_path.len() || path.contains(&0) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    for (to, from) in address.sun_path.iter_mut().zip(path) {
        *to = *from as libc::c_char;
    }
    let raw = unsafe {
        libc::socket(
            libc::AF_UNIX,
            libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
            0,
        )
    };
    if raw < 0 {
        return Err(io::Error::last_os_error());
    }
    // socket returned a fresh owned descriptor; OwnedFd closes every return path.
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let end = Instant::now() + Duration::from_millis(300);
    loop {
        let result = unsafe {
            libc::connect(
                raw,
                (&address as *const libc::sockaddr_un).cast(),
                mem::size_of_val(&address) as libc::socklen_t,
            )
        };
        if result == 0 {
            break;
        }
        let error = io::Error::last_os_error();
        match error.raw_os_error() {
            Some(libc::EINTR | libc::EAGAIN) => {
                wait(&fd, libc::POLLOUT, end)?;
                if Instant::now() >= end {
                    return Err(io::ErrorKind::TimedOut.into());
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            Some(libc::EINPROGRESS | libc::EALREADY) => {
                wait(&fd, libc::POLLOUT, end)?;
                let mut error = 0;
                let mut size = mem::size_of_val(&error) as libc::socklen_t;
                let result = unsafe {
                    libc::getsockopt(
                        raw,
                        libc::SOL_SOCKET,
                        libc::SO_ERROR,
                        (&mut error as *mut libc::c_int).cast(),
                        &mut size,
                    )
                };
                if result < 0 {
                    return Err(io::Error::last_os_error());
                }
                if error != 0 {
                    return Err(io::Error::from_raw_os_error(error));
                }
                break;
            }
            _ => return Err(error),
        }
    }
    loop {
        let sent = unsafe {
            libc::send(
                raw,
                message.as_ptr().cast(),
                message.len(),
                libc::MSG_NOSIGNAL,
            )
        };
        if sent >= 0 {
            if sent as usize != message.len() {
                return Err(io::ErrorKind::WriteZero.into());
            }
            break;
        }
        let error = io::Error::last_os_error();
        if !matches!(
            error.kind(),
            io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
        ) {
            return Err(error);
        }
        wait(&fd, libc::POLLOUT, end)?;
    }
    let mut bytes = vec![0; LIMIT];
    loop {
        let received =
            unsafe { libc::recv(raw, bytes.as_mut_ptr().cast(), bytes.len(), libc::MSG_TRUNC) };
        if received >= 0 {
            if received as usize > LIMIT {
                return Err(io::ErrorKind::InvalidData.into());
            }
            bytes.truncate(received as usize);
            return String::from_utf8(bytes)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e));
        }
        let error = io::Error::last_os_error();
        if !matches!(
            error.kind(),
            io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
        ) {
            return Err(error);
        }
        wait(&fd, libc::POLLIN, end)?;
    }
}

// A missing, stopped, or incompatible plugin retains normal application launching.
pub fn request(message: &str) -> String {
    exchange(message.as_bytes()).unwrap_or_default()
}
