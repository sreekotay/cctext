#!/bin/sh
# cctext embed fixture: heredocs, here-strings, case patterns.
NAME=world

cat <<EOF
Hello ${NAME}, today is $(date +%Y).
Unquoted heredoc: expansions apply, but \$ESCAPED and "def" stay literal text.
EOF

cat <<'EOF'
Quoted heredoc: $(this) and ${that} are NOT expanded.
Neither is "this" or 'that'.
EOF

if true; then
	cat <<-EOF
	Indented heredoc: leading tabs are stripped.
	Value: ${NAME}
	EOF
fi

python3 - <<PY
import sys
def main():
    """docstring inside a heredoc inside a shell script"""
    print("hello from python", sys.version_info[:2])
main()
PY

case "$1" in
  -h|--help) echo "usage: $0 [-h] name" ;;
  *.py|*.js) echo "script: $1" ;;
  "")        echo "empty" ;;
  *)         echo "other: $1" ;;
esac

read -r first rest <<<"one two three"
echo "$first"
