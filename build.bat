@echo off

set CL_FLAGS=/nologo /Zi /W4 /TC

if /I "%1" == "release" (
	set CL_FLAGS=%CL_FLAGS% /O2 /DNDEBUG
) else (
	set CL_FLAGS=%CL_FLAGS% /Od
)

cl lelcache.c %CL_FLAGS% /link Shell32.lib Ole32.lib
