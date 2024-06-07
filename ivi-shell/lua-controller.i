%module ivi
%{
#include "libweston/libweston.h"
#include "lua-controller.h"
%}

extern int ivi_print(const char *log);
