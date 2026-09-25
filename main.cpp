// main.cpp — xaml2winuic.exe entry point
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

#include <cstdlib>
#include <set>
#include <string>

import xamlconv.cli;

auto main(int argc, char** argv) -> int {
    return xamlconv::cli::run(argc, argv);
}
