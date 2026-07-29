project(MCDK_API LANGUAGES CXX C)

add_library(mcdk-api SHARED
    src/mcdk_api.cpp
)
target_compile_features(mcdk-api PRIVATE cxx_std_23)
target_compile_definitions(mcdk-api PRIVATE MCDK_API_BUILD)
target_include_directories(mcdk-api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(mcdk-api PRIVATE mcdevtool_game_discovery)

install(TARGETS mcdk-api
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES include/mcdk_api.h DESTINATION include)
