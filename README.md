# Leafturn

Leafturn is a lightweight Linux PDF reader written in C. It is inspired by the *library + saved progress* model of LibreCrate and the *physical paper page-turn* presentation of Turn the Page, but it is a fresh implementation rather than a source-code port.

## Goals

- Fast startup and page navigation.
- PDF-only scope to keep the code and runtime small.
- Book-like single-page reader with animated turns.
- Remember last page and percentage read per PDF.
- Keep the original PDF where it is; Leafturn stores only library metadata.
- No accounts, network services, tracking, or bundled PDF engine.

## Dependencies

- C11 compiler.
- GTK 3 for the desktop UI.
- MuPDF for PDF parsing and rendering.
- Cairo (via GTK) for compositing rendered pages and the page-turn effect.

MuPDF's C library is designed as a fast, lightweight embeddable rendering core; Leafturn uses it as an external dependency rather than copying its source into this repository. The Nix package disables MuPDF features Leafturn does not need (networking and X11/GL viewer support) to keep the dependency closure smaller.

## Build on NixOS

With flakes:

```sh
nix build .
./result/bin/leafturn
```

Run directly:

```sh
nix run . -- ~/Documents/book.pdf
```

Development shell:

```sh
nix develop
cmake -S . -B build
cmake --build build -j2
```

Without flakes:

```sh
nix-build -E 'with import <nixpkgs> {}; callPackage ./package.nix {}'
```

## Usage

Open a PDF from the `+ Open PDF` button or pass a PDF file as an argument.

Controls:

- Left click left/right side of the page: previous/next page.
- Mouse wheel: previous/next page.
- `Left` / `Right`: previous/next page.
- `Page Up` / `Page Down`, `Backspace` / `Space`: page turns.
- `Up` / `Down`: zoom in/out.
- `Home` / `End`: first / last page.
- `+` / `-`: zoom.
- `0`: reset zoom.
- `Esc` or the `Home` button: return to the PDF library.

Library state is stored under the XDG state directory, normally:

```text
~/.local/state/leafturn/library.txt
```

The file contains only URL-style-escaped path/title fields plus page information and a timestamp.

## Scope of this first version

The reader is deliberately narrower than the Android projects used as inspiration. It currently focuses on PDFs and does not implement encryption-at-rest, EPUB/FB2, full-text search, annotations, or a database. Those omissions are intentional to keep the Linux PC application small and fast.

## Licensing

Leafturn is distributed under AGPL-3.0-or-later in this initial implementation because it links to MuPDF. Review the exact MuPDF licensing terms for your intended redistribution model; Artifex also offers alternative commercial licensing.
