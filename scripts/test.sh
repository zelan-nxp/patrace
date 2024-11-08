#!/bin/bash

### Run release integration testing
#

set -e

### Sanity checking the test
#

if [ "$(pwd|sed 's/.*scripts.*/scripts/')" == "scripts" ]; then
	echo "Must be run from source root directory"
	exit 1
fi

# Make sure we do not pollute screenshots with FPS numbers, causing indeterminism
unset __GL_SHOW_GRAPHICS_OSD

### Defines
#

OUT=tmp/testfiles
X11_PATRACE_ROOT="$(pwd)/install/patrace/x11_x64/sanitizer"
X11_PATRACE_BIN="${X11_PATRACE_ROOT}/bin"
X11_PATRACE_LIB="${X11_PATRACE_ROOT}/lib"
X11_PATRACE_TEST="${X11_PATRACE_ROOT}/tests"
SANITIZER_PARETRACE="$(pwd)/install/patrace/x11_x64/sanitizer/bin/paretrace"
RELEASE_PARETRACE="$(pwd)/install/patrace/x11_x64/release/bin/paretrace"
TESTTRACE1=$OUT/indirectdraw_1.1.pat
TESTTRACE2=$OUT/khr_blend_equation_advanced.1.pat
TESTTRACE3=$OUT/geometry_shader_1.1.pat
TESTTRACE_MSAA=$OUT/multisample_1.1.pat
ASANLIB=$(ldd ${X11_PATRACE_BIN}/paretrace | grep libasan | sed 's#.* \(/usr.*\) .*#\1#')
if [[ "$ASANLIB" == "" ]]; then
	echo "Failed to find ASAN library"
	exit 1
fi
export LSAN_OPTIONS="detect_leaks=0"
export ASAN_OPTIONS="symbolize=1,abort_on_error=1"

### Check that all build targets exist
#
if [[ "$(pwd)/install/patrace/x11_x64/sanitizer/bin/paretrace" == "" ]]; then
	echo "Sanitizer build missing - run scripts/test_build.sh"
fi
if [[ "$(pwd)/install/patrace/x11_x64/debug/bin/paretrace" == "" ]]; then
	echo "Debug build missing - run scripts/test_build.sh"
fi
if [[ "$(pwd)/install/patrace/x11_x64/release/bin/paretrace" == "" ]]; then
	echo "Release build missing - run scripts/test_build.sh"
fi
if [[ "$(pwd)/install/patrace/x11_x32/release/bin/paretrace" == "" ]]; then
	echo "32-bit build missing - run scripts/test_build.sh"
fi
if [[ "$(pwd)/install/patrace/fbdev_aarch64/release/bin/paretrace" == "" ]]; then
	echo "aarch64 build missing - run scripts/test_build.sh"
fi

### Functions
#

function trace()
{
	echo
	echo "-- tracing $1 --"
	echo "ASANLIB: $ASANLIB"
	set -x
	TOOLSTEST_NULL_RUN=1 OUT_TRACE_FILE=$OUT/$1 LD_PRELOAD=${ASANLIB}:${X11_PATRACE_LIB}/libegltrace.so INTERCEPTOR_LIB=${X11_PATRACE_LIB}/libegltrace.so ${X11_PATRACE_TEST}/gles_$1
	set +x
}

function nreplay()
{
	echo
	echo "-- replaying $1 (no image output) --"
	set -x
	${X11_PATRACE_BIN}/paretrace -multithread -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/$1.1.pat
	set +x
}

function replay()
{
	echo
	echo "-- replaying $1 --"
	set -x
	${SANITIZER_PARETRACE} -multithread -noscreen -snapshotprefix ${1}_ -framenamesnaps -s */frame -overrideEGL 8 8 8 8 24 8 $OUT/$1.1.pat
	mv *.png tmp/png1
	${RELEASE_PARETRACE} -multithread -noscreen -snapshotprefix ${1}_ -framenamesnaps -s */frame -overrideEGL 8 8 8 8 24 8 $OUT/$1.1.pat
	mv *.png tmp/png2
	set +x
}

function integration_test()
{
	echo "*** Testing INTEGRATION TESTS"

	rm -rf tmp/png1 tmp/png2
	rm -f tmp/testfiles/*
	mkdir -p $OUT

	# --- STEP : Trace
	trace dummy_1
	trace multisurface_1
	trace multithread_1
	trace multithread_2
	trace multithread_3
	trace drawrange_1
	trace drawrange_2
	trace compute_1
	trace compute_2
	trace compute_3
	trace programs_1
	trace imagetex_1
	trace indirectdraw_1
	trace indirectdraw_2
	trace multisample_1
	trace vertexbuffer_1
	trace bindbufferrange_1
	trace geometry_shader_1
	trace khr_debug
	trace copy_image_1
	trace ext_texture_border_clamp
	trace ext_texture_buffer
	trace ext_texture_cube_map_array
	trace ext_texture_sRGB_decode
	trace khr_blend_equation_advanced
	trace oes_sample_shading
	trace oes_texture_stencil8
	trace ext_gpu_shader5
	trace uninit_texture_1
	trace uninit_texture_2

	rm -f *.png
	mkdir -p tmp/png1
	mkdir -p tmp/png2

	# --- STEP : Retrace
	# These tests do not generate image output
	nreplay dummy_1
	nreplay compute_1
	nreplay compute_2
	nreplay compute_3
	nreplay programs_1
	nreplay imagetex_1
	nreplay bindbufferrange_1
	nreplay khr_debug

	# play image generating tests twice to look for non-determinism
	replay drawrange_1
	replay drawrange_2
	replay multisurface_1
	replay multithread_1
	replay multithread_2
	replay multithread_3
	replay indirectdraw_1
	replay indirectdraw_2
	replay multisample_1
	replay vertexbuffer_1
	replay geometry_shader_1
	replay copy_image_1
	replay ext_texture_border_clamp
	replay ext_texture_buffer
	replay ext_texture_cube_map_array
	replay ext_texture_sRGB_decode
	replay khr_blend_equation_advanced
	replay oes_sample_shading
	replay oes_texture_stencil8
	replay ext_gpu_shader5

	# will warn if tests do not generate deterministic results -- image based
	( cd tmp/png2 ; find . -type f -exec cmp {} ../png1/{} ';' )
}

function replayer_test()
{
	echo "*** Testing REPLAYER FEATURES"

	echo "Testing MSAAx2 injection"
	set -x
	PATRACE_SUPPORT2XMSAA=true trace multisample_1 >out.txt
	cat out.txt | grep "MSAA settings" | grep -e "2"
	set +x

	echo "Testing paretrace options"
	set -x
	${X11_PATRACE_BIN}/paretrace -overrideEGL 8 8 8 8 24 8 -noscreen $TESTTRACE2
	${X11_PATRACE_BIN}/paretrace -offscreen $TESTTRACE2
	${X11_PATRACE_BIN}/paretrace -preload 2 7 -noscreen $TESTTRACE2
	${X11_PATRACE_BIN}/paretrace -overrideEGL 8 8 8 8 24 8 -framerange 0 10 -noscreen $TESTTRACE2
	${X11_PATRACE_BIN}/paretrace -infojson $TESTTRACE2
	${X11_PATRACE_BIN}/paretrace -info $TESTTRACE2
	${X11_PATRACE_BIN}/paretrace -overrideEGL 8 8 8 8 24 8 -noscreen -debug -framerange 3 9 -flush -collect $TESTTRACE3
	${X11_PATRACE_BIN}/paretrace -noscreen -debug -framerange 3 9 -overrideMSAA 2 $TESTTRACE_MSAA
	set +x
}

function tools_test()
{
	rm -rf aztmp* *.ra
	echo "*** Testing TOOLS"

	echo "*** Testing shader_repacker"
	set -x
	${X11_PATRACE_ROOT}/tools/shader_repacker --split geom $TESTTRACE3
	echo >> geomshader_4_p0_c0.geom
	echo "// dumb comment" >> geomshader_4_p0_c0.geom
	${X11_PATRACE_ROOT}/tools/shader_repacker --repack geom $TESTTRACE3 geom2.pat
	${X11_PATRACE_ROOT}/tools/shader_repacker --split geom2 geom2.pat
	cmp geomshader_3_p0_c0.frag geom2shader_3_p0_c0.frag
	grep -e comment geom2shader_4_p0_c0.geom
	set +x
	rm -f geom2shader* geomshader*

	echo "Testing totxt"
	set -x
	find tmp/testfiles -name \*.pat -execdir install/patrace/x11_x64/debug/tools/totxt {} \; 2> /dev/null
	set +x

	echo "Testing shader analyzer and shader repacker"
	set -x
	${X11_PATRACE_BIN}/drawstate --version
	${X11_PATRACE_ROOT}/tools/shader_analyzer -v
	${X11_PATRACE_ROOT}/tools/shader_analyzer --selftest
	${X11_PATRACE_ROOT}/tools/shader_repacker --split geom_ $TESTTRACE3
	${X11_PATRACE_ROOT}/tools/shader_analyzer --test geom_shader_3_p0_c0.frag
	${X11_PATRACE_ROOT}/tools/shader_analyzer --test geom_shader_2_p0_c0.vert
	${X11_PATRACE_ROOT}/tools/shader_analyzer --test geom_shader_4_p0_c0.geom
	${X11_PATRACE_ROOT}/tools/shader_repacker --repack geom_ $TESTTRACE3 tmp/tmp.pat
	${X11_PATRACE_ROOT}/tools/shader_repacker --split indirect_ $TESTTRACE1
	${X11_PATRACE_ROOT}/tools/shader_analyzer --test indirect_shader_3_p0_c0.frag
	${X11_PATRACE_ROOT}/tools/shader_analyzer --test indirect_shader_2_p0_c0.vert
	${X11_PATRACE_ROOT}/tools/shader_repacker --repack indirect_ $TESTTRACE1 tmp/tmp.pat
	set +x
	rm -f geom_shader_* indirect_shader_* tmp/tmp.pat

	echo "*** Testing DEDUPLICATOR"
	set -x
	${X11_PATRACE_ROOT}/tools/deduplicator --all $TESTTRACE3 tmp/dedup.pat
	set +x
	COUNT=$(${X11_PATRACE_ROOT}/tools/trace_to_txt tmp/dedup.pat 2>&1 | grep glUseProgram | wc -l)
	if [ "$COUNT" != "1" ]; then
		echo "Bad glUseProgram count: $COUNT"
		exit 1
	fi
	${X11_PATRACE_ROOT}/tools/deduplicator --all --replace $TESTTRACE3 tmp/dedup.pat
	COUNT=$(${X11_PATRACE_ROOT}/tools/trace_to_txt tmp/dedup.pat 2>&1 | grep glEnable\(cap=0xFF | wc -l)
	if [ "$COUNT" != "10" ]; then
		echo "Bad glEnable(GL_INVALID_INDEX) count: $COUNT"
		exit 1
	fi
	rm -f *.ra tmp/dedup.pat

	echo "*** Testing FASTFORWARDER"
	set -x
	${X11_PATRACE_BIN}/fastforward --version
	#${X11_PATRACE_BIN}/fastforward --noscreen --input $TESTTRACE3 --output ff_f4.pat --targetFrame 4 --endFrame 10
	#${SANITIZER_PARETRACE} -snapshotprefix ff_f4_ -framenamesnaps -s 4/frame -noscreen -overrideEGL 8 8 8 8 24 8 ff_f4.pat
	#cmp ff_f4_0004*.png tmp/png1/geometry_shader_1_0004*.png
	set +x

	echo "*** Testing MULTITHREADING"
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	mv multithread_1*.png run_multithread_1_1.png
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	mv multithread_1*.png run_multithread_1_2.png
	cmp run_multithread_1_1.png run_multithread_1_2.png
	${RELEASE_PARETRACE} -multithread -snapshotprefix multithread_1 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	mv multithread_1*.png run_multithread_1_3.png
	cmp run_multithread_1_1.png run_multithread_1_3.png
	#
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	mv multithread_2*.png run_multithread_2_1.png
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	mv multithread_2*.png run_multithread_2_2.png
	cmp run_multithread_2_1.png run_multithread_2_2.png
	${RELEASE_PARETRACE} -multithread -snapshotprefix multithread_2 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	mv multithread_2*.png run_multithread_2_3.png
	cmp run_multithread_2_1.png run_multithread_2_3.png
	#
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	mv multithread_3*.png run_multithread_3_1.png
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	mv multithread_3*.png run_multithread_3_2.png
	cmp run_multithread_3_1.png run_multithread_3_2.png
	${RELEASE_PARETRACE} -multithread -snapshotprefix multithread_3 -framenamesnaps -s 3/frame -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	mv multithread_3*.png run_multithread_3_3.png
	cmp run_multithread_3_1.png run_multithread_3_3.png

	echo "*** Stress-testing MULTITHREADING"
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -flushonswap -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -collect -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -callstats -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_1 -perfmon -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_1.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -callstats -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -flushonswap -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_2 -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_2.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -perfmon -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -flushonswap -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -callstats -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -all -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat
	${SANITIZER_PARETRACE} -multithread -snapshotprefix multithread_3 -collect -noscreen -overrideEGL 8 8 8 8 24 8 $OUT/multithread_3.1.pat

	echo "*** Testing SINGLE_SURFACE" # now largely obsolete
	set -x
	${X11_PATRACE_ROOT}/tools/single_surface -l $OUT/multithread_3.1.pat
	set +x

	echo "*** Testing REPLACE_SHADER"
	SOURCECALL=$(${X11_PATRACE_ROOT}/tools/totxt $TESTTRACE1 2>&1 | grep glShaderSource | head -1 | sed 's/.*] //'g |  sed 's/ .*//')
	set -x
	${X11_PATRACE_ROOT}/tools/replace_shader -d $TESTTRACE1 $SOURCECALL # creates shader.txt
	${X11_PATRACE_ROOT}/tools/replace_shader $TESTTRACE1 $SOURCECALL $OUT/tmp.pat # reads shader.txt
	${SANITIZER_PARETRACE} -noscreen $OUT/tmp.pat # make sure it works
	set +x

	echo "*** Testing PATCH FILES"
	set -x
	${X11_PATRACE_ROOT}/tools/replace_shader -p $TESTTRACE1 $SOURCECALL patch_shader.pf # reads shader.txt made above
	${X11_PATRACE_ROOT}/tools/totxt patch_shader.pf
	${X11_PATRACE_ROOT}/tools/totxt -p patch_shader.pf $TESTTRACE1 > /dev/null
	${X11_PATRACE_BIN}/paretrace -noscreen -patch patch_shader.pf $TESTTRACE1
	${X11_PATRACE_ROOT}/tools/deduplicator -p --all $TESTTRACE1 patch_dedup.pf
	${X11_PATRACE_ROOT}/tools/totxt patch_dedup.pf
	${X11_PATRACE_ROOT}/tools/totxt -p patch_dedup.pf $TESTTRACE1 > /dev/null
	${SANITIZER_PARETRACE} -noscreen -patch patch_dedup.pf $TESTTRACE1
	set +x
}

function az_subtest_1()
{
	set -x
	${X11_PATRACE_ROOT}/tools/analyze_trace -n -o tmp/$1 -r 1 $OUT/$1.1.pat
	${X11_PATRACE_ROOT}/tools/trace_to_txt $OUT/$1.1.pat tmp/$1.txt
	${X11_PATRACE_ROOT}/tools/totxt $OUT/$1.1.pat tmp/totxt_$1.txt
	scripts/validate_analysis.py tmp/$1 -1
	set +x
}

function az_subtest_2()
{
	set -x
	${X11_PATRACE_ROOT}/tools/analyze_trace -n -o tmp/$1 -r 1 -f 0 2 -j $OUT/$1.1.pat
	scripts/renderpass_json/validate.py tmp/$1_f1_rp1
	set +x
}

function az_subtest()
{
	az_subtest_1 $1
	#az_subtest_2 $1
}

function analyze_trace_test()
{
	echo "*** Testing ANALYZE_TRACE"
	az_subtest drawrange_1
	az_subtest drawrange_2
	az_subtest indirectdraw_1
	az_subtest indirectdraw_2
	az_subtest vertexbuffer_1
	az_subtest geometry_shader_1
	az_subtest copy_image_1
	az_subtest ext_texture_border_clamp
	az_subtest ext_texture_buffer
	az_subtest ext_texture_cube_map_array
	az_subtest ext_texture_sRGB_decode
	az_subtest khr_blend_equation_advanced
	az_subtest oes_sample_shading
	az_subtest oes_texture_stencil8
	az_subtest ext_gpu_shader5
	az_subtest multithread_1
	az_subtest multithread_2

	# -- The below do not work with the renderpass json feature
	az_subtest_1 multisurface_1
	az_subtest_1 multisample_1
	az_subtest_1 compute_1
	az_subtest_1 compute_2
	az_subtest_1 bindbufferrange_1
	az_subtest_1 imagetex_1
	az_subtest_1 khr_debug

	# -- The below needs glCreateShaderProgramv support in analyze_trace first
	#az_subtest programs_1
	#az_subtest_1 compute_3
}

### Run all the tests
#

integration_test
replayer_test
tools_test
analyze_trace_test

echo
echo "*** ALL DONE - all tests passed ***"
echo

# Cleanup...
rm -f *.ra
