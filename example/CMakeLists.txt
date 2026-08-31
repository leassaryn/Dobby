if(SYSTEM.Android)
  add_executable(android_example
    main.cc
    android_common_api.cc
    )

  target_link_libraries(android_example
    dobby
    )
endif()