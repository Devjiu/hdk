set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(TEST_BASE_PATH "./tmp")
add_definitions("-DBASE_PATH=\"${TEST_BASE_PATH}\"")
add_definitions("-DTEST_SOURCE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/Udf/udf_sample.cpp DESTINATION ${CMAKE_BINARY_DIR}/Tests/Udf)

add_executable(CodeGeneratorTest CodeGeneratorTest.cpp)
add_executable(ArrowBasedExecuteTest ArrowBasedExecuteTest.cpp)
add_executable(StandaloneQueryRunner StandaloneQueryRunner.cpp)
add_executable(StringDictionaryTest StringDictionaryTest.cpp)
add_executable(StringTransformTest StringTransformTest.cpp)
add_executable(StringFunctionsTest StringFunctionsTest.cpp)
add_executable(EncoderTest EncoderTest.cpp)
add_executable(DataRecyclerTest DataRecyclerTest.cpp)
add_executable(ParallelSortTest ParallelSortTest.cpp)

if(ENABLE_CUDA)
  message(DEBUG "Tests CUDA_COMPILATION_ARCH: ${CUDA_COMPILATION_ARCH}")
  set(MAPD_DEFINITIONS -DHAVE_CUDA)
  if (SUPPRESS_NULL_LOGGER_DEPRECATION_WARNINGS)
    list(APPEND MAPD_DEFINITIONS -DSUPPRESS_NULL_LOGGER_DEPRECATION_WARNINGS)
  endif()
endif()

add_executable(ResultSetTest ResultSetTest.cpp ResultSetTestUtils.cpp)
add_executable(FromTableReorderingTest FromTableReorderingTest.cpp)
add_executable(ResultSetBaselineRadixSortTest ResultSetBaselineRadixSortTest.cpp ResultSetTestUtils.cpp)
add_executable(UtilTest UtilTest.cpp)
add_executable(ArrayTest ArrayTest.cpp)
add_executable(GroupByTest GroupByTest.cpp)
add_executable(TopKTest TopKTest.cpp)
add_executable(CorrelatedSubqueryTest CorrelatedSubqueryTest.cpp)
add_executable(DateTimeUtilsTest Shared/DateTimeUtilsTest.cpp)
add_executable(JoinHashTableTest JoinHashTableTest.cpp)
add_executable(CachedHashTableTest CachedHashTableTest.cpp)
add_executable(ColumnarResultsTest ColumnarResultsTest.cpp ResultSetTestUtils.cpp)
if(NOT MSVC)
  add_executable(QuantileCpuTest Quantile/QuantileCpuTest.cpp)
endif()
add_executable(NoCatalogRelAlgTest NoCatalogRelAlgTest.cpp TestRelAlgDagBuilder.cpp)
add_executable(NoCatalogSqlTest NoCatalogSqlTest.cpp)
add_executable(ArrowStorageTest ArrowStorageTest.cpp)
add_executable(ArrowStorageSqlTest ArrowStorageSqlTest.cpp)
add_executable(ResultSetArrowConversion ResultSetArrowConversion.cpp)
add_executable(ExecutionSequenceTest ExecutionSequenceTest.cpp TestRelAlgDagBuilder.cpp)
add_executable(QueryBuilderTest QueryBuilderTest.cpp TestRelAlgDagBuilder.cpp)
add_executable(PartitionedGroupByTest PartitionedGroupByTest.cpp)

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  add_executable(UdfTest UdfTest.cpp)
endif()
if(ENABLE_CUDA)
  add_executable(GpuSharedMemoryTest GpuSharedMemoryTest.cpp ResultSetTestUtils.cpp)
endif()

# Tests + Microbenchmarks
add_executable(StringDictionaryBenchmark StringDictionaryBenchmark.cpp)

if(ENABLE_L0)
  add_executable(L0MgrExecuteTest L0MgrExecuteTest.cpp)
  add_executable(SpirvBuildTest SpirvBuildTest.cpp)
  add_executable(DataMgrWithL0Test DataMgrWithL0Test.cpp)
  add_executable(IntelGPUEnablingTest IntelGPUEnablingTest.cpp)

  target_link_libraries(L0MgrExecuteTest L0Mgr gtest ${llvm_libs} Logger OSDependent)
  target_link_libraries(SpirvBuildTest gtest ${llvm_libs})
  target_link_libraries(DataMgrWithL0Test DataMgr gtest)
  target_link_libraries(IntelGPUEnablingTest gtest QueryEngine ArrowQueryRunner)

  add_test(L0MgrExecuteTest L0MgrExecuteTest ${TEST_ARGS})
  add_test(SpirvBuildTest SpirvBuildTest ${TEST_ARGS})
  add_test(DataMgrWithL0Test DataMgrWithL0Test ${TEST_ARGS})
  add_test(IntelGPUEnablingTest IntelGPUEnablingTest ${TEST_ARGS})
endif()

add_executable(CostModelTest CostModel/CostModelTest.cpp)
target_link_libraries(CostModelTest gtest CostModel ${ARMADILLO_LIBRARIES})

if (ENABLE_DWARF_BENCH)
  add_executable(DwarfBenchIntegrationTest CostModel/DwarfBenchIntegrationTest.cpp)
  target_link_libraries(DwarfBenchIntegrationTest gtest CostModel)
endif()

target_link_libraries(ResultSetTest gtest QueryEngine ArrowQueryRunner ArrowStorage ConfigBuilder)
target_link_libraries(ColumnarResultsTest gtest QueryEngine)
target_link_libraries(FromTableReorderingTest gtest QueryEngine)
target_link_libraries(ResultSetBaselineRadixSortTest gtest QueryEngine)
target_link_libraries(UtilTest Utils gtest Logger Shared ${Boost_LIBRARIES})
target_link_libraries(StringTransformTest Logger Shared gtest ${Boost_LIBRARIES})
target_link_libraries(StringFunctionsTest gtest QueryEngine ArrowQueryRunner)
target_link_libraries(CodeGeneratorTest gtest QueryEngine)
target_link_libraries(ArrowBasedExecuteTest gtest QueryEngine ArrowQueryRunner)
target_link_libraries(StandaloneQueryRunner gtest QueryEngine ArrowQueryRunner)
target_link_libraries(ArrayTest gtest QueryEngine ArrowQueryRunner ConfigBuilder)
target_link_libraries(GroupByTest gtest QueryEngine ArrowQueryRunner ConfigBuilder)
target_link_libraries(TopKTest gtest ArrowQueryRunner ArrowStorage Logger ConfigBuilder)

if(ENABLE_FOLLY) 
  target_link_libraries(StringDictionaryTest gtest StringDictionary Logger Utils $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs> ${CMAKE_DL_LIBS} ${Folly_LIBRARIES} ${Boost_LIBRARIES} ${ZLIB_LIBRARIES})
else()
  target_link_libraries(StringDictionaryTest gtest StringDictionary Logger Utils $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs> ${CMAKE_DL_LIBS} ${Boost_LIBRARIES} ${ZLIB_LIBRARIES})
endif()

target_link_libraries(CorrelatedSubqueryTest gtest QueryEngine ArrowQueryRunner ConfigBuilder)
target_link_libraries(DateTimeUtilsTest gtest Logger IR Shared ${LLVM_LINKER_FLAGS})
target_link_libraries(JoinHashTableTest gtest QueryEngine ArrowQueryRunner ConfigBuilder)
target_link_libraries(CachedHashTableTest gtest QueryEngine ArrowQueryRunner ConfigBuilder)
target_link_libraries(UtilTest OSDependent)
target_link_libraries(EncoderTest gtest ${Arrow_LIBRARIES} DataMgr Logger)
if(NOT MSVC)
	target_link_libraries(QuantileCpuTest gtest Logger TBB::tbb)
endif()
target_link_libraries(DataRecyclerTest gtest QueryEngine ArrowQueryRunner ConfigBuilder)
target_link_libraries(NoCatalogRelAlgTest gtest QueryEngine)
target_link_libraries(NoCatalogSqlTest gtest QueryEngine Calcite)
target_link_libraries(ArrowStorageTest gtest QueryEngine ArrowStorage)
target_link_libraries(ArrowStorageSqlTest gtest QueryEngine ArrowQueryRunner ArrowStorage)
target_link_libraries(ParallelSortTest gtest TBB::tbb Logger)
target_link_libraries(ResultSetArrowConversion gtest QueryEngine ArrowQueryRunner ArrowStorage ConfigBuilder)
target_link_libraries(ExecutionSequenceTest gtest QueryEngine ArrowQueryRunner ArrowStorage ConfigBuilder)
target_link_libraries(QueryBuilderTest gtest QueryBuilder QueryEngine ArrowQueryRunner IR ArrowStorage ConfigBuilder)
target_link_libraries(PartitionedGroupByTest gtest QueryEngine ArrowQueryRunner IR ArrowStorage ConfigBuilder)

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  target_link_libraries(UdfTest gtest UdfCompiler QueryEngine ArrowQueryRunner)
endif()

if(ENABLE_FOLLY)
  target_link_libraries(StringDictionaryBenchmark benchmark gtest StringDictionary Logger Utils $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs> ${CMAKE_DL_LIBS} ${Folly_LIBRARIES} ${Boost_LIBRARIES} ${ZLIB_LIBRARIES})
else()
  target_link_libraries(StringDictionaryBenchmark benchmark gtest StringDictionary Logger Utils $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs> ${CMAKE_DL_LIBS} ${Boost_LIBRARIES} ${ZLIB_LIBRARIES})
endif()

if(ENABLE_CUDA)
  target_link_libraries(GpuSharedMemoryTest gtest Logger QueryEngine)
endif()

set(TEST_ARGS "--gtest_output=xml:../")
add_test(UtilTest UtilTest ${TEST_ARGS})
add_test(ArrowBasedExecuteTest ArrowBasedExecuteTest ${TEST_ARGS})
add_test(NAME ArrowBasedExecuteTestColumnarOutput COMMAND ArrowBasedExecuteTest ${TEST_ARGS} "--enable-columnar-output")
add_test(CodeGeneratorTest CodeGeneratorTest ${TEST_ARGS})
add_test(ResultSetTest ResultSetTest ${TEST_ARGS})
add_test(ColumnarResultsTest ColumnarResultsTest ${TEST_ARGS})
add_test(FromTableReorderingTest FromTableReorderingTest ${TEST_ARGS})
add_test(JoinHashTableTest JoinHashTableTest ${TEST_ARGS})
add_test(CachedHashTableTest CachedHashTableTest ${TEST_ARGS})
add_test(ResultSetBaselineRadixSortTest ResultSetBaselineRadixSortTest ${TEST_ARGS})
add_test(StringDictionaryTest StringDictionaryTest ${TEST_ARGS})
add_test(NAME StringDictionaryHashTest COMMAND StringDictionaryTest ${TEST_ARGS} "--enable-string-dict-hash-cache")
add_test(StringTransformTest StringTransformTest ${TEST_ARGS})
add_test(StringFunctionsTest StringFunctionsTest ${TEST_ARGS})
add_test(ArrayTest ArrayTest ${TEST_ARGS})
add_test(GroupByTest GroupByTest ${TEST_ARGS})
add_test(TopKTest TopKTest ${TEST_ARGS})
add_test(CorrelatedSubqueryTest CorrelatedSubqueryTest ${TEST_ARGS})
add_test(DateTimeUtilsTest DateTimeUtilsTest ${TEST_ARGS})
add_test(JoinHashTableTest JoinHashTableTest ${TEST_ARGS})
add_test(EncoderTest EncoderTest ${TEST_ARGS})
add_test(DataRecyclerTest DataRecyclerTest ${TEST_ARGS})
add_test(NoCatalogRelAlgTest NoCatalogRelAlgTest ${TEST_ARGS})
add_test(NoCatalogSqlTest NoCatalogSqlTest ${TEST_ARGS})
add_test(ArrowStorageTest ArrowStorageTest ${TEST_ARGS})
add_test(ArrowStorageSqlTest ArrowStorageSqlTest ${TEST_ARGS})
add_test(ParallelSortTest ParallelSortTest ${TEST_ARGS})
add_test(ResultSetArrowConversion ResultSetArrowConversion ${TEST_ARGS})
add_test(ExecutionSequenceTest ExecutionSequenceTest ${TEST_ARGS})
add_test(QueryBuilderTest QueryBuilderTest ${TEST_ARGS})
add_test(PartitionedGroupByTest PartitionedGroupByTest ${TEST_ARGS})

add_test(NAME ArrowBasedExecuteTestColumnarOutputCpuOnly COMMAND ArrowBasedExecuteTest ${TEST_ARGS} "--enable-columnar-output")
set_tests_properties(ArrowBasedExecuteTestColumnarOutputCpuOnly PROPERTIES LABELS "cpu_only")
add_test(NAME StringDictionaryHashTestCpuOnly COMMAND StringDictionaryTest ${TEST_ARGS} "--enable-string-dict-hash-cache")
set_tests_properties(StringDictionaryHashTestCpuOnly PROPERTIES LABELS "cpu_only")

if(ENABLE_CUDA)
  add_test(GpuSharedMemoryTest GpuSharedMemoryTest ${TEST_ARGS})
endif()
if(ENABLE_L0)
  set(arrow_based_execute_skip_tests
    Select.DictionaryStringEquality
    Select.Case
    Select.GroupBy
    Select.GroupByBaselineHash
    Select.MultiStepColumnarization
    Select.Interop
    Select.ApproxCountDistinct
  )
  string(REPLACE ";" ":" filter_line "${arrow_based_execute_skip_tests}")
  string(PREPEND filter_line "--gtest_filter=-")
  add_test(NAME ArrowBasedExecuteTestIntel COMMAND ArrowBasedExecuteTest ${filter_line} ${TEST_ARGS})
  set_tests_properties(ArrowBasedExecuteTestIntel PROPERTIES LABELS "enabling")
endif()

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  add_test(UdfTest UdfTest ${TEST_ARGS})
endif()

find_package(fmt REQUIRED)

#parse s3 credentials
file(READ aws/s3client.conf S3CLIENT_CONF)
if("${S3CLIENT_CONF}" MATCHES "AWS_ACCESS_KEY_ID=([^\n]+)")
  set(AWS_ACCESS_KEY_ID "${CMAKE_MATCH_1}")
endif()
if("${S3CLIENT_CONF}" MATCHES "AWS_SECRET_ACCESS_KEY=([^\n]+)")
  set(AWS_SECRET_ACCESS_KEY "${CMAKE_MATCH_1}")
endif()
if("${S3CLIENT_CONF}" MATCHES "AWS_REGION=([^\n]+)")
  set(AWS_REGION "${CMAKE_MATCH_1}")
endif()

set(TEST_PROGRAMS 
  ArrowBasedExecuteTest
  CodeGeneratorTest
  ResultSetTest
  ColumnarResultsTest
  FromTableReorderingTest
  ResultSetBaselineRadixSortTest
  ArrayTest
  GroupByTest
  TopKTest
  CorrelatedSubqueryTest
  DateTimeUtilsTest
  JoinHashTableTest
  StringFunctionsTest
  StringDictionaryTest
  EncoderTest
  DataRecyclerTest
  NoCatalogRelAlgTest
  NoCatalogSqlTest
  ArrowStorageTest
  ArrowStorageSqlTest
  ParallelSortTest
  ResultSetArrowConversion
  ExecutionSequenceTest
  QueryBuilderTest
  PartitionedGroupByTest
)

if(ENABLE_CUDA)
  list(APPEND TEST_PROGRAMS GpuSharedMemoryTest)
endif()

#if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
#  list(APPEND TEST_PROGRAMS UdfTest)
#endif()

function(add_default_cpu_only_tests TEST_LIST)
  foreach(t ${TEST_LIST})
    add_test("${t}CpuOnly" ${t} ${TEST_ARGS} "--cpu-only")
    set_tests_properties("${t}CpuOnly" PROPERTIES LABELS "cpu_only")
  endforeach()
endfunction()
add_default_cpu_only_tests("${TEST_PROGRAMS}")
set(EXCLUDE_PLATFORM_SPECIFIC_TESTS CodeGeneratorTest)


set(SANITY_TESTS ${TEST_PROGRAMS})
list(APPEND SANITY_TESTS ArrowBasedExecuteTestColumnarOutput)
list(APPEND SANITY_TESTS StringDictionaryHashTest)

set_tests_properties(${SANITY_TESTS} PROPERTIES LABELS "sanity")

if (WIN32)
  # The following command doesn't create a directory if it already
  # exists but prints and error if this directore cannot be created.
  set(MKDIR_TMP dir tmp >nul 2>nul || mkdir tmp >nul 2>nul || mkdir tmp)
  # On windows "env" command is available as a part of binaries that
  # conda installs in omnisci-dev environment. The problem with this
  # "env" command is that it converts paths in %PATH% into mingw style
  # paths where %CONDA_PREFIX%\Library\bin path is converted into
  # /usr/bin or /bin path that are valid only for mingw
  # applications. Running HDK tests with PATH pointing somewhere to
  # /usr/bin breaks DLL search for libraries installed into
  # %CONDA_PREFIX%\Library\bin, e.g. all boost libraries. So instead
  # of "env" from conda we need to use a windows replacement here.
  # Note starting \" quote here and finishing \" in
  # EXCLUDE_SANITY_TESTS. It makes the whole command to be enclosed in
  # quotes that /s instructs to strip.
  set(ENV_COMMAND cmd /s /v /c \"set AWS_REGION=${AWS_REGION} && set AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID} && set AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY} && )
  # The following tests fail on Windows:
  # ArrowBasedExecuteTest, ArrowStorageSqlTest, NoCatalogRelAlgTest
  # ExecutionSequenceTest, CorrelatedSubqueryTest
  # excluded because of https://github.com/intel-ai/hdk/issues/488.
  # ResultSetTest excluded because of https://github.com/intel-ai/hdk/issues/489
  # StringDictionaryTest excluded because of https://github.com/intel-ai/hdk/issues/490
  # QueryBuilderTest excluded because of https://github.com/intel-ai/hdk/issues/491
  set(EXCLUDE_SANITY_TESTS "-E \"(ArrowBasedExecuteTest^|ResultSetTest^|StringDictionaryTest^|QueryBuilderTest^|ArrowStorageSqlTest^|NoCatalogRelAlgTest^|ExecutionSequenceTest^|CorrelatedSubqueryTest)\"" \")
else()
  set(MKDIR_TMP mkdir -p tmp)
  set(ENV_COMMAND env AWS_REGION=${AWS_REGION} AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID} AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY})
endif()

add_custom_target(sanity_tests
    COMMAND ${MKDIR_TMP}
    COMMAND touch tmp/DictPayload
    COMMAND ${ENV_COMMAND} $<SHELL_PATH:${CMAKE_CTEST_COMMAND}> --verbose --label-regex sanity ${EXCLUDE_SANITY_TESTS}
    DEPENDS ${TEST_PROGRAMS}
    USES_TERMINAL)

add_custom_target(sanity_cpu_only_tests
    COMMAND ${MKDIR_TMP}
    COMMAND touch tmp/DictPayload
    COMMAND ${ENV_COMMAND} $<SHELL_PATH:${CMAKE_CTEST_COMMAND}> --verbose --label-regex cpu_only -E ${EXCLUDE_PLATFORM_SPECIFIC_TESTS} ${EXCLUDE_SANITY_TESTS}
    DEPENDS ${TEST_PROGRAMS}
    USES_TERMINAL)

cmake_host_system_information(RESULT CPU_NUM QUERY NUMBER_OF_PHYSICAL_CORES)
add_custom_target(sanity_tests_par
    COMMAND ${MKDIR_TMP}
    COMMAND touch tmp/DictPayload
    COMMAND ${ENV_COMMAND} $<SHELL_PATH:${CMAKE_CTEST_COMMAND}> --verbose --label-regex sanity -j ${CPU_NUM} ${EXCLUDE_SANITY_TESTS}
    DEPENDS ${TEST_PROGRAMS}
    USES_TERMINAL)

add_custom_target(all_tests
    COMMAND ${MKDIR_TMP}
    COMMAND touch tmp/DictPayload
    COMMAND ${ENV_COMMAND} $<SHELL_PATH:${CMAKE_CTEST_COMMAND}> --verbose ${EXCLUDE_SANITY_TESTS}
    DEPENDS ${TEST_PROGRAMS} UtilTest StringDictionaryTest StringTransformTest
    USES_TERMINAL)

add_custom_target(topk_tests
    COMMAND ${MKDIR_TMP}
    COMMAND touch tmp/DictPayload
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose --tests-regex "\"(TopKTest)\""
    DEPENDS TopKTest)

if(ENABLE_L0)
  set(ENABLING_TESTS
    ArrowStorageSqlTest # taxi queries
    SpirvBuildTest
    L0MgrExecuteTest
    IntelGPUEnablingTest
  )
  set_tests_properties(${ENABLING_TESTS} PROPERTIES LABELS "enabling")

  add_custom_target(enabling_tests
      COMMAND ${MKDIR_TMP}
      COMMAND touch tmp/DictPayload
      COMMAND ${CMAKE_CTEST_COMMAND} --verbose --label-regex enabling
      DEPENDS ${ENABLING_TESTS}
      USES_TERMINAL)
endif()
