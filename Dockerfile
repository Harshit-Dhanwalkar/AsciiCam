#   docker build -t asciicam .
#   docker run -it --rm asciicam                          # built-in demo picture
#   docker run -it --rm --device /dev/video0 asciicam -d /dev/video0 -C  # webcam
#
# Plugins are loaded from /usr/share/asciicam/plugins with program's own ELF loader
# (press `/` to pick one); compiling new ones needs SDK (`./nb install-sdk`), which
# this image leaves out to stay small

FROM debian:12-slim AS build
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends gcc libc6-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY C/ .
# Built with C build program: image build needs a compiler and nothing else
RUN cc -o nb build.c && \
    ./nb -j"$(nproc)" install PREFIX=/usr DESTDIR=/rootfs && \
    mkdir -p /rootfs/tmp && chmod 1777 /rootfs/tmp

FROM scratch
COPY --from=build /rootfs/ /
ENV TERM=xterm-256color
ENTRYPOINT ["/usr/bin/asciicam"]
CMD ["--demo", "-C"]
