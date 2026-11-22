#!/usr/bin/env bash
# Standalone, local-only regression runs. No install step or external services.
set -euo pipefail
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-native}"
if (($#)); then shift; fi
# Every mode but fuzz, each run by this script exactly as on its own and into its own build directory, streamed and
# logged; a summary of what passed and failed comes last. The ctest options go to every mode, BUILD_DIR is the root.
if [[ "$MODE" == full ]]; then
    FULL_ROOT="${BUILD_DIR:-$TEST_ROOT/build}"
    FULL_LOGS="$FULL_ROOT/regressions-full"
    FULL_PASSED=()
    FULL_FAILED=()
    FULL_REPORT=()
    mkdir -p "$FULL_LOGS"
    trap 'echo; exit 130' INT
    for FULL_MODE in native no-ssl asan ubsan tsan valgrind coverage; do
        FULL_LOG="$FULL_LOGS/$FULL_MODE.log"
        FULL_START=$SECONDS
        FULL_STATUS=0
        echo "==== $FULL_MODE ===="
        BUILD_DIR="$FULL_ROOT/regressions-$FULL_MODE" bash "${BASH_SOURCE[0]}" "$FULL_MODE" "$@" 2>&1 | tee "$FULL_LOG" ||
            FULL_STATUS=$?
        FULL_TIME=$((SECONDS - FULL_START))
        FULL_TIME="$((FULL_TIME / 60))m$((FULL_TIME % 60))s"
        FULL_RESULT="$(grep -E '^[0-9]+% tests passed' "$FULL_LOG" | tail -n 1 || true)"
        if ((FULL_STATUS == 0)); then
            FULL_PASSED+=("$FULL_MODE")
            FULL_REPORT+=("$(printf 'PASS  %-9s %s (%s)' "$FULL_MODE" "${FULL_RESULT:-done}" "$FULL_TIME")")
            continue
        fi
        FULL_FAILED+=("$FULL_MODE")
        FULL_REPORT+=("$(printf 'FAIL  %-9s %s (%s, exit %d)' "$FULL_MODE" \
            "${FULL_RESULT:-stopped before the tests ran, see $FULL_LOG}" "$FULL_TIME" "$FULL_STATUS")")
        while IFS= read -r FULL_TEST; do
            FULL_REPORT+=("        $FULL_TEST")
        done < <(sed -n '/^The following tests FAILED:/,/^Errors while running CTest/p' "$FULL_LOG" |
            grep -E '^[[:space:]]+[0-9]+ - ' | sed -E 's/^[[:space:]]+//' || true)
    done
    "$TEST_ROOT/tests/report-coverage.py" "$FULL_ROOT/regressions-coverage"
    echo "==== full ===="
    printf '%s\n' "${FULL_REPORT[@]}"
    echo "Passed: ${FULL_PASSED[*]:-none}"
    echo "Failed: ${FULL_FAILED[*]:-none}"
    echo "Logs:   $FULL_LOGS"
    ((${#FULL_FAILED[@]} == 0)) || exit 1
    exit 0
fi
TEST_BUILD="${BUILD_DIR:-$TEST_ROOT/build/regressions-$MODE}"
TEST_JOBS="${JOBS:-4}"
CMAKE_ARGS=(-S "$TEST_ROOT" -B "$TEST_BUILD" -DXUTILS_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug)
case "$MODE" in
    native|no-ssl|valgrind) CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CC:-cc}") ;;
    ubsan) CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CC:-gcc}") ;;
    asan|tsan|fuzz) CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CC:-clang}") ;;
    coverage) CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CC:-gcc}") ;;
    *) echo "Usage: $0 {native|no-ssl|asan|ubsan|tsan|fuzz|valgrind|coverage|full} [ctest options]" >&2; exit 2 ;;
esac
case "$MODE" in
    no-ssl) CMAKE_ARGS+=(-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON) ;;
    asan|fuzz)
        TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
        if [[ "$MODE" == fuzz ]]; then
            TEST_FLAGS='-fsanitize=address,undefined,fuzzer-no-link -fno-omit-frame-pointer'
            CMAKE_ARGS+=(-DXUTILS_BUILD_FUZZERS=ON)
        fi
        CMAKE_ARGS+=("-DCMAKE_C_FLAGS=$TEST_FLAGS" '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined')
        export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}"
        export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
        ;;
    # UBSan on its own, by default under gcc: the asan mode already carries
    # clang's UBSan, and gcc's implementation instruments differently. With
    # -fno-sanitize-recover every report aborts the test even if UBSAN_OPTIONS
    # is overridden.
    ubsan)
        CMAKE_ARGS+=('-DCMAKE_C_FLAGS=-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined')
        export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
        ;;
    tsan)
        CMAKE_ARGS+=('-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread')
        export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}"
        ;;
    # Atomic counters: threads that run the same code at once otherwise lose increments, and the counts gcov
    # derives from the ones left go negative and cancel what other tests covered.
    coverage) CMAKE_ARGS+=('-DCMAKE_C_FLAGS=--coverage -fprofile-update=atomic' '-DCMAKE_EXE_LINKER_FLAGS=--coverage'
        -DXUTILS_TEST_COVERAGE=ON) ;;
esac
cmake "${CMAKE_ARGS[@]}"
cmake --build "$TEST_BUILD" -j "$TEST_JOBS"
case "$MODE" in
    valgrind)
        command -v valgrind >/dev/null
        export XUTILS_TEST_TIMEOUT_MS="${XUTILS_TEST_TIMEOUT_MS:-60000}"
        ctest --test-dir "$TEST_BUILD" -T memcheck --output-on-failure \
            --overwrite 'MemoryCheckCommandOptions=--leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --track-origins=yes --error-exitcode=99' "$@"
        ;;
    # Anything that runs a second thread belongs here, which now includes the
    # network cases that drive a client or a server alongside the test.
    tsan)
        TEST_TSAN_PATTERN='(thread|thread_fault|type|sync|mon|mon_fixture|sock_ext|sock_tls|sock_timeout|http_client'
        TEST_TSAN_PATTERN+='|api_tls|api_events|worker_sock|ws)_regression|network_feature|http_flow_feature|worker_feature'
        ctest --test-dir "$TEST_BUILD" --output-on-failure -R "$TEST_TSAN_PATTERN" "$@"
        ;;
    fuzz)
        mkdir -p "$TEST_BUILD/artifacts"
        "$TEST_BUILD/tests/fuzz_corpus" "$TEST_BUILD/corpus"
        "$TEST_BUILD/tests/fuzz_parsers" "$TEST_BUILD/corpus" -max_total_time="${FUZZ_TIME:-60}" \
            -max_len=65536 -timeout=5 -rss_limit_mb=2048 -artifact_prefix="$TEST_BUILD/artifacts/" "$@"
        ;;
    *) ctest --test-dir "$TEST_BUILD" --output-on-failure "$@" ;;
esac
