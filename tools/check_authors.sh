#!/bin/sh
# Every commit is by tiny-paper <noreply@tiny-paper.invalid>: no personal name or address in the public history
# (2026-10-14: 15 commits carried a personal address and the history had to be rewritten before going public).
# Usage: tools/check_authors.sh [REV]   (default HEAD; needs the full history, fetch-depth 0 in CI)
bad=$(git log --format='%h %an <%ae> | %cn <%ce>' "${1:-HEAD}" | grep -v ' tiny-paper <noreply@tiny-paper.invalid> | tiny-paper <noreply@tiny-paper.invalid>$')
[ -z "$bad" ] && exit 0
echo "commits not by tiny-paper <noreply@tiny-paper.invalid>:" >&2
echo "$bad" | head -5 >&2
exit 1
