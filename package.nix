{ lib
, stdenv
, cmake
, pkg-config
, gtk3
, mupdf
, wrapGAppsHook3
}:

let
  mupdfLite = mupdf.override {
    enableCurl = false;
    enableGL = false;
    enableX11 = false;
  };
in
stdenv.mkDerivation {
  pname = "leafturn";
  version = "0.1.5";

  src = ./.;

  nativeBuildInputs = [
    cmake
    pkg-config
    wrapGAppsHook3
  ];

  buildInputs = [
    gtk3
    mupdfLite
  ];

  # MuPDF is used as a system dependency, not vendored into Leafturn.
  cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

  meta = {
    description = "Lightweight C PDF library and book-style reader for Linux";
    license = lib.licenses.agpl3Plus;
    mainProgram = "leafturn";
    platforms = lib.platforms.linux;
  };
}
