{
  description = "MLK-F3P-DR1M90G 电路设计竞赛项目";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      py = pkgs.python3.withPackages (
        ps: with ps; [
          numpy
          scipy
          matplotlib
          pandas
          sympy
          ipython
          pyserial
          python-pptx
          openpyxl
          pillow
        ]
      );

      hwTools = with pkgs; [
        ngspice 
        xschem 

        verilator
        iverilog
        gtkwave

        pulseview 
        sigrok-cli
        minicom 
        picocom

        poppler-utils 
        pandoc
        typst 

        git
        gnumake
        jq
        py
      ];
    in
    {
      devShells.${system} = {
        default = pkgs.mkShell {
          packages = hwTools;

          shellHook = ''
            echo "MLK-F3P-DR1M90G 设计环境"
            echo "  ngspice $(ngspice --version 2>/dev/null | head -1 | awk '{print $NF}')"
            echo "  verilator $(verilator --version 2>/dev/null | awk '{print $2}')   python $(python3 --version 2>&1 | awk '{print $2}')"
            echo
            echo "提示: Anlogic TD (Tang Dynasty) 为闭源工具，需单独安装，不在此 flake 内。"
            echo "用法: nix develop   然后 ngspice / python / xschem 即可。"
          '';
        };

        fpga = pkgs.mkShell {
          packages = with pkgs; [
            verilator
            iverilog
            gtkwave
            pulseview
            sigrok-cli
            minicom
          ];
        };
      };
    };
}

