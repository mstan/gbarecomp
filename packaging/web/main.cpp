// Minimal host for a web build: the generated game library carries no main(),
// so the browser bundle links this one (see packaging/web/build_web.sh).
#include "runtime.h"

int main(int argc, char** argv) { return gbarecomp::run_game(argc, argv); }
