#!/bin/sh
# Starts room on loopback inside container, then sshd in front of it
set -eu

mkdir -p /etc/ssh/host
if [ ! -f /etc/ssh/host/ssh_host_ed25519_key ]; then
    ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/host/ssh_host_ed25519_key
fi

if ! ls /etc/ssh/authorized_keys/call >/dev/null 2>&1; then
    echo "no /etc/ssh/authorized_keys/call: mount a file with the public keys" >&2
    echo "of the people allowed in (see docker-compose.yml)" >&2
    exit 1
fi

/usr/bin/asciicam --room 127.0.0.1:7100 &
exec /usr/sbin/sshd -D -e -f /etc/ssh/sshd_config
