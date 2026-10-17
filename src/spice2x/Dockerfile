FROM node:22-bookworm-slim AS controller-build
WORKDIR /src/controller
RUN apt-get update \
    && apt-get install --no-install-recommends -y git \
    && rm -rf /var/lib/apt/lists/*
COPY --from=gitroot . /src/.git
COPY --from=controller package.json package-lock.json index.html vite.config.ts tsconfig.json ./
COPY --from=controller src ./src
COPY --from=controller public ./public
RUN npm ci && npm run build:embedded

FROM spicetools/deps
WORKDIR /src

COPY --from=gitroot . /src/.git
COPY --from=controller-build /src/controller/dist/embedded/index.html /src/controller/dist/embedded/index.html
COPY . /src/src/spice2x
WORKDIR /src/src/spice2x
ENTRYPOINT ["./build_all.sh"]
