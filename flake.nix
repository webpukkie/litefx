{
  description = "LiteFX — Ultra-lightweight PipeWire Audio DSP Equalizer & 13-Band Linear-Phase Crystalizer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "litefx";
            version = "0.3.0";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              pkg-config
              wrapGAppsHook3
            ];

            buildInputs = with pkgs; [
              pipewire
              gtk3
              zita-convolver
            ];

            makeFlags = [ "PREFIX=$(out)" ];

            doCheck = true;
            checkTarget = "test";

            meta = with pkgs.lib; {
              description = "Ultra-lightweight PipeWire Audio DSP Equalizer, 13-Band Linear-Phase Crystalizer & Microphone Enhancer";
              license = licenses.mit;
              platforms = platforms.linux;
              mainProgram = "litefx";
            };
          };
        });
    };
}
