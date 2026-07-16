#!/bin/sh
# Generate sitemap.xml for the published Doxygen HTML tree (Google indexing).
set -e

HTML_DIR="docs/html/html"
BASE_URL="https://www.christian-spoo.de/docs/minios"
OUT="$HTML_DIR/sitemap.xml"

if [ ! -d "$HTML_DIR" ]; then
    echo "WARNING: $HTML_DIR missing; run 'make docs' first" >&2
    exit 0
fi

{
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
    find "$HTML_DIR" -name '*.html' | sort | while read -r f; do
        rel=$(printf '%s' "$f" | sed "s|^$HTML_DIR/||")
        lastmod=$(date -r "$f" +%Y-%m-%d)
        printf '  <url>\n    <loc>%s/%s</loc>\n    <lastmod>%s</lastmod>\n  </url>\n' \
            "$BASE_URL" "$rel" "$lastmod"
    done
    echo '</urlset>'
} > "$OUT"

echo "wrote $OUT"
