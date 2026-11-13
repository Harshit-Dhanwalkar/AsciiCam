#   docker build -f docker/call/Dockerfile -t asciicam-call
# Or: docker compose up -d --build
#
# A room for video calls behind a forwarding-only sshd
# Build context is the repository root

FROM debian:12-slim AS build
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends gcc libc6-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY C/ .
RUN cc -o nb build.c && ./nb -j"$(nproc)" install PREFIX=/usr DESTDIR=/rootfs

FROM alpine:3.20
RUN apk add --no-cache openssh-server && \
    adduser -D -s /bin/false call && \
    passwd -u call && \
    mkdir -p /etc/ssh/authorized_keys /etc/ssh/host
COPY --from=build /rootfs/usr/bin/asciicam /usr/bin/asciicam
COPY docker/call/sshd_config /etc/ssh/sshd_config
COPY docker/call/entrypoint.sh /entrypoint.sh
EXPOSE 2222
ENTRYPOINT ["/entrypoint.sh"]
