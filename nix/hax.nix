{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  python3,
  curl,
  jansson,
  coreutils,
  tmux ? null,
  src ? ../.,
}:

stdenv.mkDerivation {
  pname = "hax";
  version = "0.6.1";

  inherit src;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    python3
    coreutils
  ];

  postPatch = ''
    # Nix sandboxes lack FHS paths (/usr/bin/printf, /bin/mv) and block
    # the setuid bit, so make the browser and setuid tests sandbox-friendly.
    substituteInPlace tests/system/test_browser.c \
      --replace-fail '/usr/bin/printf' '${coreutils}/bin/printf' \
      --replace-fail '/bin/mv' '${coreutils}/bin/mv'
    substituteInPlace tests/tools/test_write.c \
      --replace-fail 'EXPECT(chmod(path, 04755) == 0); /* setuid + rwxr-xr-x */' \
        'if (chmod(path, 04755) != 0) { T_SKIP("setuid bit not permitted in this build environment"); }'
  '';

  buildInputs = [
    curl
    jansson
  ];

  nativeCheckInputs = lib.optionals (tmux != null) [ tmux ];

  doCheck = true;

  mesonFlags = [ ];

  meta = with lib; {
    description = "A minimalist, terminal-native coding agent written in C";
    homepage = "https://github.com/OleksandrChekhovskyi/hax";
    license = licenses.mit;
    mainProgram = "hax";
    platforms = platforms.unix;
  };
}
