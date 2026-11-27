ci_dependencies(){ :; }
ci_build(){
    cmake -S "$CI_ROOT" -B "$CI_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -Dlibobs_DIR="$CI_SDK/lib/cmake/libobs" \
      -DFRAMEROT_CODEC_ONLY=OFF -DFRAMEROT_OBS_TESTS=ON -DFRAMEROT_PACKAGE_LAYOUT="$CI_LAYOUT"
    cmake --build "$CI_BUILD" --parallel 4
}
ci_test(){
    export LD_LIBRARY_PATH="$CI_SDK/lib/obs-plugins:$LD_LIBRARY_PATH"
    xvfb-run -a -s '-screen 0 1280x1024x24' ctest --test-dir "$CI_BUILD" --output-on-failure --no-tests=error --output-junit "$CI_WORK/ctest.xml"
    python3 -B "$CI_ROOT/.github/scripts/verify_ctest_report.py" "$CI_WORK/ctest.xml" "$MINIMUM_TESTS" linux
}
ci_stage(){
    dest="$CI_STAGE/$MODULE"
    cmake --install "$CI_BUILD" --prefix "$dest"
    cp "$CI_ROOT/COPYING" "$CI_ROOT/README.md" "$dest/"
    mkdir "$dest/docs"
    for doc in guide.md technical.md development.md; do cp "$CI_ROOT/docs/$doc" "$dest/docs/$doc"; done
    printf '%s\n' "$OBS_VERSION" > "$dest/OBS_SDK_VERSION"
    [[ "$(find "$dest" -type f | wc -l)" == 7 ]]
    rel=bin/64bit/$MODULE.so
    [[ "$CI_LAYOUT" != modern ]] || rel=$MODULE.so
    if readelf -d "$dest/$rel" | grep -E 'RPATH|RUNPATH' | grep -F "$CI_WORK"; then
      echo 'Build-specific runtime path in installed module' >&2; exit 1
    fi
}
