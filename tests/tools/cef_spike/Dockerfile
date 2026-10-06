FROM ubuntu:24.04
ARG CEF_VERSION=144.0.21+g4f5b28c+chromium-144.0.7559.248
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl bzip2 build-essential cmake ninja-build python3 \
      libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libcups2t64 libdrm2 libgbm1 libxkbcommon0 \
      libxcomposite1 libxdamage1 libxext6 libxfixes3 libxrandr2 libxshmfence1 libasound2t64 libpango-1.0-0 \
      libcairo2 libx11-6 libx11-xcb1 libxcb1 libdbus-1-3 libexpat1 libegl1 libgl1 libgles2 libvulkan1 libglib2.0-0t64 \
      fonts-dejavu-core && rm -rf /var/lib/apt/lists/*
WORKDIR /opt
COPY fetch.sh /usr/local/bin/fetch.sh
RUN url="https://cef-builds.spotifycdn.com/cef_binary_$(echo "$CEF_VERSION" | sed 's/+/%2B/g')_linux64_minimal.tar.bz2" \
 && sh /usr/local/bin/fetch.sh "$url" cef.tar.bz2 "$(curl --http1.1 -fsSL "$url.sha1")" \
 && mkdir cef && tar xjf cef.tar.bz2 -C cef --strip-components=1 && rm cef.tar.bz2
RUN cmake -S cef -B cef/build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C cef/build libcef_dll_wrapper
COPY spike.cpp spike.html /opt/spike/
RUN cp -a cef/Release/. /opt/spike/ && cp -a cef/Resources/. /opt/spike/ \
 && g++ -std=c++20 -O2 -DNDEBUG -I/opt/cef -o /opt/spike/spike /opt/spike/spike.cpp \
      /opt/cef/build/libcef_dll_wrapper/libcef_dll_wrapper.a -L/opt/spike -lcef -Wl,-rpath,'$ORIGIN' -lpthread \
 && (ldd /opt/spike/libcef.so | grep -i "not found" || true)
WORKDIR /opt/spike
ENTRYPOINT ["/opt/spike/spike"]
