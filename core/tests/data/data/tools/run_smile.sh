#!/bin/bash
# Runs inside eclipse-temurin:21-jdk with this directory mounted at /w.
set -e
cd /w
V=2.17.2
fetch() {
  local group=$1 name=$2
  [ -f "$name-$V.jar" ] || curl -sSfLO "https://repo1.maven.org/maven2/com/fasterxml/jackson/$group/$name/$V/$name-$V.jar"
}
fetch core jackson-core
fetch core jackson-databind
fetch core jackson-annotations
fetch dataformat jackson-dataformat-smile
java -version 2>&1 | head -1
case "$1" in
  encode)
    java -cp "*" SmileTool.java encode smile_input.json jackson_default.sml
    java -cp "*" SmileTool.java encode smile_input.json jackson_shared.sml shared
    java -cp "*" SmileTool.java encode smile_input.json jackson_7bit.sml raw
    ;;
  decode)
    java -cp "*" SmileTool.java decode ours.sml ours_decoded.json
    ;;
esac
