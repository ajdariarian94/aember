FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates cmake ninja-build g++ zlib1g-dev pkg-config && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch v1.1 --recurse-submodules --shallow-submodules https://github.com/google/bloaty.git /src \
 && cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release -DBLOATY_ENABLE_RE2=ON \
 && cmake --build /b -j4 --target bloaty && cp /b/bloaty /usr/local/bin/ && rm -rf /b /src
ENTRYPOINT ["bloaty"]
