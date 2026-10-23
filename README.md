# 7adesktop

A small collection of desktop apps for X11 — weather, calendar, todo, sensors, RSS reader, and more. Built to be fast and light, with minimal resource use.

## Build

```sh
make
make clean
```

Dependencies: `libX11`. Some apps additionally require `sqlite3` or `libXpm`.

## Sync (`7async`)

`7async sync` synchronizes `~/.7a/organizer.db` (todos + calendar) between
devices over ssh. The server side, `7async-serve`, is not a daemon: sshd
starts it for one session, it talks over stdin/stdout and stores records
in a single SQLite file. Needs only `sqlite3`, no X11.

Server setup (once):

```sh
useradd -m sync
install -m 755 7async-serve /usr/local/bin/
su sync -c 'mkdir -p ~/.7a ~/.ssh && chmod 700 ~/.ssh'
```

One line per device in `~sync/.ssh/authorized_keys`:

```
command="/usr/local/bin/7async-serve",restrict ssh-ed25519 AAAA... laptop
```

The database is `~sync/.7a/sync-server.db` (created on first sync; pass a
path as an argument in `command=` to change it).

Client: copy `sync.conf.sample` to `~/.7a/sync.conf`, set `server=`, then
run `7async sync` by hand or from cron. `7async status` shows the last
sync and the server cursor.

Migrating from the old Go `7asyncd`: sync every device with `7asyncd`
one last time, then point them at `7async-serve` with an empty database.
The first `7async sync` on each device pushes all local rows (last write
wins by `updated_at`), so no server-side data export is needed. An
existing Go-era `~/.7a/sync.conf` can stay: just add `server=`; the old
keys (`server_url`, `records_cursor`, ...) are ignored.
