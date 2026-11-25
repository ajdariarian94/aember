# Compiler Explorer (https://github.com/compiler-explorer/compiler-explorer),
# upstream at a pinned commit; `aember godbolt` builds this image and runs it
# with the aember SDK's cross compilers mounted read-only. Nothing of
# Compiler Explorer is copied into this repo.
#
# CE_COMMIT: tag gh-20105 (2026-10-31). Bump both together; the node version
# is the one in that commit's .node-version.
FROM node:22.23.4-bookworm-slim

ARG CE_COMMIT=4341ea30ac71a06cd97bd31c3c7483e5a8802901

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git make python3 rsync \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /ce
RUN git init -q . \
 && git remote add origin https://github.com/compiler-explorer/compiler-explorer.git \
 && git fetch -q --depth 1 origin "$CE_COMMIT" \
 && git checkout -q FETCH_HEAD \
 && rm -rf .git \
 && echo "$CE_COMMIT" > /ce/COMMIT
# find-node: use the image's node; prebuild = npm ci + webpack + tsc.
RUN echo "$(command -v node)" > .node-bin \
 && make prebuild \
 && npm cache clean --force \
 && chmod -R a+rwX /ce/out /ce/etc/config

# As `make run-only`: production mode (its assets; the dev ones are not built).
ENV NODE_ENV=production
EXPOSE 10240
# The c++.local.properties and compiler-explorer.local.properties that
# `aember godbolt` writes are bind-mounted into /ce/etc/config.
CMD ["node", "./out/dist/app.js", "--static", "./out/webpack/static", "--language", "c++", "--port", "10240"]
