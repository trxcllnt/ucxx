NUMARGS=$#
ARGS=$*

# NOTE: ensure all dir changes are relative to the location of this
# script, and that this script resides in the repo dir!
REPODIR=$(cd $(dirname $0); pwd)

VALIDARGS="cpp_tests py_tests py_async_tests py_bench py_async_bench"
HELP="$0 [cpp_tests] [py_tests] [py_async_tests] [py_bench] [py_async_bench]
   cpp_tests                     - run all C++ tests
   py_tests                      - run all Python core tests
   py_async_tests                - run all Python async tests
   py_bench                      - run Python core benchmarks
   py_async_bench                - run Python async benchmarks
   clean                         - remove all existing build artifacts and configuration (start
                                   over)
   libucxx                       - build the UCXX C++ module
   libucxx_python                - build the UCXX C++ Python support module
   ucxx                          - build the ucxx Python package
   tests                         - build tests
   -v                            - verbose build mode
   -g                            - build for debug
   -n                            - no install step
   --show_depr_warn              - show cmake deprecation warnings
   --build_metrics               - generate build metrics report for libucxx
   --incl_cache_stats            - include cache statistics in build metrics report
   --cmake-args=\\\"<args>\\\"   - pass arbitrary list of CMake configuration options (escape all quotes in argument)
   -h | --h[elp]                 - print this text

   default action (no args) is to build (with command below) and run all tests and benchmarks.
     ./build.sh libucxx libucxx_python ucxx tests
"
RUN_CPP_TESTS=0
RUN_PY_TESTS=0
RUN_PY_ASYNC_TESTS=0
RUN_PY_BENCH=0
RUN_PY_ASYNC_BENCH=0

# export CMAKE_EXPORT_COMPILE_COMMANDS=ON

# Exit if a building error occurs
set -e

(cd ${REPODIR}; ./build.sh libucxx libucxx_python ucxx tests)
# (cd ${REPO_DIR}; cp cpp/build/compile_commands.json cpp/)

# Let all tests run even if they fail
set +e

function hasArg {
    (( ${NUMARGS} != 0 )) && (echo " ${ARGS} " | grep -q " $1 ")
}

function runAll {
    ((${NUMARGS} == 0 )) || !(echo " ${ARGS} " | grep -q " [^-]\+ ")
}

if hasArg -h || hasArg --h || hasArg --help; then
    echo "${HELP}"
    exit 0
fi

if runAll || hasArg cpp_tests; then
    RUN_CPP_TESTS=1
fi
if runAll || hasArg py_tests; then
    RUN_PY_TESTS=1
fi
if runAll || hasArg py_async_tests; then
    RUN_PY_ASYNC_TESTS=1
fi
if runAll || hasArg py_bench; then
    RUN_PY_BENCH=1
fi
if runAll || hasArg py_async_bench; then
    RUN_PY_ASYNC_BENCH=1
fi

run_tests_async() {
  PROGRESS_MODE=$1
  ENABLE_DELAYED_SUBMISSION=$2
  ENABLE_PYTHON_FUTURE=$3
  SKIP=$4

  CMD_LINE="UCXPY_PROGRESS_MODE=${PROGRESS_MODE} UCXPY_ENABLE_DELAYED_SUBMISSION=${ENABLE_DELAYED_SUBMISSION} UCXPY_ENABLE_PYTHON_FUTURE=${ENABLE_PYTHON_FUTURE} pytest -vs python/ucxx/_lib_async/tests/"

  if [ $SKIP -ne 0 ]; then
    echo -e "\e[31;1mSkipping unstable test: ${CMD_LINE}\e[0m"
  else
    echo -e "\e[1mRunning: ${CMD_LINE}\e[0m"
    UCXPY_PROGRESS_MODE=${PROGRESS_MODE} UCXPY_ENABLE_DELAYED_SUBMISSION=${ENABLE_DELAYED_SUBMISSION} UCXPY_ENABLE_PYTHON_FUTURE=${ENABLE_PYTHON_FUTURE} pytest -vs python/ucxx/_lib_async/tests/
  fi
}

run_benchmark() {
  BACKEND=$1
  PROGRESS_MODE=$2
  ASYNCIO_WAIT=$3
  ENABLE_DELAYED_SUBMISSION=$4
  ENABLE_PYTHON_FUTURE=$5
  SLOW=$6

  if [ $ASYNCIO_WAIT -ne 0 ]; then
    ASYNCIO_WAIT="--asyncio-wait"
  else
    ASYNCIO_WAIT=""
  fi

  CMD_LINE="UCXPY_ENABLE_DELAYED_SUBMISSION=${ENABLE_DELAYED_SUBMISSION} UCXPY_ENABLE_PYTHON_FUTURE=${ENABLE_PYTHON_FUTURE} python -m ucxx.benchmarks.send_recv --backend ${BACKEND} -o cupy --reuse-alloc -d 0 -e 1 -n 8MiB --progress-mode ${PROGRESS_MODE} ${ASYNCIO_WAIT}"

  echo -e "\e[1mRunning: ${CMD_LINE}\e[0m"
  if [ $SLOW -ne 0 ]; then
    echo -e "\e[31;1mSLOW BENCHMARK: it may seem like a deadlock but will eventually complete.\e[0m"
  fi
  UCXPY_ENABLE_DELAYED_SUBMISSION=${ENABLE_DELAYED_SUBMISSION} UCXPY_ENABLE_PYTHON_FUTURE=${ENABLE_PYTHON_FUTURE} python -m ucxx.benchmarks.send_recv --backend ${BACKEND} -o cupy --reuse-alloc -d 0 -e 1 -n 8MiB --progress-mode ${PROGRESS_MODE} ${ASYNCIO_WAIT}
}

if [[ $RUN_CPP_TESTS != 0 ]]; then
  ./cpp/build/gtests/UCXX_TEST
fi
if [[ $RUN_PY_TESTS != 0 ]]; then
  echo -e "\e[1mRunning: pytest-vs python/ucxx/_lib/tests/\e[0m"
  pytest -vs python/ucxx/_lib/tests/
fi
if [[ $RUN_PY_ASYNC_TESTS != 0 ]]; then
  # run_tests_async PROGRESS_MODE   ENABLE_DELAYED_SUBMISSION ENABLE_PYTHON_FUTURE SKIP
  run_tests_async   polling         0                         0                    1    # Unstable
  run_tests_async   polling         0                         1                    1    # Unstable
  run_tests_async   polling         1                         0                    1    # Unstable
  run_tests_async   polling         1                         1                    1    # Unstable
  run_tests_async   thread-polling  0                         0                    1    # Unstable
  run_tests_async   thread-polling  0                         1                    1    # Unstable
  run_tests_async   thread-polling  1                         0                    1    # Unstable
  run_tests_async   thread-polling  1                         1                    1    # Unstable
  run_tests_async   thread          0                         0                    0
  run_tests_async   thread          0                         1                    0
  run_tests_async   thread          1                         0                    0
  run_tests_async   thread          1                         1                    0
fi

if [[ $RUN_PY_BENCH != 0 ]]; then
  # run_benchmark BACKEND   PROGRESS_MODE   ASYNCIO_WAIT  ENABLE_DELAYED_SUBMISSION ENABLE_PYTHON_FUTURE SLOW
  run_benchmark   ucxx-core blocking        0             0                         0                    0
  run_benchmark   ucxx-core polling         0             0                         0                    0
  run_benchmark   ucxx-core thread-polling  0             0                         0                    0
  run_benchmark   ucxx-core thread-polling  1             0                         0                    0
  run_benchmark   ucxx-core thread          0             0                         0                    0
  run_benchmark   ucxx-core thread          1             0                         0                    0
fi
if [[ $RUN_PY_ASYNC_BENCH != 0 ]]; then
  # run_benchmark BACKEND     PROGRESS_MODE   ASYNCIO_WAIT  ENABLE_DELAYED_SUBMISSION ENABLE_PYTHON_FUTURE SLOW
  run_benchmark   ucxx-async  polling         0             0                         0                    0
  run_benchmark   ucxx-async  polling         0             0                         1                    0
  run_benchmark   ucxx-async  polling         0             1                         0                    0
  run_benchmark   ucxx-async  polling         0             1                         1                    0
  run_benchmark   ucxx-async  thread-polling  0             0                         0                    0
  run_benchmark   ucxx-async  thread-polling  0             0                         1                    0
  run_benchmark   ucxx-async  thread-polling  0             1                         0                    0
  run_benchmark   ucxx-async  thread-polling  0             1                         1                    0
  run_benchmark   ucxx-async  thread          0             0                         0                    1
  run_benchmark   ucxx-async  thread          0             0                         1                    1
  run_benchmark   ucxx-async  thread          0             1                         0                    1
  run_benchmark   ucxx-async  thread          0             1                         1                    0
fi