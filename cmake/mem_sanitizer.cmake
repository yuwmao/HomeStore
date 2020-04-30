set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined -fsanitize-address-use-after-scope -fno-sanitize=alignment -DCDS_ADDRESS_SANITIZER_ENABLED -fno-omit-frame-pointer -fno-optimize-sibling-calls -Wno-attributes")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address,undefined -fsanitize-address-use-after-scope -DCDS_ADDRESS_SANITIZER_ENABLED -fno-omit-frame-pointer -fno-optimize-sibling-calls")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fsanitize=undefined")
