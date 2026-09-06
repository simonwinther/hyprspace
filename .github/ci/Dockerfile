FROM archlinux:base-devel@sha256:b21289eb1954872de0dc9f88976627e38611b1817be75e50946c83ab7b9c474d

# Keep the compositor and its libraries together at a tested Arch snapshot.
RUN printf 'Server = https://archive.archlinux.org/repos/2026/09/01/$repo/os/$arch\n' > /etc/pacman.d/mirrorlist \
    && pacman-key --init \
    && pacman-key --populate archlinux \
    && pacman -Syyuu --noconfirm --needed hyprland cairo pango gdk-pixbuf2 librsvg nlohmann-json git jq python ripgrep \
    && test "$(pkg-config --modversion hyprland)" = 0.56.2 \
    && pacman -Scc --noconfirm

WORKDIR /src
COPY . .
RUN make -j2 && make test && make release-check && python3 test/test_release.py
