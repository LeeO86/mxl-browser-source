# syntax=docker/dockerfile:1
# mxl-browser-source (SPEC.md §15.1). Pins: IMPLEMENTATION_PLAN.md §1. The CEF pin is here
# and in .github/workflows/ci.yaml (keep in sync).
#
# Behind a TLS-intercepting proxy, pass EXTRA_CA_CERT_B64 (a base64 PEM) and the BuildKit
# proxy args; every downloading stage trusts that CA, the runtime stage does not keep it.

# ARGs used in FROM lines must come before the first FROM.
ARG UBUNTU=ubuntu:24.04

FROM node:22-bookworm AS webui
ARG EXTRA_CA_CERT_B64=""
RUN if [ -n "$EXTRA_CA_CERT_B64" ]; then echo "$EXTRA_CA_CERT_B64" | base64 -d > /usr/local/share/ca-certificates/extra.crt && update-ca-certificates; fi
ENV NODE_EXTRA_CA_CERTS=/etc/ssl/certs/ca-certificates.crt
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ${UBUNTU} AS cef
ARG CEF_VERSION=144.0.21+g4f5b28c+chromium-144.0.7559.248
ARG CEF_SHA1=e6233049cec12ba108b4b920b499e2a082ffd0ae
ARG EXTRA_CA_CERT_B64=""
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl bzip2 build-essential cmake ninja-build python3 \
    && rm -rf /var/lib/apt/lists/*
RUN if [ -n "$EXTRA_CA_CERT_B64" ]; then echo "$EXTRA_CA_CERT_B64" | base64 -d > /usr/local/share/ca-certificates/extra.crt && update-ca-certificates; fi
COPY docker/fetch.sh /usr/local/bin/fetch.sh
WORKDIR /opt
# libcef.so ships with debug symbols (1.5 GB): stripped, the dynamic symbols stay for linking.
RUN url="https://cef-builds.spotifycdn.com/cef_binary_$(echo "$CEF_VERSION" | sed 's/+/%2B/g')_linux64_minimal.tar.bz2" \
    && sh /usr/local/bin/fetch.sh "$url" cef.tar.bz2 "$CEF_SHA1" \
    && mkdir cef && tar xjf cef.tar.bz2 -C cef --strip-components=1 && rm cef.tar.bz2 \
    && strip --strip-unneeded cef/Release/libcef.so \
    && cmake -S cef -B cef/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && ninja -C cef/build libcef_dll_wrapper \
    && rm -rf cef/build/libcef_dll_wrapper/CMakeFiles

# Everything the build needs except this repository (cached; also the incremental dev image).
FROM ${UBUNTU} AS deps
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62
ARG EXTRA_CA_CERT_B64=""
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates \
        bison flex autoconf automake libtool \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev uuid-dev libavahi-compat-libdnssd-dev libsamplerate0-dev \
    && rm -rf /var/lib/apt/lists/*
RUN if [ -n "$EXTRA_CA_CERT_B64" ]; then echo "$EXTRA_CA_CERT_B64" | base64 -d > /usr/local/share/ca-certificates/extra.crt && update-ca-certificates; fi

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg
RUN git init /tmp/mxl \
    && git -C /tmp/mxl remote add origin https://github.com/dmf-mxl/mxl.git \
    && git -C /tmp/mxl fetch --depth 1 origin "${MXL_REF}" \
    && git -C /tmp/mxl checkout FETCH_HEAD
COPY docker/mxl-vcpkg.json /tmp/mxl-manifest/vcpkg.json
RUN BASELINE="$(git -C /opt/vcpkg rev-parse HEAD)" \
    && sed -i "1a\\  \"builtin-baseline\": \"${BASELINE}\"," /tmp/mxl-manifest/vcpkg.json
RUN cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/tmp/mxl-manifest \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build
RUN git init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp checkout FETCH_HEAD
COPY --from=cef /opt/cef /opt/cef
# libcef.so is linked against these (the link resolves its dependencies).
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libcups2t64 libdrm2 libgbm1 libxkbcommon0 \
        libxcomposite1 libxdamage1 libxext6 libxfixes3 libxrandr2 libxshmfence1 libasound2t64 libpango-1.0-0 \
        libcairo2 libx11-6 libx11-xcb1 libxcb1 libdbus-1-3 libexpat1 libglib2.0-0t64 \
    && rm -rf /var/lib/apt/lists/*

FROM deps AS build
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62
COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DMBS_WITH_CEF=ON -DCEF_ROOT=/opt/cef -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
        -DMBS_MXL_REVISION="${MXL_REF}" -DMBS_NMOS_CPP_REF="${NMOS_CPP_REF}" \
    && cmake --build /src/build -j"$(nproc)" \
    && /src/build/unit-tests \
    && gcc -shared -fPIC -O2 -o /src/build/libmallinfo_shim.so /src/docker/mallinfo_shim.c

FROM ${UBUNTU} AS runtime
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG CEF_VERSION=144.0.21+g4f5b28c+chromium-144.0.7559.248
# CEF's libraries without GTK or X (spike S3: headless Ozone), certutil, fonts; libnss-mdns resolves
# the .local names of DNS-SD services (NMOS_DNS_SD=true; unused when it is off).
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libnss3 libnspr4 libnss3-tools libatk1.0-0t64 libatk-bridge2.0-0t64 libcups2t64 libdrm2 libgbm1 libxkbcommon0 \
        libxcomposite1 libxdamage1 libxext6 libxfixes3 libxrandr2 libxshmfence1 libasound2t64 libpango-1.0-0 \
        libcairo2 libx11-6 libx11-xcb1 libxcb1 libdbus-1-3 libexpat1 libegl1 libgl1 libgles2 libvulkan1 libglib2.0-0t64 \
        fontconfig fonts-dejavu-core fonts-liberation2 fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 libssl3t64 libavahi-compat-libdnssd1 libnss-mdns libsamplerate0 \
    && rm -rf /var/lib/apt/lists/*
# CEF next to the binaries (resources and locales are found relative to the executable).
COPY --from=cef /opt/cef/Release/ /usr/local/lib/mxl-browser-source/
COPY --from=cef /opt/cef/Resources/ /usr/local/lib/mxl-browser-source/
COPY --from=build /src/build/mxl-browser-source /src/build/mxl-browser-source-helper /src/build/libmallinfo_shim.so /usr/local/lib/mxl-browser-source/
COPY --from=build /src/build/grain_reader /usr/local/bin/mxl-bs-grain-reader
COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /tmp/mxl/build/vcpkg_installed/x64-linux/lib/ /usr/local/lib/
COPY templates/ /usr/local/share/mxl-browser-source/templates/
# The NVIDIA container toolkit injects libEGL_nvidia but not the glvnd vendor file; without it
# EGL falls back to llvmpipe (spike S1).
RUN mkdir -p /usr/share/glvnd/egl_vendor.d \
    && printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0"}}\n' > /usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    && ln -s /usr/local/lib/mxl-browser-source/mxl-browser-source /usr/local/bin/mxl-browser-source \
    && ldconfig \
    && if ! getent passwd 1000 >/dev/null; then useradd --uid 1000 --no-create-home --shell /usr/sbin/nologin mxl; fi \
    && mkdir -p /config /tmp/home && chown 1000:1000 /config /tmp/home
LABEL io.dmf.mxl.revision="${MXL_REF}" \
      org.opencontainers.image.licenses="MIT" \
      io.mxl.cef.version="${CEF_VERSION}"
ENV HOME=/tmp/home \
    LD_PRELOAD=/usr/local/lib/mxl-browser-source/libmallinfo_shim.so \
    NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics
USER 1000:1000
EXPOSE 8160 3312 3313
ENTRYPOINT ["/usr/local/lib/mxl-browser-source/mxl-browser-source"]
