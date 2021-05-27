cmake_minimum_required(VERSION 3.15)
project(BeeAgent)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

if(NOT TARGET spdlog)
    find_package(spdlog REQUIRED)
endif()

find_package(nlohmann_json 3.2.0 REQUIRED)
add_executable(bee_agent main.cc args.hxx)

if (APPLE)
    include_directories(/usr/local/include)
    link_directories(/usr/local/lib)
    target_link_libraries(bee_agent PRIVATE "/usr/local/lib/libuv.a" spdlog::spdlog nlohmann_json::nlohmann_json)
elseif(WINDOWS)
    target_link_libraries(bee_agent PRIVATE spdlog::spdlog)
else()
    include_directories(/usr/local/include)
    link_directories(/usr/local/lib)
    target_link_libraries(bee_agent PRIVATE libuv spdlog::spdlog nlohmann_json::nlohmann_json)
endif()