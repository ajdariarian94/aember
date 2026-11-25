#!/usr/bin/env bash
# Model-check the TLA+ specs with TLC (in Docker: no Java needed here).
#   spec/tla/check.sh            every model (*.cfg)
#   spec/tla/check.sh Diamond    one model
# Models whose .cfg says "EXPECTED TO FAIL" must find a violation: they pin
# down a bug the specs found (README.md).
set -euo pipefail
cd "$(dirname "$0")"

TLA_VERSION=1.7.4
TLA_SHA256=936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
JRE_IMAGE=eclipse-temurin:21-jre
CACHE=${TLA_CACHE:-$HOME/.cache/aember-tla}
JAR=$CACHE/tla2tools-$TLA_VERSION.jar

mkdir -p "$CACHE"
if ! echo "$TLA_SHA256  $JAR" | sha256sum -c --status >/dev/null 2>&1; then
  curl -sSfL -o "$JAR.part" \
    "https://github.com/tlaplus/tlaplus/releases/download/v$TLA_VERSION/tla2tools.jar"
  echo "$TLA_SHA256  $JAR.part" | sha256sum -c --status
  mv "$JAR.part" "$JAR"
fi

# Which module a model checks: MC<spec>.tla next to it.
module_for() {
  case "$1" in
    Diamond|DiamondLimited|Cyclic|RestartDuringShutdown) echo MCStartGraph ;;
    *) echo "unknown model $1" >&2; exit 2 ;;
  esac
}

models=("$@")
if [ ${#models[@]} -eq 0 ]; then
  for cfg in *.cfg; do models+=("${cfg%.cfg}"); done
fi

status=0
for model in "${models[@]}"; do
  module=$(module_for "$model")
  expect_fail=0
  grep -q "EXPECTED TO FAIL" "$model.cfg" && expect_fail=1
  echo "== $model ($module)"
  set +e
  docker run --rm --memory 4g -u "$(id -u):$(id -g)" \
    -v "$PWD:/spec" -v "$JAR:/tla2tools.jar:ro" -w /spec "$JRE_IMAGE" \
    java -Xmx3g -XX:+UseParallelGC -cp /tla2tools.jar tlc2.TLC \
    -workers auto -cleanup -metadir /tmp/tlc -config "$model.cfg" "$module.tla" \
    > "/tmp/tlc-$model.log" 2>&1
  rc=$?
  set -e
  grep -E "^Error:|violated|states generated|Model checking completed|Finished in" \
    "/tmp/tlc-$model.log" || true
  if [ $expect_fail -eq 1 ]; then
    if [ $rc -eq 0 ]; then
      echo "!! $model: expected a violation, TLC found none"; status=1
    else
      echo "ok $model: violation found, as expected"
    fi
  elif [ $rc -ne 0 ]; then
    echo "!! $model failed (TLC exit $rc); the whole output:"; cat "/tmp/tlc-$model.log"; status=1
  else
    echo "ok $model"
  fi
done
exit $status
