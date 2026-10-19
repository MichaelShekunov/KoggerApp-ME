// Stand-in for the application's core.h. id_binnary.cpp includes it and declares `extern Core
// core;` but uses it nowhere live, so the parser classes can be checked without the whole app.
#pragma once
class Core {};
