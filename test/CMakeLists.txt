set(ELY_TESTS
    util)

find_package(doctest REQUIRED)

function(make_test target)
    string(CONCAT target_src ${target} ".cpp")
    add_executable(${target} ${target_src})
    target_link_libraries(${target} PRIVATE ely::ely doctest::doctest)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

foreach(t ${ELY_TESTS})
    make_test(${t})
endforeach()