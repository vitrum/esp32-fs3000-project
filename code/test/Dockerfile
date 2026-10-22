FROM espressif/idf:v4.4.6

RUN apt-get update \
    && apt-get install -y --no-install-recommends python3-venv \
    && rm -rf /var/lib/apt/lists/*

COPY test/requirements.txt /opt/aeroprobe-test-requirements.txt
RUN --mount=type=cache,target=/root/.cache/pip python -m venv /opt/pytest-venv \
    && /opt/pytest-venv/bin/pip install --timeout 120 --retries 5 pip==25.0.1 \
    && /opt/pytest-venv/bin/pip install --timeout 120 --retries 5 -r /opt/aeroprobe-test-requirements.txt

RUN apt-get update \
    && apt-get install -y --no-install-recommends libpixman-1-0 libsdl2-2.0-0 libslirp0 \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
RUN case "$TARGETARCH" in \
      arm64) architecture=aarch64; checksum=00de5985094c14e47d1b38464a006b8ed64fd0fa7a289c56da24f3a329f65339 ;; \
      amd64) architecture=x86_64; checksum=0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35 ;; \
      *) exit 1 ;; \
    esac \
    && curl -fsSL --retry 3 "https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-${architecture}-linux-gnu.tar.xz" -o /tmp/qemu.tar.xz \
    && printf '%s  %s\n' "$checksum" /tmp/qemu.tar.xz | sha256sum -c - \
    && mkdir -p /opt/esp-qemu \
    && tar -xJf /tmp/qemu.tar.xz -C /opt/esp-qemu --strip-components=1 \
    && rm /tmp/qemu.tar.xz \
    && /opt/esp-qemu/bin/qemu-system-xtensa --version

RUN git init /opt/test-deps/TinyGPSPlus \
    && git -C /opt/test-deps/TinyGPSPlus remote add origin https://github.com/mikalhart/TinyGPSPlus.git \
    && git -C /opt/test-deps/TinyGPSPlus fetch --depth 1 origin 0a205759da22a1c54c5f5285480fe6132592a4e2 \
    && git -C /opt/test-deps/TinyGPSPlus checkout --detach FETCH_HEAD
ENV GPS_SOURCE_DIR=/opt/test-deps/TinyGPSPlus/src
ENV PATH=/opt/esp-qemu/bin:${PATH}

WORKDIR /project