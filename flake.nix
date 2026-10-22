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
      launchHelper = pkgs.rustPlatform.buildRustPackage {
        pname = "hyprspace-launch";
        version = "1.0.0";
        src = ./contrib/launch-helper;
        cargoLock.lockFile = ./contrib/launch-helper/Cargo.lock;
        meta.license = pkgs.lib.licenses.mit;
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
                ./contrib/launch-helper ./contrib/bindings.conf
                ./third_party/lucide
              ];
            };
            buildInputs = with pkgs; [ cairo pango gdk-pixbuf librsvg nlohmann_json ];
            dontUseCmakeConfigure = true;
            dontUseMesonConfigure = true;
            enableParallelBuilding = true;
            env.NIX_CFLAGS_COMPILE = hookCompilerFlags;
            makeFlags = [
              "CXX=${compositor.stdenv.cc.targetPrefix}g++"
              "LAUNCH_HELPER=${launchHelper}/bin/hyprspace-launch"
            ];
            installPhase = ''
              runHook preInstall
              install -Dm755 build/hyprspace.so "$out/lib/libhyprspace.so"
              install -Dm755 build/hyprspace-launch "$out/lib/hyprspace-launch"
              mkdir -p "$out/lib/launch-bin" "$out/bin"
              for name in uwsm-app uwsm app2unit; do
                ln -s ../hyprspace-launch "$out/lib/launch-bin/$name"
              done
              ln -s ../lib/hyprspace-launch "$out/bin/hyprspace-launch"
              install -Dm644 contrib/bindings.conf "$out/share/hyprspace/bindings.conf"
              install -Dm644 third_party/lucide/LICENSE "$out/share/licenses/hyprspace/lucide-LICENSE"
              runHook postInstall
            '';
            passthru = { inherit compositor supportedRevision launchHelper; };
            meta = {
              description = "Live workspace overview and Alt+Tab switcher for Hyprland";
              homepage = "https://github.com/simonwinther/hyprspace";
              license = with pkgs.lib.licenses; [ mit isc ];
              platforms = [ system ];
            };
          };
        default = hyprspace;
      };
      checks.${system}.package = self.packages.${system}.hyprspace;
    };
}
