FROM debian:12.11-slim@sha256:b1a741487078b369e78119849663d7f1a5341ef2768798f7b7406c4240f86aef

LABEL org.opencontainers.image.title="ec-systemcore ARM64 build toolchain"
LABEL org.opencontainers.image.description="Pinned Debian Bookworm cross-build and release validation environment"
LABEL org.opencontainers.image.licenses="GPL-3.0-only"

ENV DEBIAN_FRONTEND=noninteractive
ENV BUN_VERSION=1.3.14
ENV JAVA_VERSION=25.0.3+9
ENV JAVA_HOME=/opt/java/openjdk
ENV CMAKE_VERSION=3.31.8
ENV ACTIONLINT_VERSION=1.7.12
ENV SYFT_VERSION=1.44.0
ENV OPKG_UTILS_COMMIT=d179a334f7bfbe55dec4839607dac2c38f6b7c8f

# Point every package install at one immutable Debian snapshot rather than a
# moving mirror. The Debian 12.11 base and all subsequent package installs are
# held to the same Bookworm generation and one dated repository view.
# The slim image has no CA bundle yet, so bootstrap over HTTP; apt still
# verifies the signed Debian repository metadata and package hashes.
RUN rm -f /etc/apt/sources.list.d/debian.sources \
 && printf '%s\n' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260720T000000Z bookworm main' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20260720T000000Z bookworm-security main' \
      > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends \
      bash \
      binutils \
      binutils-aarch64-linux-gnu \
      ca-certificates \
      cppcheck \
      curl \
      file \
      g++-aarch64-linux-gnu \
      gcc-aarch64-linux-gnu \
      git \
      gzip \
      jq \
      ninja-build \
      patch \
      pkg-config \
      python3 \
      qemu-user \
      ripgrep \
      shellcheck \
      systemd \
      tar \
      unzip \
      xz-utils \
      zstd \
 && rm -rf /var/lib/apt/lists/* \
 && rm -f /etc/machine-id \
 && touch /etc/machine-id

# WPILib 2027 targets Java 25. Debian Bookworm intentionally supplies the
# conservative target libc/toolchain, so install one immutable official
# Eclipse Temurin archive instead of widening the Debian package baseline.
RUN curl -fsSLo /tmp/temurin-jdk.tar.gz \
      "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.3%2B9/OpenJDK25U-jdk_x64_linux_hotspot_25.0.3_9.tar.gz" \
 && echo "69264a7a211bf5029830d07bc3370f879769d62ebc5b5488e90c9343a2da0e1f  /tmp/temurin-jdk.tar.gz" \
      | sha256sum --check --strict \
 && mkdir -p "${JAVA_HOME}" \
 && tar -xzf /tmp/temurin-jdk.tar.gz --strip-components=1 -C "${JAVA_HOME}" \
 && rm /tmp/temurin-jdk.tar.gz \
 && "${JAVA_HOME}/bin/java" -version \
 && "${JAVA_HOME}/bin/javac" -version

ENV PATH="${JAVA_HOME}/bin:${PATH}"

RUN curl -fsSLo /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
 && echo "630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0  /tmp/cmake.tar.gz" \
      | sha256sum --check --strict \
 && mkdir -p /opt/cmake \
 && tar -xzf /tmp/cmake.tar.gz --strip-components=1 -C /opt/cmake \
 && ln -s /opt/cmake/bin/cmake /usr/local/bin/cmake \
 && ln -s /opt/cmake/bin/ctest /usr/local/bin/ctest \
 && ln -s /opt/cmake/bin/cpack /usr/local/bin/cpack \
 && rm /tmp/cmake.tar.gz

RUN curl -fsSLo /tmp/bun.zip \
      "https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/bun-linux-x64.zip" \
 && echo "951ee2aee855f08595aeec6225226a298d3fea83a3dcd6465c09cbccdf7e848f  /tmp/bun.zip" \
      | sha256sum --check --strict \
 && unzip -q /tmp/bun.zip -d /tmp/bun \
 && install -m 0755 /tmp/bun/bun-linux-x64/bun /usr/local/bin/bun \
 && rm -rf /tmp/bun /tmp/bun.zip

COPY .bun-arm64-zip.sha256 /tmp/bun-arm64-zip.sha256

RUN curl -fsSLo /tmp/bun-arm64.zip \
      "https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/bun-linux-aarch64.zip" \
 && echo "$(tr -d '\r\n' < /tmp/bun-arm64-zip.sha256)  /tmp/bun-arm64.zip" \
      | sha256sum --check --strict \
 && unzip -q /tmp/bun-arm64.zip -d /tmp/bun-arm64 \
 && install -d -m 0755 /opt/ec-systemcore-target-runtime \
 && install -m 0755 /tmp/bun-arm64/bun-linux-aarch64/bun \
      /opt/ec-systemcore-target-runtime/bun \
 && bun_archive_sha256="$(tr -d '\r\n' < /tmp/bun-arm64-zip.sha256)" \
 && bun_executable_sha256="$(sha256sum /opt/ec-systemcore-target-runtime/bun | awk '{ print $1 }')" \
 && printf '%s\n' "${bun_executable_sha256}" \
      > /opt/ec-systemcore-target-runtime/bun.sha256 \
 && printf '%s\n' \
      "{\"version\":\"${BUN_VERSION}\",\"asset\":\"bun-linux-aarch64.zip\",\"archive_sha256\":\"${bun_archive_sha256}\",\"executable_sha256\":\"${bun_executable_sha256}\"}" \
      > /opt/ec-systemcore-target-runtime/bun-provenance.json \
 && chmod 0644 /opt/ec-systemcore-target-runtime/bun.sha256 \
      /opt/ec-systemcore-target-runtime/bun-provenance.json \
 && rm -rf /tmp/bun-arm64 /tmp/bun-arm64.zip /tmp/bun-arm64-zip.sha256

RUN curl -fsSLo /tmp/actionlint.tar.gz \
      "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz" \
 && echo "8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8  /tmp/actionlint.tar.gz" \
      | sha256sum --check --strict \
 && tar -xzf /tmp/actionlint.tar.gz -C /usr/local/bin actionlint \
 && rm /tmp/actionlint.tar.gz

RUN curl -fsSLo /tmp/syft.tar.gz \
      "https://github.com/anchore/syft/releases/download/v${SYFT_VERSION}/syft_${SYFT_VERSION}_linux_amd64.tar.gz" \
 && echo "0e91737aee2b5baf1d255b959630194a302335d848ff97bb07921eb6205b5f5a  /tmp/syft.tar.gz" \
      | sha256sum --check --strict \
 && tar -xzf /tmp/syft.tar.gz -C /usr/local/bin syft \
 && rm /tmp/syft.tar.gz

RUN git init /opt/opkg-utils \
 && git -C /opt/opkg-utils remote add origin https://git.yoctoproject.org/opkg-utils.git \
 && git -C /opt/opkg-utils fetch --depth 1 origin "${OPKG_UTILS_COMMIT}" \
 && git -C /opt/opkg-utils checkout --detach FETCH_HEAD \
 && test "$(git -C /opt/opkg-utils rev-parse HEAD)" = "${OPKG_UTILS_COMMIT}" \
 && ln -s /opt/opkg-utils/opkg-build /usr/local/bin/opkg-build

WORKDIR /workspace
