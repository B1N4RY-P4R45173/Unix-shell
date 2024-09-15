#!/usr/bin/env bash
# Feeds command lines to mysh on stdin (non-interactive mode) and checks the output.
set -u
cd "$(dirname "$0")/.."
SH=./mysh
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0 fail=0

check() {
    local name=$1 input=$2 expected=$3
    local actual
    actual=$(cd "$TMP" && printf '%s\n' "$input" | "$OLDPWD/$SH" 2>&1)
    if [ "$actual" == "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n  expected: %q\n  actual:   %q\n' "$name" "$expected" "$actual"
    fi
}

check "simple command"      "echo hello world"                   "hello world"
check "single quotes"       "echo 'a  |  b'"                     "a  |  b"
check "double quotes"       'echo "x \"y\" z"'                   'x "y" z'
check "backslash"           'echo a\ b'                          "a b"
check "two-stage pipe"      "printf 'b\na\nc\n' | sort"          $'a\nb\nc'
check "three-stage pipe"    "printf 'x\ny\nx\n' | sort | uniq -c | wc -l" "2"
check "output redirect"     $'echo one > f\ncat f'               "one"
check "append redirect"     $'echo one > f\necho two >> f\ncat f' $'one\ntwo'
check "input redirect"      $'printf "3\\n1\\n2\\n" > f\nsort < f' $'1\n2\n3'
check "pipe + both redirs"  $'printf "b\\na\\n" > in\nsort < in | tr a-z A-Z > out\ncat out' $'A\nB'
check "stderr redirect"     $'ls /nonexistent 2> err\nwc -l < err' "1"
check "semicolon list"      "echo a; echo b"                     $'a\nb'
check "cd builtin"          $'mkdir -p d\ncd d\npwd | xargs basename' "d"
check "command not found"   "no_such_cmd_xyz"                    "mysh: no_such_cmd_xyz: command not found"
check "missing input file"  "cat < nope"                         "mysh: nope: No such file or directory"
check "syntax: leading |"   "| ls"                               "mysh: syntax error"
check "syntax: trailing |"  "ls |"                               "mysh: syntax error"
check "syntax: no target"   "echo >"                             "mysh: syntax error"
check "unterminated quote"  "echo 'abc"                          "mysh: unterminated quote"
check "comment"             "echo hi # ignored"                  "hi"
check "exit status"         "exit 3; echo unreachable"           ""

# Background job: prompt returns immediately, job is reported Done later.
out=$(printf 'sleep 0.3 &\necho started\njobs\nsleep 0.5\necho after\n' | $SH 2>&1)
if [[ "$out" =~ ^\[1\]\ [0-9]+$'\n'started$'\n'\[1\]\+\ +Running\ +sleep\ 0\.3\ \&$'\n'\[1\]\+?\ +Done\ +sleep\ 0\.3$'\n'after$ ]]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1)); printf 'FAIL: background job\n%s\n' "$out"
fi

# Background job really runs concurrently: two 0.5s sleeps finish in well under 1s.
start=$(date +%s%N)
printf 'sleep 0.5 & sleep 0.5\n' | $SH > /dev/null 2>&1
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
if [ "$elapsed" -lt 900 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: concurrency (${elapsed}ms)"; fi

# Exit code propagates.
printf 'false\n' | $SH; code=$?
if [ "$code" -eq 1 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: exit code $code"; fi

# No leaked pipe fds: `yes | head` must terminate (yes gets SIGPIPE).
if timeout 3 bash -c "printf 'yes | head -n 2\n' | $SH" > /dev/null; then
    pass=$((pass + 1))
else
    fail=$((fail + 1)); echo "FAIL: yes | head hung (leaked pipe fd?)"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
