# Use Arch's registry to avoid Docker Hub's unauthenticated pull limit.
FROM ghcr.io/archlinux/archlinux:base-devel-20260901.0.583369@sha256:c6728f9c62ebb58749fe34e884d24b2d331a79a552f5519ab3fdb1e10c9e9146

# Keep the compositor and its libraries together at a tested Arch snapshot.
RUN printf 'Server = https://archive.archlinux.org/repos/2026/09/01/$repo/os/$arch\n' > /etc/pacman.d/mirrorlist \
    && pacman-key --init \
    && pacman-key --populate archlinux \
    && pacman -Syyuu --noconfirm --needed hyprland cairo pango gdk-pixbuf2 librsvg ttf-liberation nlohmann-json git jq python ripgrep \
    && test "$(pkg-config --modversion hyprland)" = 0.56.2 \
    && pacman -Scc --noconfirm

WORKDIR /src
COPY . .
RUN make -j2 && make test && make release-check && python3 test/test_release.py
