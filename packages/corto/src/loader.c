/*
 * corto_loader.c
 *
 *  Created on: Aug 30, 2012
 *      Author: sander
 */

#include "corto/corto.h"

#ifdef CORTO_LOADER

void corto_onexit(void(*handler)(void*),void*userData);

static corto_ll fileHandlers = NULL;
static corto_ll loadedAdmin = NULL;
static corto_ll libraries = NULL;

extern corto_mutex_s corto_adminLock;

struct corto_loadedAdmin {
    corto_string name;
    corto_thread loading;
    corto_int16 result;
    corto_dl library;
    char *filename;
    char *base;
};

struct corto_fileHandler {
    corto_string ext;
    corto_loadAction load;
    void* userData;
};

static char* corto_convertToPath(corto_string lib, corto_id path) {
    char *ptr, *bptr, ch;
    /* Convert '::' in library name to '/' */
    ptr = lib;
    bptr = path;

    if (ptr[0] == '/') {
        ptr++;
    } else
    if ((ptr[0] == ':') && (ptr[1] == ':')) {
        ptr += 2;
    }

    while ((ch = *ptr)) {
        if (ch == ':') {
            if (ptr[1] == ':') {
                ch = '/';
                ptr++;
            }
        }
        *bptr = ch;

        ptr++;
        bptr++;
    }
    *bptr = '\0';

    return path;
}

/* Lookup loaded library by name */
static struct corto_loadedAdmin* corto_loadedAdminFind(corto_string name) {
    if (loadedAdmin) {
        corto_iter iter = corto_llIter(loadedAdmin);
        struct corto_loadedAdmin *lib;
        corto_id libPath, adminPath;

        corto_convertToPath(name, libPath);

        while (corto_iterHasNext(&iter)) {
            lib = corto_iterNext(&iter);
            corto_convertToPath(lib->name, adminPath);
            if (!strcmp(adminPath, libPath)) {
                return lib;
            }
        }
    }

    return NULL;
}

/* Add file */
static struct corto_loadedAdmin* corto_loadedAdminAdd(corto_string library) {
    struct corto_loadedAdmin *lib = corto_calloc(sizeof(struct corto_loadedAdmin));
    lib->name = corto_strdup(library);
    lib->loading = corto_threadSelf();
    if (!loadedAdmin) {
        loadedAdmin = corto_llNew();
    }
    corto_llInsert(loadedAdmin, lib);
    return lib;
}

/* Lookup file handler action */
static int corto_lookupExtWalk(struct corto_fileHandler* h, struct corto_fileHandler** data) {
    if (!strcmp(h->ext, (*data)->ext)) {
        *data = h;
        return 0;
    }
    return 1;
}

/* Lookup file handler */
static struct corto_fileHandler* corto_lookupExt(corto_string ext) {
    struct corto_fileHandler dummy, *dummy_p;

    dummy.ext = ext;
    dummy_p = &dummy;

    /* Walk handlers */
    if (fileHandlers) {
        corto_llWalk(fileHandlers, (corto_walkAction)corto_lookupExtWalk, &dummy_p);
    }

    if (dummy_p == &dummy) {
        dummy_p = NULL;
    }

    return dummy_p;
}

/* Register a filetype */
int corto_loaderRegister(corto_string ext, corto_loadAction handler, void* userData) {
    struct corto_fileHandler* h;

    /* Check if extension is already registered */
    corto_mutexLock(&corto_adminLock);
    if ((h = corto_lookupExt(ext))) {
        if (h->load != handler) {
            corto_error("corto_loaderRegister: extension '%s' is already registered with another loader.", ext);
            abort();
            goto error;
        }
    } else {
        h = corto_alloc(sizeof(struct corto_fileHandler));
        h->ext = ext;
        h->load = handler;
        h->userData = userData;

        if (!fileHandlers) {
            fileHandlers = corto_llNew();
        }

        corto_llAppend(fileHandlers, h);
    }
    corto_mutexUnlock(&corto_adminLock);

    return 0;
error:
    return -1;
}

static corto_string corto_replaceColons(corto_id buffer, corto_string package) {
    corto_char ch, *ptr, *bptr;
    corto_string fileName, start;

    fileName = buffer;

    ptr = package;
    bptr = buffer;
    start = bptr;
    while ((ch = *ptr)) {
        switch (ch) {
        case ':':
            ptr++;
        case '/':
            if (bptr != start) {
                *bptr = '/';
                bptr++;
            }
            fileName = bptr;
            break;
        default:
            *bptr = ch;
            bptr++;
            break;
        }
        ptr++;
    }
    *bptr = '\0';

    return fileName;
}

/* Convert package identifier to filename */
static corto_string corto_packageToFile(corto_string package) {
    corto_string path;
#ifdef CORTO_REDIS
    corto_asprintf(&path, "lib%s.so", package[0] == '/' ? package + 1 : package);
    char ch, *ptr;
    for (ptr = path; (ch = *ptr); ptr++) {
        if (ch == '/') *ptr = '_';
    }
#else
    corto_string fileName;
    int fileNameLength;
    path = malloc(strlen(package) * 2 + strlen("/lib.so") + 1);
    fileName = corto_replaceColons(path, package);
    fileNameLength = strlen(fileName);
    memcpy(fileName + fileNameLength, "/lib", 4);
    memcpy(fileName + fileNameLength + 4, fileName, fileNameLength);
    memcpy(fileName + fileNameLength * 2 + 4, ".so\0", 4);
#endif
    return path;
}

corto_dl corto_loadValidLibrary(corto_string fileName, corto_string *build_out) {
    corto_dl result = NULL;
    corto_string ___ (*build)(void);
    corto_string ___ (*library)(void);

    if (build_out) {
        *build_out = NULL;
    }

    if (!(result = corto_dlOpen(fileName))) {
        corto_seterr("%s", corto_dlError());
        goto error;
    }

    /* Lookup build function */
    build = (corto_string ___ (*)(void))corto_dlProc(result, "corto_getBuild");
    library = (corto_string ___ (*)(void))corto_dlProc(result, "corto_getLibrary");

    /* Validate version */
    if (build && strcmp(build(), corto_getBuild())) {
        corto_seterr(
          "corto: library '%s' links with conflicting corto library\n"
          "  links with: '%s' (%s)\n"
          "  current:    '%s' (%s)\n",
          fileName, library ? library() : "???", build(), corto_getLibrary(), corto_getBuild());
        /* Library is linked with different Corto version */
        if (build_out) {
            *build_out = corto_strdup(build());
        }
        goto error;
    } else if (build) {
        corto_debug(
          "loader: '%s' links with correct corto library\n  build:   '%s'\n  library: '%s'\n",
          fileName, build(), library());
    } else {
        corto_trace("loader: found '%s' which doesn't link with corto", fileName);
    }

    /* If no build function is available, the library is not linked with
     * Corto, and probably represents a --nocorto package */

    return result;
error:
    if (result) corto_dlClose(result);
    return NULL;
}

static corto_bool corto_checkLibrary(corto_string fileName, corto_string *build_out, corto_dl *dl_out) {
    corto_dl dl = corto_loadValidLibrary(fileName, build_out);
    corto_bool result = FALSE;
    if (dl) {
        result = TRUE;
        if (!dl_out) {
            corto_dlClose(dl);
        } else {
            if (*dl_out) {
                corto_dlClose(*dl_out);
            }
            *dl_out = dl;
        }
    }
    return result;
}

/* Load from a dynamic library */
int corto_loadFromDl(corto_dl dl, char *fileName, int argc, char *argv[]) {
    int (*proc)(int argc, char* argv[]);

    corto_debug("loader: invoke cortomain of '%s' with %d arguments", fileName, argc);

    /* Lookup main function */
    proc = (int(*)(int,char*[]))corto_dlProc(dl, "cortomain");
    if (proc) {
        /* Call main */
        if (proc(argc, argv)) {
            if (!corto_lasterr()) {
                corto_seterr("cortomain failed");
            }
            goto error;
        }
    } else {
        corto_string ___ (*build)(void);

        /* Lookup build function */
        build = (corto_string ___ (*)(void))corto_dlProc(dl, "corto_getBuild");
        if (build) {
            corto_seterr("library '%s' linked with corto but does not have a cortomain",
                fileName);
            goto error;
        }
    }

    /* Add library to libraries list */
    corto_mutexLock (&corto_adminLock);
    if (!libraries) {
        libraries = corto_llNew();
    }

    corto_llInsert(libraries, dl);
    corto_mutexUnlock (&corto_adminLock);

    corto_debug("loader: loaded '%s'", fileName);

    return 0;
error:
    return -1;
}

/*
 * Load a Corto library
 * Receives the absolute path to the lib<name>.so file.
 */
static int corto_loadLibrary(corto_string fileName, corto_bool validated, int argc, char* argv[]) {
    corto_dl dl = NULL;
    corto_string build = NULL;

    if (!validated) {
        dl = corto_loadValidLibrary(fileName, &build);
    } else {
        dl = corto_dlOpen(fileName);
    }

    if (!dl) {
        if (build) {
            corto_seterr("%s: uses a different corto build (%s)", fileName, build);
        } else {
            corto_seterr("%s: %s", corto_dlError());
        }
        goto error;
    }

    if (corto_loadFromDl(dl, fileName, argc, argv)) {
        goto error;
    }

    return 0;
error:
    if (dl) corto_dlClose(dl);
    return -1;
}

void (*corto_loaderResolveProc(corto_string procName))(void) {
    void (*result)(void) = NULL;

    /* Search libraries for specified symbol */
    corto_mutexLock (&corto_adminLock);
    corto_iter iter = corto_llIter(libraries);
    while (!result && corto_iterHasNext(&iter)) {
        corto_dl dl = corto_iterNext(&iter);
        result = (void(*)(void))corto_dlProc(dl, procName);
    }
    corto_mutexUnlock (&corto_adminLock);

    return result;
}

/*
 * An adapter on top of corto_loadLibrary to fit the corto_loadAction signature.
 */
int corto_loadLibraryAction(corto_string file, int argc, char* argv[], void *data) {
    CORTO_UNUSED(data);
    return corto_loadLibrary(file, FALSE, argc, argv);
}

/* Load a package */
int corto_loadIntern(corto_string str, int argc, char* argv[], corto_bool try, corto_bool ignoreRecursive) {
    corto_char ext[16];
    struct corto_fileHandler* h;
    int result = -1;
    struct corto_loadedAdmin *lib = NULL;

    corto_mutexLock(&corto_adminLock);
    lib = corto_loadedAdminFind(str);
    if (lib) {
        if (lib->loading == corto_threadSelf()) {
            if (!ignoreRecursive) {
                corto_error("illegal recursive load of file '%s' from:", lib->name);
                corto_iter iter = corto_llIter(loadedAdmin);
                while (corto_iterHasNext(&iter)) {
                    struct corto_loadedAdmin *lib = corto_iterNext(&iter);
                    if (lib->loading) {
                        fprintf(stderr, "   %s\n", lib->name);
                    }
                }
                corto_backtrace(stderr);
                abort();
            } else {
                corto_mutexUnlock(&corto_adminLock);
                goto loaded;
            }
        } else {
            result = lib->result;
            corto_mutexUnlock(&corto_adminLock);

            /* Other thread is loading library. Wait until finished */
            while (lib->loading) {
                corto_sleep(1, 0);
            }

            goto loaded;
        }
    }
    corto_mutexUnlock(&corto_adminLock);

    /* Get extension from filename */
    if (!corto_fileExtension(str, ext)) {
        goto error;
    }

    /* Lookup extension */
    corto_mutexLock(&corto_adminLock);
    h = corto_lookupExt(ext);

    if (!h) {
        corto_id extPackage;
        sprintf(extPackage, "corto/ext/%s", ext);
        corto_mutexUnlock(&corto_adminLock);
        if (corto_load(extPackage, 0, NULL)) {
            if (!try) {
                corto_seterr(
                    "unable to load file '%s' with extension '%s': %s",
                    str,
                    ext,
                    corto_lasterr());
                goto error;
            }
            result = -1;            
        }
        corto_mutexLock(&corto_adminLock);
        h = corto_lookupExt(ext);
    }

    /* Load file */
    lib = corto_loadedAdminAdd(str);
    corto_mutexUnlock(&corto_adminLock);
    result = h->load(str, argc, argv, h->userData);
    corto_mutexLock(&corto_adminLock);
    lib->loading = 0;
    lib->result = result;

    corto_mutexUnlock(&corto_adminLock);

    if (!result) {
        corto_trace("loader: '%s' loaded", str[0] == '/' ? &str[1] : str);
    }

    return result;
error:
    return -1;
loaded:
    return result;
}

/* Load a package */
int corto_load(corto_string str, int argc, char* argv[]) {
    return corto_loadIntern(str, argc, argv, FALSE, FALSE);
}

/* Try loading a package */
int corto_loadTry(corto_string str, int argc, char* argv[]) {
    return corto_loadIntern(str, argc, argv, TRUE, FALSE);
}

#ifndef CORTO_REDIS
static time_t corto_getModified(corto_string file) {
    struct stat attr;

    if (stat(file, &attr) < 0) {
        printf("failed to stat '%s' (%s)\n", file, strerror(errno));
    }

    return attr.st_mtime;
}

static corto_string corto_locatePackageIntern(
    corto_string lib,
    corto_string *base,
    corto_dl *dl_out,
    corto_bool isLibrary)
{
    corto_string targetPath = NULL, homePath = NULL, usrPath = NULL;
    corto_string result = NULL;
    corto_string targetBuild = NULL, homeBuild = NULL, usrBuild = NULL;
    corto_string targetErr = NULL, homeErr = NULL, usrErr = NULL;
    corto_string details = NULL;
    corto_bool fileError = FALSE;
    corto_dl dl = NULL;
    time_t t = 0;

    /* Reset error */
    corto_seterr(NULL);

    /* Look for local packages first */
    targetPath = corto_envparse("$CORTO_TARGET/lib/corto/%s.%s/%s",
        CORTO_VERSION_MAJOR, CORTO_VERSION_MINOR, lib);
    if (!targetPath) {
        goto error;
    }
    if (corto_fileTest(targetPath)) {
        corto_debug("loader: locate '%s': found '%s'", lib, targetPath);
        if (!isLibrary || corto_checkLibrary(targetPath, &targetBuild, &dl)) {
            t = corto_getModified(targetPath);
            result = targetPath;
            if (base) {
                *base = corto_envparse("$CORTO_TARGET/%%s/corto/%s.%s",
                    CORTO_VERSION_MAJOR, CORTO_VERSION_MINOR);
            }
        } else {
            if (corto_lasterr() != NULL) {
                targetErr = corto_strdup(corto_lasterr());
                corto_seterr(NULL);
            }
        }
    } else {
        if (corto_lasterr()) {
            targetErr = corto_strdup(corto_lasterr());
            fileError = TRUE;
        } else {
            corto_debug("loader: locate '%s': '%s' not found", lib, targetPath);
        }
    }

    /* Look for packages in CORTO_HOME */
    if (strcmp(corto_getenv("CORTO_HOME"), corto_getenv("CORTO_TARGET"))) {
        corto_string homePath = corto_envparse("$CORTO_HOME/lib/corto/%s.%s/%s",
            CORTO_VERSION_MAJOR, CORTO_VERSION_MINOR, lib);
        if (!homePath) {
            goto error;
        }
        if (corto_fileTest(homePath)) {
            time_t myT = corto_getModified(homePath);
            corto_debug("loader: locate '%s': found '%s'", lib, homePath);
            if ((myT >= t) || !result) {
                if (!isLibrary || corto_checkLibrary(homePath, &homeBuild, &dl)) {
                    t = myT;
                    result = homePath;
                    if (base) {
                        *base = corto_envparse("$CORTO_HOME/%%s/corto/%s.%s",
                            CORTO_VERSION_MAJOR, CORTO_VERSION_MINOR);
                    }
                } else {
                    if (corto_lasterr() != NULL) {
                        homeErr = corto_strdup(corto_lasterr());
                        corto_seterr(NULL);
                    }
                }
            } else if (!result) {
                corto_debug("loader: discarding '%s' because '%s' is newer", homePath, result);
            }
        } else {
            if (corto_lasterr()) {
                homeErr = corto_strdup(corto_lasterr());
                fileError = TRUE;
            } else {
                corto_debug("loader: locate '%s': '%s' not found", lib, homePath);
            }
        }
    } else {
        corto_debug("loader: locate '%s': '%s' already searched ($CORTO_HOME == $CORTO_TARGET)",
            lib, corto_getenv("CORTO_HOME"));
    }

    /* Look for global packages */
    if (strcmp("/usr/local", corto_getenv("CORTO_TARGET")) &&
        strcmp("/usr/local", corto_getenv("CORTO_HOME"))) {
        usrPath = corto_envparse("/usr/local/lib/corto/%s.%s/%s",
            CORTO_VERSION_MAJOR, CORTO_VERSION_MINOR, lib);
        if (!usrPath) {
            goto error;
        }
        if (corto_fileTest(usrPath)) {
            time_t myT = corto_getModified(usrPath);
            corto_debug("loader: locate '%s': found '%s'", usrPath, lib);
            if ((myT >= t) || !result) {
                if (!isLibrary || corto_checkLibrary(usrPath, &usrBuild, &dl)) {
                    t = myT;
                    result = usrPath;
                    if (base) {
                        *base = corto_envparse("/usr/local/%%s/corto/%s.%s",
                            CORTO_VERSION_MAJOR, CORTO_VERSION_MINOR);
                    }
                } else {
                    if (corto_lasterr() != NULL) {
                        usrErr = corto_strdup(corto_lasterr());
                        corto_seterr(NULL);
                    }
                }
            } else if (!result) {
                corto_debug("loader: discarding '%s' because '%s' is newer", usrPath, result);
            }
        } else {
            if (corto_lasterr()) {
                usrErr = corto_strdup(corto_lasterr());
                fileError = TRUE;
            } else {
                corto_debug("loader: locate '%s': '%s' not found", lib, usrPath);
            }
        }
    } else {
        corto_debug("loader: locate '%s': '/usr/local' already searched ($CORTO_HOME='%s' $CORTO_TARGET='%s')",
            lib, corto_getenv("CORTO_HOME"), corto_getenv("CORTO_TARGET"));
    }

    corto_string targetDetail = NULL, homeDetail = NULL, usrDetail = NULL;
    if (targetBuild) {
        corto_asprintf(
          &targetDetail,
          "\n- %s found but uses different corto build ('%s')",
          targetPath, targetBuild);
        corto_dealloc(targetBuild);
    }
    if (homeBuild) {
        corto_asprintf(
          &homeDetail,
          "\n- %s found but uses different corto build ('%s')",
          homePath, homeBuild);
        corto_dealloc(homeBuild);
    }
    if (usrBuild) {
        corto_asprintf(
          &usrDetail,
          "\n- %s found but uses different corto build ('%s')",
          usrPath, usrBuild);
        corto_dealloc(usrBuild);
    }

    if (targetDetail || homeDetail || usrDetail ||
        targetErr || homeErr || usrErr)
    {
        corto_asprintf(&details,
          "%s%s%s%s%s%s%s%s%s",
          targetDetail ? targetDetail : "",
          targetErr ? "\n- " : "",
          targetErr ? targetErr : "",
          homeDetail ? homeDetail : "",
          homeErr ? "\n- " : "",
          homeErr ? homeErr : "",
          usrDetail ? usrDetail : "",
          usrErr ? "\n- " : "",
          usrErr ? usrErr : "");
        if (targetDetail) corto_dealloc(targetDetail);
        if (homeDetail) corto_dealloc(homeDetail);
        if (usrDetail) corto_dealloc(usrDetail);
    }

    if (targetPath && (targetPath != result)) corto_dealloc(targetPath);
    if (homePath && (homePath != result)) corto_dealloc(homePath);
    if (usrPath && (usrPath != result)) corto_dealloc(usrPath);

    /* If there is a problem with one of the environments, don't load package */
    if (fileError) {
        if (result) {
            corto_dealloc(result);
            result = NULL;
        }
    }

    if (!result) {
        if (details) {
            if (fileError) {
                corto_seterr(details);
            }
        } else {
            corto_setinfo("library '%s' not found", lib);
        }
        if (dl) {
            corto_dlClose(dl);
            dl = NULL;
        }
    }

    if (dl_out) {
        *dl_out = dl;
    }

    if (details) {
        corto_setinfo(details);
        corto_dealloc(details);
    }

    return result;
error:
    return NULL;
}
#endif

corto_string corto_locateGetName(corto_string package, corto_loaderLocationKind kind) {
    corto_string result = corto_strdup(package);
    corto_string name;

    if (package[0] == '/') {
        name = corto_replaceColons(result, package + 1);
    } else if (package[0] == ':') {
        name = corto_replaceColons(result, package + 2);
    } else {
        name = corto_replaceColons(result, package);
    }

    if (kind == CORTO_LOCATION_NAME) {
        name = corto_strdup(name);
        corto_dealloc(result);
        result = name;
    }

    return result;
}

corto_string corto_locate(corto_string package, corto_dl *dl_out, corto_loaderLocationKind kind) {
    corto_string relativePath = NULL;
    corto_string result = NULL;

#ifndef CORTO_REDIS
    corto_dl dl = NULL;
    corto_string base = NULL;
    struct corto_loadedAdmin *loaded = NULL;

    /* If package has been loaded already, don't resolve it again */
    loaded = corto_loadedAdminFind(package);
    if (loaded) {
        result = loaded->filename;
        base = loaded->base;
    }    
#endif

    if (!result) {
        relativePath = corto_packageToFile(package);
        if (!relativePath) {
            goto error;
        }
    }

#ifdef CORTO_REDIS
    if (!corto_checkLibrary(relativePath, NULL, dl_out)) {
        goto error;
    }

    result = relativePath;
    switch(kind) {
    case CORTO_LOCATION_ENV:
    case CORTO_LOCATION_LIBPATH:
    case CORTO_LOCATION_INCLUDE:
        corto_dealloc(result);
        result = corto_strdup("");
        break;
    case CORTO_LOCATION_LIB:
        /* Result is already pointing to the lib */
        break;
    case CORTO_LOCATION_NAME:
    case CORTO_LOCATION_FULLNAME: {
        corto_dealloc(result);
        result = corto_locateGetName(package, kind);
        break;
    }
    }
#else
    if (!loaded || (!result && loaded->loading)) {
        result = corto_locatePackageIntern(relativePath, &base, &dl, TRUE);
        if (!result && (kind == CORTO_LOCATION_ENV)) {
            corto_lasterr();
            result = corto_locatePackageIntern(package, &base, &dl, FALSE);
        }
    }
    if (relativePath) corto_dealloc(relativePath);

    if (result) {
        corto_bool cleanupBase = FALSE;
        if (!loaded) {
            loaded = corto_loadedAdminAdd(package);
        }

        if (!loaded->filename) {
            corto_mutexLock(&corto_adminLock);
            corto_setstr(&loaded->filename, result);
            corto_setstr(&loaded->base, base);
            if (dl_out) {
                loaded->library = dl;
            }
            corto_mutexUnlock(&corto_adminLock);
            cleanupBase = TRUE;
        }

        switch(kind) {
        case CORTO_LOCATION_ENV:
            /* Quick & dirty trick to strip everything but the env */
            corto_asprintf(&result, base, "@");
            *(strchr(result, '@') - 1) = '\0'; /* Also strip the '/' */
            break;
        case CORTO_LOCATION_LIB:
            /* Result is already pointing to the lib */
            break;
        case CORTO_LOCATION_LIBPATH: {
            corto_string lib;
            corto_asprintf(&lib, base, "lib");
            corto_asprintf(&result, "%s/%s", lib, package);
            break;
        }
        case CORTO_LOCATION_INCLUDE: {
            corto_string include;
            corto_asprintf(&include, base, "include");
            corto_asprintf(&result, "%s/%s", include, package);
            corto_dealloc(include);
            break;
        }
        case CORTO_LOCATION_NAME:
        case CORTO_LOCATION_FULLNAME: {
            result = corto_locateGetName(package, kind);
            break;
        }
        }

        if (cleanupBase) {
            corto_dealloc(base);
        }
    }

    if (dl_out) {
        *dl_out = loaded->library;
    } else if (dl) {
        corto_dlClose(dl);
    }
#endif

    return result;
error:
    return NULL;
}

corto_ll corto_loadGetDependencies(corto_string file) {
    corto_ll result = NULL;

    if (corto_fileTest(file)) {
        corto_id name;
        result = corto_llNew();
        corto_file f = corto_fileRead(file);
        char *dependency;
        while ((dependency = corto_fileReadLine(f, name, sizeof(name)))) {
            corto_llAppend(result, corto_strdup(dependency));
        }
        corto_fileClose(f);
    }

    return result;
}

static void corto_loadFreeDependencies(corto_ll dependencies) {
    if (dependencies) {
        corto_iter iter = corto_llIter(dependencies);
        while (corto_iterHasNext(&iter)) {
            corto_dealloc(corto_iterNext(&iter));
        }
        corto_dealloc(dependencies);
    }
}

static corto_bool corto_loadRequiresDependency(corto_ll dependencies, corto_string query) {
    corto_bool result = FALSE;

    if (dependencies) {
        corto_iter iter = corto_llIter(dependencies);
        while (!result && corto_iterHasNext(&iter)) {
            corto_string package = corto_iterNext(&iter);
            if (!strcmp(package, query)) {
                result = TRUE;
            }
        }
    }

    return result;
}

corto_ll corto_loadGetPackages(void) {
    return corto_loadGetDependencies(".corto/packages.txt");
}

void corto_loadFreePackages(corto_ll packages) {
    corto_loadFreeDependencies(packages);
}

corto_bool corto_loadRequiresPackage(corto_string package) {
    corto_ll packages = corto_loadGetPackages();
    corto_bool result = corto_loadRequiresDependency(packages, package);
    corto_loadFreePackages(packages);
    return result;
}

int corto_loadPackages(void) {
    corto_ll packages = corto_loadGetPackages();
    if (packages) {
        corto_iter iter = corto_llIter(packages);
        while (corto_iterHasNext(&iter)) {
            corto_load(corto_iterNext(&iter), 0, NULL);
        }
        corto_loadFreePackages(packages);
    }
    return 0;
}

/* Load file with unspecified extension */
int corto_fileLoader(corto_string package, int argc, char* argv[], void* udata) {
    CORTO_UNUSED(udata);
    corto_string fileName;
    int result;
    corto_dl dl = NULL;

    fileName = corto_locate(package, &dl, CORTO_LOCATION_LIB);
    if (!fileName) {
        corto_seterr(corto_lastinfo());
        return -1;
    }

    corto_assert(dl != NULL, "package located but dl_out is NULL");

    result = corto_loadFromDl(dl, fileName, argc, argv);
    corto_dealloc(fileName);

    return result;
}

void corto_loaderOnExit(void* udata) {
    struct corto_fileHandler* h;
    corto_dl dl;
    corto_iter iter;

    CORTO_UNUSED(udata);

    /* Free loaded administration (always happens from mainthread) */

    if (loadedAdmin) {
        iter = corto_llIter(loadedAdmin);
         while(corto_iterHasNext(&iter)) {
             struct corto_loadedAdmin *loaded = corto_iterNext(&iter);
             corto_dealloc(loaded->name);
             corto_dealloc(loaded);
         }
         corto_llFree(loadedAdmin);
    }

    /* Free handlers */
    while ((h = corto_llTakeFirst(fileHandlers))) {
        corto_dealloc(h);
    }
    corto_llFree(fileHandlers);

    /* Free libraries */
    if (libraries) {
        while ((dl = corto_llTakeFirst(libraries))) {
            corto_dlClose(dl);
        }
        corto_llFree(libraries);
    }
}

#else
int corto_load(corto_string str) {
    CORTO_UNUSED(str);
    corto_error("corto build doesn't include loader");
    return -1;
}
#endif
