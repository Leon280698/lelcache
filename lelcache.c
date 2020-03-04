#include <Windows.h>
#include <ShlObj.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <wchar.h>

#pragma warning(push, 0)
#define XXH_INLINE_ALL
#include "./xxhash/xxhash.h"
#pragma warning(pop)

/*
 * TODO:
 * - Handle edge case when source file has not changed but the name of the associated pdb is different.
 *    -> Copy object file from cache and manually change the pdb name inside (COFF).
 */

#define DEFAULT_CACHE_SIZE_GIGABYTES 4

#define LEL_HASH64_STRING_LENGTH (sizeof(XXH64_hash_t) * 2) // Two characters per byte

typedef WCHAR Hash64String[LEL_HASH64_STRING_LENGTH + 1];

void hash64_to_string(XXH64_hash_t hash, Hash64String out) {
	LPCWSTR hexLookup = L"0123456789abcdef";
	BYTE* bytes = (BYTE*)&hash;

	for(int i = 0; i < sizeof(XXH64_hash_t); ++i) {
		int strIndex = i * 2;
		out[strIndex] = hexLookup[bytes[i] & 0x0F];
		out[strIndex + 1] = hexLookup[(bytes[i] >> 4) & 0xF];
	}

	out[LEL_HASH64_STRING_LENGTH] = L'\0';
}

void path_from_hash64_string(Hash64String str, LPWSTR buffer) {
	for(int i = 0; i < sizeof(XXH64_hash_t); i += 2) {
		*buffer = str[i];
		++buffer;
		*buffer = str[i + 1];
		++buffer;
		*buffer = L'\\';
		++buffer;
	}

	*buffer = L'\0';
}

LPWSTR file_name_from_path(LPWSTR filePath) {
	LPWSTR tmp = filePath;

	while(*tmp)
		++tmp;

	while(tmp != filePath && *(tmp - 1) != L'/' && *(tmp - 1) != L'\\')
		--tmp;

	return tmp;
}

LPCWSTR file_extension_from_path(LPCWSTR filePath) {
	LPCWSTR tmp = filePath;

	while(*tmp)
		++tmp;

	LPCWSTR end = tmp;

	while(tmp != filePath && *(tmp - 1) != L'.') {
		--tmp;

		if(*tmp == L'/' || *tmp == L'\\') {
			tmp = end;

			break;
		}
	}

	return tmp;
}

int __cdecl compare_strings_for_qsort(const void* a, const void* b) {
	return lstrcmpW((LPCWSTR)a, (LPCWSTR)b);
}


void make_cmd_line(int argc, LPCWSTR* argv, LPWSTR buffer) {
	int offset = 0;

	for(int i = 0; i < argc; ++i) {
		buffer[offset++] = L'\"';
		lstrcpyW(buffer + offset, argv[i]);
		offset += lstrlenW(argv[i]);;
		buffer[offset++] = L'\"';
		buffer[offset++] = L' ';
	}

	buffer[offset - 1] = '\0';
}

// Should be more than enough for pretty much any case...
#define MAX_PREPROCESSOR_FLAGS 128
#define MAX_COMPILER_FLAGS 128

struct CommandLineInfo {
	XXH64_hash_t compilerCmdLineHash;
	LPWSTR sourceFile;
	LPWSTR objectFile;
	LPWSTR pdbFile;
	LPWSTR temporaryPreprocessedFile;
	LPWSTR temporaryCompiledObjectFile;
	LPWSTR temporaryDebugInformationDatabase;
	int numPreprocessorFlags;
	int preprocessorCmdLineLength;
	int numCompilerFlags;
	int compilerCmdLineLength;
	LPWSTR preprocessorFlags[MAX_PREPROCESSOR_FLAGS];
	LPWSTR compilerFlags[MAX_COMPILER_FLAGS];
	WCHAR preprocessorOutputFile[MAX_PATH];
	WCHAR compilerOutputFile[MAX_PATH];
	WCHAR debugInformationOutputFile[MAX_PATH];
	WCHAR objectFileBuffer[MAX_PATH];
};

BOOL is_linker_flag(LPCWSTR flag) {
	return (flag[0] == L'F' && iswdigit(flag[1])) ||
		   *flag == L'l' ||
		   *flag == L'L';
}

BOOL is_preprocessor_flag(LPCWSTR flag) {
	return (flag[0] == L'A' && flag[1] == L'I') ||
		   *flag == L'C' ||
		   *flag == L'D' ||
		   (flag[0] == L'E' && flag[1] != L'H') ||
		   (flag[0] == L'F' && (flag[1] == L'I' || flag[1] == L'U' || flag[1] == L'x')) ||
		   *flag == L'I' ||
		   *flag == L'P' ||
		   *flag == L'U' ||
		   *flag == L'u' ||
		   *flag == L'X';
}

void add_preprocessor_flag(struct CommandLineInfo* cmdLineInfo, LPWSTR flag) {
	if(cmdLineInfo->numPreprocessorFlags >= MAX_PREPROCESSOR_FLAGS) {
		wprintf(L"ERROR: More than %i preprocessor flags. WTF are you doing?\n", MAX_PREPROCESSOR_FLAGS);
		ExitProcess(EXIT_FAILURE);
	}

	cmdLineInfo->preprocessorFlags[cmdLineInfo->numPreprocessorFlags] = flag;
	cmdLineInfo->preprocessorCmdLineLength += lstrlenW(cmdLineInfo->preprocessorFlags[cmdLineInfo->numPreprocessorFlags]);
	++cmdLineInfo->numPreprocessorFlags;
}

void add_compiler_flag(struct CommandLineInfo* cmdLineInfo, LPWSTR flag) {
	if(cmdLineInfo->numCompilerFlags >= MAX_COMPILER_FLAGS) {
		wprintf(L"ERROR: More than %i compiler flags. WTF are you doing?\n", MAX_COMPILER_FLAGS);
		ExitProcess(EXIT_FAILURE);
	}

	cmdLineInfo->compilerFlags[cmdLineInfo->numCompilerFlags] = flag;
	cmdLineInfo->compilerCmdLineLength += lstrlenW(cmdLineInfo->compilerFlags[cmdLineInfo->numCompilerFlags]);
	++cmdLineInfo->numCompilerFlags;
}

/*
 * Parses the compiler command line and extracts necessary information like input/output files etc.
 * Returns FALSE if command line is not understood and thus should be directly forwarded to the compiler instead
 * of trying to find a cached object file.
 * A command line is considered not supported if it contains more than one input file, linker flags or it does not
 * compile a single object file (/c).
 * Compiler flags are sorted alphabetically and then hashed. This is technically not correct since it could give a wrong
 * result in edge cases where a flag overwrites a previous one (e.g. /Zi and /Z7) but this is considered a usage error and
 * thus is not handled. It will probably trigger an error in the future though.
 */
BOOL parse_cl_command_line(int argc, LPWSTR* argv, struct CommandLineInfo* cmdLineInfo) {
	BOOL compilesToObj = FALSE;
	BOOL generatesPdb = FALSE;
	BOOL noLogo = FALSE;

	// Preprocessor command line initial setup

	add_preprocessor_flag(cmdLineInfo, argv[1]); // cl.exe
	add_preprocessor_flag(cmdLineInfo, L"/EP");
	add_preprocessor_flag(cmdLineInfo, L"/P");
	add_preprocessor_flag(cmdLineInfo, L"/nologo");

	int endAdditionalPreprocessorArgs = cmdLineInfo->numPreprocessorFlags;

	// Compiler command line initial setup

	add_compiler_flag(cmdLineInfo, argv[1]); // cl.exe

	// Parsing command line

	for(int i = 2; i < argc; ++i) { // We start at 2 since the first two arguments are always lelcache.exe and cl.exe
		if(*argv[i] == L'/' || *argv[i] == L'-') {
			LPCWSTR flag = argv[i] + 1;

			if(is_linker_flag(flag))
				return FALSE;

			if(is_preprocessor_flag(flag)) {
				if(*flag == L'E' || *flag == L'P') // Only preprocess, don't compile
					return FALSE;

				add_preprocessor_flag(cmdLineInfo, argv[i]);

				continue;
			}

			// Checking for output file names

			if(*flag == L'F') {
				LPCWSTR* outputFileStr = NULL;

				switch(flag[1]) {
				case L'd':
					outputFileStr = &cmdLineInfo->pdbFile;
					break;
				case L'o':
					outputFileStr = &cmdLineInfo->objectFile;
					break;
				}

				if(outputFileStr) {
					flag += 2; // Skipping 'Fx'

					if(*flag == L':') // ':' is optional
						++flag;

					while(iswspace(*flag))
						++flag;

					*outputFileStr = flag;

					continue;
				}
			}

			// /nologo is a commonly used flag whose presence has no effect on the outcome of the compilation.
			// That's why we check whether it exists and add it to the command line later after it was hashed.
			if(lstrcmpW(flag, L"nologo") == 0) {
				noLogo = TRUE;

				continue;
			}

			// Default case: adding flag to compiler command line

			if(flag[0] == L'c' && flag[1] == L'\0')
				compilesToObj = TRUE;

			if(flag[0] == L'Z' && (flag[1] == L'i' || flag[1] == 'I'))
				generatesPdb = TRUE;

			add_compiler_flag(cmdLineInfo, argv[i]);
		} else {
			if(cmdLineInfo->sourceFile)
				return FALSE; // Multiple source files are not supported at the moment

			cmdLineInfo->sourceFile = argv[i];
		}
	}

	if(compilesToObj && cmdLineInfo->sourceFile) {
		// Preprocessor options

		WCHAR tempFileName[MAX_PATH];
		LPWSTR sourceFileName = file_name_from_path(cmdLineInfo->sourceFile);
		//LPCWSTR srcFileExt = file_extension_from_path(cmdLineInfo->sourceFile);

		GetTempFileNameW(L".", sourceFileName, 0, tempFileName);

		lstrcpyW(cmdLineInfo->preprocessorOutputFile, L"/Fi:");
		lstrcatW(cmdLineInfo->preprocessorOutputFile, tempFileName);

		cmdLineInfo->temporaryPreprocessedFile = cmdLineInfo->preprocessorOutputFile + ARRAYSIZE(L"/Fi:") - 1;

		add_preprocessor_flag(cmdLineInfo, cmdLineInfo->preprocessorOutputFile);
		add_preprocessor_flag(cmdLineInfo, cmdLineInfo->sourceFile);

		// Compiler options

		if(!cmdLineInfo->objectFile) { // If object file was not specified it is assumed to be the base name of the source file with the extension '.obj'
			lstrcpyW(cmdLineInfo->objectFileBuffer, file_name_from_path(cmdLineInfo->sourceFile));

			LPWSTR ext = (LPWSTR)file_extension_from_path(cmdLineInfo->objectFileBuffer);

			*ext = L'\0';
			lstrcatW(ext, L"obj");

			cmdLineInfo->objectFile = cmdLineInfo->objectFileBuffer;
		}

		lstrcpyW(cmdLineInfo->compilerOutputFile, L"/Fo:");
		lstrcatW(cmdLineInfo->compilerOutputFile, cmdLineInfo->objectFile);

		// Sort and hash compiler command line
		LPWSTR* sortedArgv = _malloca(cmdLineInfo->numCompilerFlags * sizeof(LPWSTR));
		LPWSTR tempCmdLine = _malloca((cmdLineInfo->compilerCmdLineLength + cmdLineInfo->numCompilerFlags * 3) * sizeof(WCHAR));

		memcpy(sortedArgv, cmdLineInfo->compilerFlags, cmdLineInfo->numCompilerFlags * sizeof(LPWSTR));
		qsort(sortedArgv, cmdLineInfo->numCompilerFlags, sizeof(*sortedArgv), compare_strings_for_qsort);
		make_cmd_line((int)cmdLineInfo->numCompilerFlags, cmdLineInfo->compilerFlags, tempCmdLine);
		cmdLineInfo->compilerCmdLineHash = XXH64(tempCmdLine, lstrlenW(tempCmdLine) * sizeof(*tempCmdLine), 0);

		_freea(tempCmdLine);
		_freea(sortedArgv);

		if(noLogo)
			add_compiler_flag(cmdLineInfo, L"/nologo");

		for(int i = endAdditionalPreprocessorArgs; i < cmdLineInfo->numPreprocessorFlags - 2; ++i) // Adding all preprocessor flags except /P and the ones for input and output files to the compiler command line
			add_compiler_flag(cmdLineInfo, cmdLineInfo->preprocessorFlags[i]);

		add_compiler_flag(cmdLineInfo, cmdLineInfo->compilerOutputFile);

		if(generatesPdb) {
			if(!cmdLineInfo->pdbFile)
				cmdLineInfo->pdbFile = L"vc140.pdb"; // Strange visual studio default behavior...

			lstrcpyW(cmdLineInfo->debugInformationOutputFile, L"/Fd:");
			lstrcatW(cmdLineInfo->debugInformationOutputFile, cmdLineInfo->pdbFile);
			add_compiler_flag(cmdLineInfo, cmdLineInfo->debugInformationOutputFile);
		} else {
			cmdLineInfo->pdbFile = NULL; // Resetting this in case /Fd was used without /Zi
		}

		add_compiler_flag(cmdLineInfo, cmdLineInfo->sourceFile);

		return TRUE;
	}

	return FALSE;
}

BOOL launch_process(LPCWSTR executable, LPWSTR cmdLine, LPPROCESS_INFORMATION outProcessInfo, BOOL silent) {
	STARTUPINFOW startupInfo = {0};

	startupInfo.cb = sizeof(startupInfo);

	if(silent) {
		startupInfo.dwFlags = STARTF_USESTDHANDLES;
		startupInfo.hStdOutput = CreateFileW(L"nul:", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	}

	BOOL result = CreateProcessW(executable, cmdLine, NULL, NULL, FALSE, 0, 0, NULL, &startupInfo, outProcessInfo);

	if(!result)
		wprintf(L"Unable to start %s\n", executable);

	return result;
}

DWORD wait_for_process(LPPROCESS_INFORMATION processInfo) {
	DWORD exitCode = 0;

	WaitForSingleObject(processInfo->hProcess, INFINITE);
	GetExitCodeProcess(processInfo->hProcess, &exitCode);
	CloseHandle(processInfo->hThread);
	CloseHandle(processInfo->hProcess);

	return exitCode;
}

XXH64_hash_t hash_file_content(LPWSTR filePath) {
	XXH64_hash_t hash = 0;
	HANDLE file = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if(file != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER fileSize;

		if(GetFileSizeEx(file, &fileSize)) {
			HANDLE heap = GetProcessHeap();
			LPVOID mem = HeapAlloc(heap, 0, (SIZE_T)fileSize.QuadPart);
			DWORD numBytesRead;

			if(!ReadFile(file, mem, (DWORD)fileSize.QuadPart, &numBytesRead, NULL))
				wprintf(L"Unable to read file content '%s'\n", filePath);

			hash = XXH64(mem, numBytesRead, 0);

			HeapFree(heap, 0, mem);
		} else {
			wprintf(L"Unable to determine file size for '%s'\n", filePath);
		}

		CloseHandle(file);
	} else {
		wprintf(L"Unable to open file '%s'\n", filePath);
	}

	return hash;
}

UINT64 file_size(LPCWSTR filePath) {
	WIN32_FILE_ATTRIBUTE_DATA fileAttribData;

	return GetFileAttributesExW(filePath, GetFileExInfoStandard, &fileAttribData) ?
		(UINT64)fileAttribData.nFileSizeHigh << 32 | fileAttribData.nFileSizeLow : 0;
}

BOOL file_exists(LPCWSTR path) {
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

BOOL make_path(LPCWSTR path) {
	WCHAR tempPath[MAX_PATH] = {0};
	int index = 0;

	while(*path) {
		do {
			tempPath[index++] = *path;
			++path;
		} while(*path && *path != L'\\');

		DWORD attributes = GetFileAttributesW(tempPath);

		if((attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)) { // Create directory only if it doesn't exist
			if(!CreateDirectoryW(tempPath, NULL)) {
				wprintf(L"Unable to create directory '%s'\n", tempPath);

				return FALSE;
			}
		}
	}

	return TRUE;
}

struct CacheConfig {
	UINT64 maxCacheSize;
	WCHAR cachePath[MAX_PATH];
} globalConfig = {0};

BOOL cache_config(struct CacheConfig* config, BOOL write) {
	PWSTR appDataLocal;
	WCHAR cacheConfigPath[MAX_PATH];

	SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &appDataLocal);
	lstrcpyW(cacheConfigPath, appDataLocal);
	CoTaskMemFree(appDataLocal);
	lstrcatW(cacheConfigPath, L"\\lelcache");
	make_path(cacheConfigPath);
	lstrcatW(cacheConfigPath, L"\\cache.config");

	if(write) {
		HANDLE file = CreateFileW(cacheConfigPath, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if(file == INVALID_HANDLE_VALUE) {
			wprintf(L"Unable to open config file at '%s' for writing\n", cacheConfigPath);

			return FALSE;
		}

		DWORD numBytesWritten;

		WriteFile(file, config, sizeof(*config), &numBytesWritten, NULL);
		CloseHandle(file);
	} else {
		if(file_exists(cacheConfigPath)) { // If file exists read it, otherwise initialize info with default values
			HANDLE file = CreateFileW(cacheConfigPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

			if(file == INVALID_HANDLE_VALUE) {
				wprintf(L"Unable to open config file at '%s' for reading\n", cacheConfigPath);

				return FALSE;
			}

			DWORD numBytesRead;
			BOOL success = ReadFile(file, config, sizeof(*config), &numBytesRead, NULL);

			CloseHandle(file);

			return success;
		} else {
			PWSTR cachePath;

			*config = (struct CacheConfig){0};
			config->maxCacheSize = DEFAULT_CACHE_SIZE_GIGABYTES * 1024ll * 1024ll * 1024ll;
			SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &cachePath); // Default cache path is in the user directory
			lstrcpyW(config->cachePath, cachePath);
			CoTaskMemFree(cachePath);
		}
	}

	return TRUE;
}

struct CacheInfo {
	UINT32 numCacheHits;
	UINT32 numCacheMisses; // Does not include cases when the command line was not understood and the compiler was called directly. TODO: should it?
	UINT64 currentCacheSize;
};

BOOL cache_info(struct CacheInfo* info, BOOL write) {
	WCHAR cacheInfoPath[MAX_PATH];

	lstrcpyW(cacheInfoPath, globalConfig.cachePath);
	lstrcatW(cacheInfoPath, L"\\.lelcache");
	make_path(cacheInfoPath);
	lstrcatW(cacheInfoPath, L"\\cache.info");

	if(write) {
		HANDLE file = CreateFileW(cacheInfoPath, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if(file == INVALID_HANDLE_VALUE) {
			wprintf(L"Unable to open cache info file at '%s' for writing\n", cacheInfoPath);

			return FALSE;
		}

		DWORD numBytesWritten;

		WriteFile(file, info, sizeof(*info), &numBytesWritten, NULL);
		CloseHandle(file);
	} else {
		if(file_exists(cacheInfoPath)) { // If file exists read it, otherwise initialize info with default values
			HANDLE file = CreateFileW(cacheInfoPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

			if(file == INVALID_HANDLE_VALUE) {
				wprintf(L"Unable to open cache info file at '%s' for reading\n", cacheInfoPath);

				return FALSE;
			}

			DWORD numBytesRead;
			BOOL success = ReadFile(file, info, sizeof(*info), &numBytesRead, NULL);

			CloseHandle(file);

			return success;
		} else {
			*info = (struct CacheInfo){0};
		}
	}

	return TRUE;
}

int lelcache_main(int argc, LPWSTR* argv) {
	if(lstrcmpW(file_name_from_path(argv[1]), L"cl.exe") != 0) {
		wprintf(L"First argument is expected to be the path to cl.exe\n");

		return EXIT_FAILURE;
	}

	int exitCode = EXIT_SUCCESS;
	struct CommandLineInfo cmdLineInfo = {0};
	PROCESS_INFORMATION processInfo = {0};

	if(parse_cl_command_line(argc, argv, &cmdLineInfo)) {
		LPWSTR cmdLineBuffer = _malloca((max(cmdLineInfo.preprocessorCmdLineLength, cmdLineInfo.compilerCmdLineLength) +
										 max(cmdLineInfo.numPreprocessorFlags, cmdLineInfo.numCompilerFlags) * 3) *
										sizeof(WCHAR));

		make_cmd_line((int)cmdLineInfo.numPreprocessorFlags, cmdLineInfo.preprocessorFlags, cmdLineBuffer);

		if(launch_process(argv[1], cmdLineBuffer, &processInfo, TRUE) && wait_for_process(&processInfo) == 0) {
			HANDLE cacheInfoMutex = CreateMutexW(NULL, FALSE, L"lelcacheinfofile");
			assert(cacheInfoMutex); // Does this ever fail?
			struct CacheInfo cacheInfo;
			WCHAR hashPath[MAX_PATH];
			XXH64_hash_t hash = hash_file_content(cmdLineInfo.temporaryPreprocessedFile);
			Hash64String hashStr;

			lstrcpyW(hashPath, globalConfig.cachePath);
			lstrcatW(hashPath, L"\\.lelcache\\");
			hash64_to_string(hash, hashStr);
			path_from_hash64_string(hashStr, hashPath + lstrlenW(hashPath));
			hash64_to_string(cmdLineInfo.compilerCmdLineHash, hashStr);
			lstrcatW(hashPath, hashStr);

			LPWSTR hashPathEnd = hashPath + lstrlenW(hashPath);

			lstrcpyW(hashPathEnd, L"\\obj");

			if(file_exists(hashPath)) {
				WaitForSingleObject(cacheInfoMutex, INFINITE);
				cache_info(&cacheInfo, FALSE);
				++cacheInfo.numCacheHits;
				cache_info(&cacheInfo, TRUE);
				ReleaseMutex(cacheInfoMutex);
				CopyFileW(hashPath, cmdLineInfo.objectFile, FALSE);

				if(cmdLineInfo.pdbFile) {
					lstrcpyW(hashPathEnd, L"\\pdb");

					if(file_exists(hashPath))
						CopyFileW(hashPath, cmdLineInfo.pdbFile, FALSE);
					else
						wprintf(L"Cached pdb file not found for '%s', at '%s'\n", cmdLineInfo.sourceFile, hashPath); // This should never happen unless somebody deletes it on purpose
				}
			} else {
				make_cmd_line((int)cmdLineInfo.numCompilerFlags, cmdLineInfo.compilerFlags, cmdLineBuffer);

				*hashPathEnd = L'\0'; // Stripping '\obj' since it is a file and not part of the path

				if(make_path(hashPath) && launch_process(argv[1], cmdLineBuffer, &processInfo, FALSE)) {
					exitCode = wait_for_process(&processInfo);

					if(exitCode == 0) {
						UINT64 additionalHashSize = 0;

						lstrcpyW(hashPathEnd, L"\\obj");
						CopyFileW(cmdLineInfo.objectFile, hashPath, FALSE);

						additionalHashSize += file_size(hashPath);

						if(cmdLineInfo.pdbFile) {
							lstrcpyW(hashPathEnd, L"\\pdb");
							CopyFileW(cmdLineInfo.pdbFile, hashPath, FALSE);

							additionalHashSize += file_size(hashPath);
						}

						WaitForSingleObject(cacheInfoMutex, INFINITE);
						cache_info(&cacheInfo, FALSE);
						++cacheInfo.numCacheMisses;
						cacheInfo.currentCacheSize += additionalHashSize;
						cache_info(&cacheInfo, TRUE);
						ReleaseMutex(cacheInfoMutex);
					}
				}
			}

			DeleteFileW(cmdLineInfo.temporaryPreprocessedFile);
			CloseHandle(cacheInfoMutex);
		} else {
			exitCode = EXIT_FAILURE;
		}

		_freea(cmdLineBuffer);
	} else {
		int cmdLineLen = (argc - 1) * 3; // Enough for surrounding quotes and separating space

		for(int i = 1; i < argc; ++i)
			cmdLineLen += lstrlenW(argv[i]);

		LPWSTR cmdLine = _malloca(cmdLineLen * sizeof(WCHAR));

		make_cmd_line(argc - 1, argv + 1, cmdLine);

		exitCode = launch_process(argv[1], cmdLine, &processInfo, FALSE) ? wait_for_process(&processInfo) : EXIT_FAILURE;

		_freea(cmdLine);
	}

	return exitCode;
}

void print_help() {
	wprintf(L"Usage:\n"
			L"    lelcache.exe <path_to_cl.exe> <cl_args>"
			L"  or\n"
			L"    lelcache.exe <options>\n"
			L"\n"
			L"Available options:\n"
			L" -h      show this help\n"
			L" -i      show info\n"
			L" -m<n>   set maximum cache size to n megabytes\n"
			L" -p<d>   set cache path to directory d\n");
}

int wmain(int argc, LPWSTR* argv, LPWSTR* envp) {
	UNREFERENCED_PARAMETER(envp);

	if(argc <= 1) {
		print_help();

		return EXIT_FAILURE;
	}

	if (!cache_config(&globalConfig, FALSE))
		return EXIT_FAILURE;

	if(*argv[1] == L'-') {
		for(int i = 1; i < argc; ++i) {
			if(*argv[i] != L'-') {
				wprintf(L"Unknown option '%s'\n", argv[i]);

				return EXIT_FAILURE;
			}

			LPWSTR arg = argv[i] + 1;
			struct CacheInfo info;

			switch(*arg) {
			case L'h':
				print_help();
				break;
			case 'i':
				if(cache_info(&info, FALSE)) {
					wprintf(L"cache hits:         %lu\n"
							L"cache misses:       %lu\n"
							L"cache hit rate:     %.2f%%\n"
							L"current cache size: %llu MB\n"
							L"maximum cache size: %llu MB\n"
							L"cache location:     %s\n",
							info.numCacheHits,
							info.numCacheMisses,
							(float)info.numCacheHits / (info.numCacheHits + info.numCacheMisses) * 100.0f,
							info.currentCacheSize / (1024ll * 1024ll),
							globalConfig.maxCacheSize / (1024ll * 1024ll),
							globalConfig.cachePath);
				}

				break;
			case 'm':
				{
					++arg;

					if(*arg == L'\0') {
						if(i != argc - 1) {
							arg = argv[++i];
						} else {
							wprintf(L"The -m option expects a numer in megabytes\n");

							return EXIT_FAILURE;
						}
					}
				
					UINT64 newCacheSize = (UINT64)wcstoull(arg, NULL, 0);

					if(newCacheSize >= 32) {
						globalConfig.maxCacheSize = newCacheSize * 1024ll * 1024ll; // TODO: Clean up the cache if necessary
						cache_config(&globalConfig, TRUE);
					} else {
						wprintf(L"Cache size must be at least 32 megabytes\n");

						return EXIT_FAILURE;
					}
				}

				break;
			case 'p':
				{
					++arg;

					if(*arg == L'\0') {
						if(i != argc - 1) {
							arg = argv[++i];
						} else {
							wprintf(L"The -p option expects a path as an argument\n");

							return EXIT_FAILURE;
						}
					}

					WCHAR buffer[MAX_PATH];

					while(iswspace(*arg))
						++arg;

					GetFullPathNameW(arg, MAX_PATH, buffer, NULL);

					arg = buffer + lstrlenW(buffer);

					while(arg != buffer && (*(arg - 1) == L'\\' || *(arg - 1) == L'/')) { // Removing trailing path separators
						--arg;
						*arg = L'\0';
					}

					lstrcpyW(globalConfig.cachePath, buffer);
					cache_config(&globalConfig, TRUE);
				}

				break;
			default:
				wprintf(L"Unknown option '%s'\n", argv[i]);

				return EXIT_FAILURE;
			}
		}
	} else {
		return lelcache_main(argc, argv);
	}

	return EXIT_SUCCESS;
}