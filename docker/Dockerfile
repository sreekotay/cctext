# Low-end Linux smoke host: Ubuntu 24.04, ccc from concurrent-c, full @smoke.
# Run with docker/run-smoke.sh (1 CPU, 2 GiB RAM by default).
#
#   ./docker/run-smoke.sh
#   CPUS=1 MEMORY=1536m ./docker/run-smoke.sh

FROM ubuntu:24.04

ARG CCC_REF=
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git python3 time \
    && rm -rf /var/lib/apt/lists/*

ENV PREFIX=/opt/ccc
ENV PATH="/opt/ccc/bin:${PATH}"
ENV CCC=ccc

RUN git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git /tmp/concurrent-c \
    && if [ -n "$CCC_REF" ]; then git -C /tmp/concurrent-c checkout "$CCC_REF"; fi \
    && PREFIX="$PREFIX" /tmp/concurrent-c/cc-install.sh --no-editor-tools --no-add-to-path \
    && rm -rf /tmp/concurrent-c \
    && ccc --version

WORKDIR /src
COPY . .

RUN mkdir -p testdata/generated out bin \
    && ./testdata/gen_large.sh --bytes 3M testdata/generated/large.txt

# Smoke runs under CPU/RAM limits via docker/run-smoke.sh (not at image build).
CMD ["./make.shcc", "@smoke"]
