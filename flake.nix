{
  description = "Live workspace overview and Alt+Tab switcher for Hyprland";

  inputs.hyprland.url = "github:hyprwm/Hyprland/efb50993780079460b0cbed1363e2166a2de1d9f";

  outputs = { self, hyprland }:
    let
      system = "x86_64-linux";
      supportedRevision = "efb50993780079460b0cbed1363e2166a2de1d9f";
      hookCompilerFlags = "-fcf-protection=full";
      # The supported input locks Glaze 8, but its CMake requires Glaze 7.
      # Keep the compositor source and compiler while fixing that build input.
      compositor = (hyprland.packages.${system}.hyprland.override {
        glaze-hyprland = pkgs.glaze-hyprland.overrideAttrs {
          version = "7.2.0";
          src = pkgs.fetchFromGitHub {
            owner = "stephenberry";
            repo = "glaze";
            tag = "v7.2.0";
            hash = "sha256-f3NVRi3SXKo42hn0WCw7JsOK3EkdOVJIcuzhPorKjFY=";
          };
        };
      }).overrideAttrs (old: {
        # Match Arch's function entries: the upstream hook trampoline cannot
        # relocate an early relative call without branch-protection padding.
        env = old.env // {
          NIX_CFLAGS_COMPILE = (old.env.NIX_CFLAGS_COMPILE or "") + " ${hookCompilerFlags}";
        };
      });
      pkgs = import hyprland.inputs.nixpkgs {
        inherit system;
        overlays = [ hyprland.overlays.hyprland-packages ];
      };
      # Override the package set so the builder itself uses the compositor's
      # compiler, as well as passing its headers and libraries to the plugin.
      mkHyprlandPlugin = (pkgs.hyprlandPlugins.override {
        hyprland = compositor;
      }).mkHyprlandPlugin;
    in {
      packages.${system} = rec {
        hyprspace =
          if (hyprland.rev or "") != supportedRevision then
            throw "hyprspace supports only Hyprland 0.56.2 (${supportedRevision}); pin your Hyprland input to that revision"
          else mkHyprlandPlugin {
            pluginName = "hyprspace";
            version = pkgs.lib.removeSuffix "\n" (builtins.readFile ./version.txt);
            src = pkgs.lib.fileset.toSource {
              root = ./.;
              fileset = pkgs.lib.fileset.unions [
                ./Makefile ./src ./scripts/atomic-output.sh
                ./contrib/hyprspace-launch ./contrib/bindings.conf
              ];
            };
            buildInputs = with pkgs; [ cairo pango gdk-pixbuf librsvg nlohmann_json ];
            nativeBuildInputs = [ pkgs.python3 ];
            dontUseCmakeConfigure = true;
            dontUseMesonConfigure = true;
            enableParallelBuilding = true;
            env.NIX_CFLAGS_COMPILE = hookCompilerFlags;
            makeFlags = [ "CXX=${compositor.stdenv.cc.targetPrefix}g++" ];
            installPhase = ''
              runHook preInstall
              install -Dm755 build/hyprspace.so "$out/lib/libhyprspace.so"
              install -Dm755 contrib/hyprspace-launch "$out/lib/hyprspace-launch"
              substituteInPlace "$out/lib/hyprspace-launch" \
                --replace-fail '#!/usr/bin/env python3' '#!${pkgs.python3}/bin/python3'
              mkdir -p "$out/lib/launch-bin" "$out/bin"
              for name in uwsm-app uwsm app2unit; do
                ln -s ../hyprspace-launch "$out/lib/launch-bin/$name"
              done
              ln -s ../lib/hyprspace-launch "$out/bin/hyprspace-launch"
              install -Dm644 contrib/bindings.conf "$out/share/hyprspace/bindings.conf"
              runHook postInstall
            '';
            passthru = { inherit compositor supportedRevision; };
            meta = {
              description = "Live workspace overview and Alt+Tab switcher for Hyprland";
              homepage = "https://github.com/simonwinther/hyprspace";
              license = pkgs.lib.licenses.mit;
              platforms = [ system ];
            };
          };
        default = hyprspace;
      };
      checks.${system}.package = self.packages.${system}.hyprspace;
    };
}
