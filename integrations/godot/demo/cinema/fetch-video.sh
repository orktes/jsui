#!/usr/bin/env bash
# Downloads the clip the cinema demo (cinema.tscn) plays on its in-game TV:
# the Big Buck Bunny trailer, Ogg Theora (what Godot's VideoStreamPlayer
# decodes), 720x400, 33 s, 4.4 MB.
#
#   Big Buck Bunny · (c) copyright 2008, Blender Foundation · www.bigbuckbunny.org
#   Licensed under Creative Commons Attribution 3.0 (CC BY 3.0).
#
# The file is not committed (cinema/.gitignore); without it the scene shows
# "NO SIGNAL" and runs everything else.
#
#   integrations/godot/demo/cinema/fetch-video.sh          the trailer
#   integrations/godot/demo/cinema/fetch-video.sh --full   the whole film
#                                                          (archive.org, 10 min, 49 MB)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="$here/big_buck_bunny_trailer.ogv"
# Wikimedia Commons: File:Big_Buck_Bunny_Trailer_400p.ogv
trailer="https://upload.wikimedia.org/wikipedia/commons/b/b3/Big_Buck_Bunny_Trailer_400p.ogv"
# Internet Archive: BigBuckBunny_328 (the film, Theora + Vorbis)
film="https://archive.org/download/BigBuckBunny_328/BigBuckBunny.ogv"

urls=("$trailer" "$film")  # the film is the trailer's fallback
if [[ "${1:-}" == "--full" ]]; then
  urls=("$film")
elif [[ -s "$out" ]]; then
  echo "already there: $out"
  exit 0
fi

for url in "${urls[@]}"; do
  echo "downloading $url"
  # Wikimedia asks for a descriptive User-Agent and rate-limits anonymous
  # clients (HTTP 429): then the archive.org copy is tried.
  if curl -fL --retry 2 --connect-timeout 20 \
      -A "jsui-godot-demo/1.0 (https://github.com/orktes/jsui)" \
      -o "$out.part" "$url"; then
    if [[ "$(head -c 4 "$out.part")" == "OggS" ]]; then
      mv "$out.part" "$out"
      echo "saved $out ($(wc -c <"$out") bytes)"
      echo "Big Buck Bunny (c) 2008 Blender Foundation, www.bigbuckbunny.org, CC BY 3.0"
      exit 0
    fi
    echo "not an Ogg file, trying the next source" >&2
  fi
  rm -f "$out.part"
done
echo "could not download the video; the cinema scene runs without it (NO SIGNAL)" >&2
exit 1
