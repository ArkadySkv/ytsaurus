#!/bin/bash
################################################################################

function shout() {
    set +x
    echo '*' $@ >&2
    set -x
}

function tc() {
    set +x
    echo "##teamcity[$*]"
    set -x
}

function usage() {
    echo "$0: effortlessly build yt source tree on TeamCity build farm"
    echo "$0: <checkout-directory> <working-directory> <build-branch> <build-type> <with-package> <with-deploy>"
    echo ""
    echo "<build-type> have to be compliant with CMAKE_BUILD_TYPE"
    echo "<with-package> have to be either YES or NO"
    echo "<with-deploy> have to be either YES or NO"
    echo ""
    echo "Following environment variables must be set:"
    echo "  TEAMCITY_VERSION"
    echo "  TEAMCITY_BUILDCONF_NAME"
    echo "  TEAMCITY_PROJECT_NAME"
    echo "  BUILD_NUMBER"
    echo "  BUILD_VCS_NUMBER"
}

################################################################################
tc "progressMessage 'Setting up...'"

export LANG=en_US.UTF-8
export LANGUAGE=en_US.UTF-8
export LC_ALL=en_US.UTF-8
export LC_CTYPE=C

[[ -z "$TEAMCITY_VERSION"        ]] && usage && exit 1
[[ -z "$TEAMCITY_BUILDCONF_NAME" ]] && usage && exit 1
[[ -z "$TEAMCITY_PROJECT_NAME"   ]] && usage && exit 1

[[ -z "$BUILD_NUMBER" ]] && usage && exit 2
[[ -z "$BUILD_VCS_NUMBER" ]] && usage && exit 2

[[ -z "$1" || -z "$2" || -z "$3" || -z "$4" || -z "$5" || -z "$6" ]] && usage && exit 3

CHECKOUT_DIRECTORY=$1
WORKING_DIRECTORY=$2
SANDBOX_DIRECTORY=$7
BUILD_BRANCH=$3
BUILD_TYPE=$4
WITH_PACKAGE=$5
WITH_DEPLOY=$6

# COMPAT(sandello): Set default.
[[ -z "$SANDBOX_DIRECTORY" ]] && SANDBOX_DIRECTORY=/home/teamcity/sandbox

if [[ ( $WITH_PACKAGE != "YES" ) && ( $WITH_PACKAGE != "NO" ) ]]; then
    shout "WITH_PACKAGE have to be either YES or NO."
    exit 1
fi

if [[ ( $WITH_DEPLOY != "YES" ) && ( $WITH_DEPLOY != "NO" ) ]]; then
    shout "WITH_DEPLOY have to be either YES or NO."
    exit 1
fi

try_to_find_compiler() {
    local version="$1"

    if [[ -z "$CC" ]]; then
        shout "C compiler is not specified; trying to find gcc-${version}..."
        CC=$(which gcc-${version})
        shout "CC=$CC"
    fi
    if [[ -z "$CXX" ]]; then
        shout "C++ compiler is not specified; trying to find g++-${version}..."
        CXX=$(which g++-${version})
        shout "CXX=$CXX"
    fi
}

try_to_find_compiler "4.7"
try_to_find_compiler "4.6"
try_to_find_compiler "4.5"

[[ -z "$CC"  ]] && shout "Unable to find proper C compiler; exiting..." && exit 1
[[ -z "$CXX" ]] && shout "Unable to find proper C++ compiler; exiting..." && exit 1

################################################################################

export CC
export CXX

export LC_ALL=C
export LANG=en_US.UTF-8

set -e
set -x

################################################################################

tc "progressMessage 'Cleaning other builds...'"
mkdir -p $WORKING_DIRECTORY
cd $WORKING_DIRECTORY

tc "blockOpened name='CMake'"

shout "Running CMake..."
tc "progressMessage 'Running CMake...'"
cmake \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DCMAKE_COLOR_MAKEFILE:BOOL=OFF \
    -DYT_BUILD_ENABLE_EXPERIMENTS:BOOL=ON \
    -DYT_BUILD_ENABLE_TESTS:BOOL=ON \
    -DYT_BUILD_ENABLE_NODEJS:BOOL=ON \
    -DYT_BUILD_BRANCH=${BUILD_BRANCH/\/0.??/} \
    -DYT_BUILD_NUMBER=$BUILD_NUMBER \
    -DYT_BUILD_VCS_NUMBER=$(echo $BUILD_VCS_NUMBER | cut -c 1-7) \
    $CHECKOUT_DIRECTORY

trap '(cd $WORKING_DIRECTORY ; find . -name "default.log" -delete)' 0

tc "blockClosed name='CMake'"

tc "blockOpened name='make'"

shout "Running make (1/2; fast)..."
tc "progressMessage 'Running make (1/2; fast)...'"
make -j $(cat /proc/cpuinfo | grep processor | wc -l) || true

shout "Running make (2/2; slow)..."
tc "progressMessage 'Running make (2/2; slow)...'"
make -j 1

tc "blockClosed name='make'"

# set set-uid-bit for ytserver
sudo chown root $WORKING_DIRECTORY/bin/ytserver
sudo chmod 4755 $WORKING_DIRECTORY/bin/ytserver

package_version=
package_ticket=

if [[ ( $WITH_PACKAGE = "YES" ) ]]; then
    tc "progressMessage 'Packing...'"

    make package
    make version

    package_version=$(cat ytversion)

    dupload --to yandex-precise --nomail ARTIFACTS/yandex-yt*${package_version}*.changes

    tc "setParameter name='yt.package_built' value='1'"
    tc "setParameter name='yt.package_version' value='$package_version'"
fi

if [[ ( $WITH_PACKAGE = "YES" ) && ( $WITH_DEPLOY = "YES" ) ]]; then
    tc "progressMessage 'Deploying...'"

    comment_file=$(mktemp)
    deploy_file=ARTIFACTS/deploy_${package_version}

    # TODO(sandello): More verbose commentary is always better.
    # TODO(sandello): Insert proper buildTypeId here.

    trap 'rm -f $comment_file' INT TERM EXIT

    echo "Auto-generated ticket posted by $(hostname) on $(date)" > $comment_file
    echo "See http://teamcity.yandex.ru/viewLog.html?buildTypeId=bt1364&buildNumber=${BUILD_NUMBER}" >> $comment_file

    curl http://c.yandex-team.ru/auth_update/ticket_add/ \
        --silent --get \
        --data-urlencode "package[0]=yandex-yt" \
        --data-urlencode "version[0]=${package_version}" \
        --data-urlencode "package[1]=yandex-yt-http-proxy" \
        --data-urlencode "version[1]=${package_version}" \
        --data-urlencode "ticket[branch]=testing" \
        --data-urlencode "ticket[mailcc]=yt-dev-root@yandex-team.ru" \
        --data-urlencode "ticket[comment]@${comment_file}" \
        --cookie "conductor_auth=$(cat ~/.conductor_auth)" \
        --header "Accept: application/xml" \
        --write-out "\nHTTP %{http_code} (done in %{time_total}s)\n" \
        --output "${deploy_file}" \
        --show-error

    package_ticket=$(cat ${deploy_file} | grep URL | cut -d : -f 2- | cut -c 2-)

    tc "setParameter name='yt.package_ticket' value='$package_ticket'"
fi

[[ -d ARTIFACTS ]] && (cd ARTIFACTS && ls -1t . | tac | head -n -10 | xargs rm -f)

set +e
a=0

tc "blockOpened name='Unit Tests'"

shout "Running unit tests..."
tc "progressMessage 'Running unit tests...'"

cd $WORKING_DIRECTORY
gdb \
    --batch \
    --return-child-result \
    --command=$CHECKOUT_DIRECTORY/scripts/teamcity-gdb-script \
    --args \
    ./bin/unittester \
        --gtest_color=no \
        --gtest_output=xml:$WORKING_DIRECTORY/unit_tests.xml
b=$?
a=$((a+b))

tc "blockClosed name='Unit Tests'"

# Global preparation
mkdir -p $HOME/failed_tests
ls -1td $HOME/failed_tests/* | awk 'BEGIN { a = 0; } { ++a; if (a > 5) print $0; }' | xargs rm -rf

cd "$CHECKOUT_DIRECTORY/python/yt/wrapper" && make
cd "$CHECKOUT_DIRECTORY/python" && make version

# Pre-install npm packages.
(cd "$WORKING_DIRECTORY/yt/nodejs" && rm -rf node_modules && npm install)

ulimit -c unlimited

run_python_test()
{
    local dir=$1
    local test_name=$2
    local block_name="'${test_name} tests'"

    shout "Running $test_name tests..."

    tc "blockOpened name=${block_name}"
    tc "progressMessage 'Running $test_name tests...'"

    mkdir -p "$SANDBOX_DIRECTORY/${test_name}"

    cd $dir && \
    TESTS_SANDBOX="$SANDBOX_DIRECTORY/${test_name}" \
    PYTHONPATH="$CHECKOUT_DIRECTORY/python:$PYTHONPATH" \
    PATH="$WORKING_DIRECTORY/bin:$WORKING_DIRECTORY/yt/nodejs:$PATH" \
        py.test \
            -rx -vs \
            --tb=native \
            --timeout 300 \
            --junitxml="$WORKING_DIRECTORY/test_${test_name}.prexml"
    b=$?
    a=$((a+b))
    cat > /tmp/fix_xml_entities.py <<-EOP
#!/usr/bin/python

import xml.etree.ElementTree as etree
import sys

tree = etree.parse(sys.stdin)
for node in tree.iter():
    if isinstance(node.text, str):
        node.text = node.text \
            .replace("&quot;", "\"") \
            .replace("&apos;", "\'") \
            .replace("&amp;", "&") \
            .replace("&lt;", "<") \
            .replace("&gt;", ">")
tree.write(sys.stdout, encoding="utf-8")
EOP
    if [[ -f $WORKING_DIRECTORY/test_${test_name}.prexml ]]; then
        cat $WORKING_DIRECTORY/test_${test_name}.prexml | python /tmp/fix_xml_entities.py > $WORKING_DIRECTORY/test_${test_name}.xml
    fi

    if [[ "$b" != "0" ]]; then
        local src="$SANDBOX_DIRECTORY/${test_name}"
        local dst="$HOME/failed_tests/${BUILD_NUMBER}_${test_name}"

        mkdir -p $dst
        cp -r $src/* $dst/
    fi

    tc "blockClosed name=${block_name}"
}

run_python_test "$CHECKOUT_DIRECTORY/tests/integration" "integration"
run_python_test "$CHECKOUT_DIRECTORY/python" "python_libraries"

tc "blockOpened name='JavaScript Tests'"

shout "Running JavaScript tests..."
tc "progressMessage 'Running JavaScript tests...'"

export MOCHA_OUTPUT_FILE=$WORKING_DIRECTORY/test_javascript.xml
(cd $WORKING_DIRECTORY/yt/nodejs && ./run_tests.sh -R xunit)

b=$?
a=$((a+b))

tc "blockClosed name='JavaScript Tests'"

cd $WORKING_DIRECTORY

# TODO(sandello): Export final package name as build parameter.
# TODO(sandello): Measure some statistics.
exit $a

