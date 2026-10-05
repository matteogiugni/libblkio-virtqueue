# Development container image for compiling and testing libblkio
FROM fedora:43
RUN dnf install -qy meson rust cargo clang-devel python3-docutils rustfmt kcov clippy diffutils
ENV CARGO_HOME=/usr/src/libblkio/.cargo
VOLUME /usr/src/libblkio
WORKDIR /usr/src/libblkio
