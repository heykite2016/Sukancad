#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /path/to/drawing.dwg" >&2
    exit 2
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_BINARY="$ROOT_DIR/dist/CadLens.app/Contents/MacOS/CadLens"
if [[ ! -x "$APP_BINARY" ]]; then
    APP_BINARY="$ROOT_DIR/build/CadLens.app/Contents/MacOS/CadLens"
fi
if [[ ! -x "$APP_BINARY" ]]; then
    echo "CadLens has not been built yet. Run ./script/build_and_run.sh first." >&2
    exit 2
fi

REPORT="$("$APP_BINARY" --inspect "$1" 2>/dev/null)"
printf '%s\n' "$REPORT"

require_line() {
    local pattern="$1"
    if ! grep -Eq "$pattern" <<<"$REPORT"; then
        echo "validation failed: missing pattern $pattern" >&2
        exit 1
    fi
}

require_line '^format=DWG '
require_line 'backend=LibreDWG$'
require_line '^entities=[1-9][0-9]*$'
require_line '^layers=[1-9][0-9]*'
require_line '^bounds=-?[0-9]'
require_line '^malformedText=0$'
require_line '^attributeAudit=mode=-?[0-9]+ source=[0-9]+ visible=[0-9]+ hidden=[0-9]+ rendered=[0-9]+ suppressed=[0-9]+ overriddenHidden=[0-9]+$'
require_line '^layerColor=.+\|#[0-9a-fA-F]{8}\|'
require_line '^layoutViewport=.+\|[01]\|'

if grep -Eqi '(^bounds=.*(nan|inf)|^computedBounds=.*(nan|inf)|^layoutBounds=.*(nan|inf)|^layerBounds=.*(nan|inf)|^layoutViewport=.*(nan|inf))' <<<"$REPORT"; then
    echo "validation failed: non-finite geometry detected" >&2
    exit 1
fi

MODEL_ENTITIES="$(sed -n 's/^entities=//p' <<<"$REPORT")"
LAYOUT_COUNT="$(grep -c '^layout=' <<<"$REPORT" || true)"
MODEL_VIEWPORTS="$(
    grep -cE '^layoutViewport=.*\|1\|' <<<"$REPORT" || true
)"
LAYER_COUNT="$(grep -c '^layerColor=' <<<"$REPORT" || true)"
ATTRIBUTE_AUDIT="$(grep '^attributeAudit=' <<<"$REPORT")"
read -r ATTRIBUTE_MODE ATTRIBUTE_SOURCE ATTRIBUTE_VISIBLE \
    ATTRIBUTE_HIDDEN ATTRIBUTE_RENDERED ATTRIBUTE_SUPPRESSED \
    ATTRIBUTE_OVERRIDDEN <<<"$(
        sed -E \
            's/^attributeAudit=mode=([^ ]+) source=([^ ]+) visible=([^ ]+) hidden=([^ ]+) rendered=([^ ]+) suppressed=([^ ]+) overriddenHidden=([^ ]+)$/\1 \2 \3 \4 \5 \6 \7/' \
            <<<"$ATTRIBUTE_AUDIT"
    )"
if (( ATTRIBUTE_VISIBLE + ATTRIBUTE_HIDDEN != ATTRIBUTE_SOURCE )); then
    echo "validation failed: attribute visibility ledger is inconsistent" >&2
    exit 1
fi
if (( ATTRIBUTE_MODE == 0 && ATTRIBUTE_RENDERED != 0 )); then
    echo "validation failed: attributes rendered in ATTMODE=0" >&2
    exit 1
fi
if (( ATTRIBUTE_MODE == 1 && ATTRIBUTE_OVERRIDDEN != 0 )); then
    echo "validation failed: hidden attributes overridden in ATTMODE=1" >&2
    exit 1
fi
if (( ATTRIBUTE_MODE == 2 && ATTRIBUTE_SUPPRESSED != 0 )); then
    echo "validation failed: attributes hidden in ATTMODE=2" >&2
    exit 1
fi

echo "validation passed: model=${MODEL_ENTITIES} entities, layers=${LAYER_COUNT}, layouts=${LAYOUT_COUNT}, model-viewports=${MODEL_VIEWPORTS}, attributes=${ATTRIBUTE_SOURCE} (${ATTRIBUTE_VISIBLE} visible, ${ATTRIBUTE_HIDDEN} hidden), finite coordinates, clean text"
