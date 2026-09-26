# Head-to-head terminal editor benchmark image (see bench/editors/README.md).
#
# Build from the repository root (the build context must contain the cctext
# sources so the image builds the same tree it measures):
#
#   docker build -f bench/editors/Dockerfile -t cctext-editor-bench .
#
# Run (fixtures are generated inside the container under /fixtures; mount a
# volume there to keep them between runs, and mount results/ to get output):
#
#   docker run --rm -it \
#       -v "$PWD/bench/editors/results:/src/bench/editors/results" \
#       -v cctext-bench-fixtures:/fixtures \
#       cctext-editor-bench
#
#   # subset / quicker:
#   docker run --rm -it cctext-editor-bench \
#       sh -c 'python3 bench/editors/run.py gen --sizes small_c,text_3m,json_100m &&
#              python3 bench/editors/run.py bench --fixtures small_c,text_3m,json_100m --repeats 3'
#
# Works on linux/amd64 and linux/arm64 (Apple Silicon Docker Desktop).
# Give Docker enough RAM for the 1-2 GiB fixtures: several editors hold the
# whole file (plus per-line overhead) in memory. The harness kills an editor
# whose process tree passes BENCH_MEM_GUARD_MB (default 10240) and records
# OOM-GUARD instead.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG NVIM_VERSION=v0.12.5
ARG HELIX_VERSION=25.07.1
ARG MICRO_VERSION=2.0.15
ARG KAKOUNE_VERSION=2025.06.03
ARG CCC_REF=
ARG TARGETARCH

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl git xz-utils bzip2 time procps \
        python3 python3-pip \
        vim nano emacs-nox \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --break-system-packages pyte==0.8.2

# Release binaries: neovim, helix, micro. Kakoune is built from the release
# tarball (upstream ships no Linux binaries).
RUN set -eux; \
    arch="${TARGETARCH:-$(dpkg --print-architecture)}"; \
    case "$arch" in \
        amd64) nv=x86_64; hx=x86_64; mi=linux64-static ;; \
        arm64) nv=arm64;  hx=aarch64; mi=linux-arm64 ;; \
        *) echo "unsupported arch $arch"; exit 1 ;; \
    esac; \
    mkdir -p /opt/editors && cd /opt/editors; \
    curl -fsSL "https://github.com/neovim/neovim/releases/download/${NVIM_VERSION}/nvim-linux-${nv}.tar.gz" | tar xz; \
    mv "nvim-linux-${nv}" nvim; \
    curl -fsSL "https://github.com/helix-editor/helix/releases/download/${HELIX_VERSION}/helix-${HELIX_VERSION}-${hx}-linux.tar.xz" | tar xJ; \
    mv "helix-${HELIX_VERSION}-${hx}-linux" helix; \
    curl -fsSL "https://github.com/zyedidia/micro/releases/download/v${MICRO_VERSION}/micro-${MICRO_VERSION}-${mi}.tar.gz" | tar xz; \
    mv "micro-${MICRO_VERSION}" micro; \
    curl -fsSL "https://github.com/mawww/kakoune/releases/download/v${KAKOUNE_VERSION}/kakoune-${KAKOUNE_VERSION}.tar.bz2" | tar xj; \
    make -C "kakoune-${KAKOUNE_VERSION}" -j"$(nproc)" PREFIX=/opt/editors/kak install; \
    rm -rf "kakoune-${KAKOUNE_VERSION}"

ENV NVIM_BIN=/opt/editors/nvim/bin/nvim \
    HX_BIN=/opt/editors/helix/hx \
    MICRO_BIN=/opt/editors/micro/micro \
    KAK_BIN=/opt/editors/kak/bin/kak

# Concurrent-C toolchain, then cctext itself (release build).
ENV PREFIX=/opt/ccc
ENV PATH="/opt/ccc/bin:${PATH}"
ENV CCC=ccc
RUN git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git /tmp/concurrent-c \
    && if [ -n "$CCC_REF" ]; then git -C /tmp/concurrent-c checkout "$CCC_REF"; fi \
    && PREFIX="$PREFIX" /tmp/concurrent-c/cc-install.sh --no-editor-tools --no-add-to-path \
    && rm -rf /tmp/concurrent-c \
    && ccc --version

# Short WORKDIR on purpose: long build paths can overflow ccc link commands.
WORKDIR /src
COPY . .
RUN ./make.shcc @cctext && ./bin/cctext --version
ENV CCTEXT_BIN=/src/bin/cctext \
    BENCH_FIXTURES=/fixtures

CMD ["sh", "-c", "python3 bench/editors/run.py gen --sizes small_c,text_3m,json_100m,json_1g,json_2g && python3 bench/editors/run.py bench"]
