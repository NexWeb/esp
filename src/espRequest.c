/*
    espRequest.c -- ESP Request handler

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/

#include    "esp.h"

/************************************* Local **********************************/
/*
    Singleton ESP control structure
 */
static Esp *esp;

/************************************ Forward *********************************/

static int cloneDatabase(HttpConn *conn);
static void closeEsp(HttpQueue *q);
static EspRoute *createEspRoute(HttpRoute *route);
static void manageEsp(Esp *esp, int flags);
static void manageReq(EspReq *req, int flags);
static int runAction(HttpConn *conn);
static int unloadEsp(MprModule *mp);

#if !ME_STATIC
static char *getModuleEntry(EspRoute *eroute, cchar *kind, cchar *source, cchar *cacheName);
static bool layoutIsStale(EspRoute *eroute, cchar *source, cchar *module);
static bool loadApp(HttpRoute *route, MprDispatcher *dispatcher);
#endif

/************************************* Code ***********************************/
/*
    Open an instance of the ESP for a new request
 */
static int openEsp(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    EspReq      *req;

    conn = q->conn;
    rx = conn->rx;

    if ((req = mprAllocObj(EspReq, manageReq)) == 0) {
        httpMemoryError(conn);
        return MPR_ERR_MEMORY;
    }

    /*
        If unloading a module, this lock will cause a wait here while ESP applications are reloaded.
        Do not use atomic APIs here
     */
    lock(esp);
    esp->inUse++;
    unlock(esp);

    /*
        Find the ESP route configuration. Search up the route parent chain.
     */
    for (eroute = 0, route = rx->route; route; route = route->parent) {
        if (route->eroute) {
            eroute = route->eroute;
            break;
        }
    }
    if (!route) {
        route = rx->route;
        eroute = createEspRoute(route);
    }
    if (!eroute) {
        httpError(conn, 0, "Cannot find a suitable ESP route");
        closeEsp(q);
        return MPR_ERR_CANT_OPEN;
    }
    conn->reqData = req;
    req->esp = esp;
    req->route = route;
    req->autoFinalize = 1;

    /*
        If a cookie is not explicitly set, use the application name for the session cookie so that
        cookies are unique per esp application.
     */
    if (!route->cookie && eroute->appName && *eroute->appName) {
        httpSetRouteCookie(route, eroute->appName);
    }
    return 0;
}


static void closeEsp(HttpQueue *q)
{
    lock(esp);
    esp->inUse--;
    assert(esp->inUse >= 0);
    unlock(esp);
}


#if !ME_STATIC
/*
    This is called when unloading a view or controller module
    All of ESP must be quiesced.
 */
static bool espUnloadModule(cchar *module, MprTicks timeout)
{
    MprModule   *mp;
    MprTicks    mark;

    if ((mp = mprLookupModule(module)) != 0) {
        mark = mprGetTicks();
        esp->reloading = 1;
        do {
            lock(esp);
            /* Own request will count as 1 */
            if (esp->inUse <= 1) {
                if (mprUnloadModule(mp) < 0) {
                    mprLog("error esp", 0, "Cannot unload module %s", mp->name);
                    unlock(esp);
                }
                esp->reloading = 0;
                unlock(esp);
                return 1;
            }
            unlock(esp);
            mprSleep(10);

        } while (mprGetRemainingTicks(mark, timeout) > 0);
        esp->reloading = 0;
    }
    return 0;
}
#endif


PUBLIC void espClearFlash(HttpConn *conn)
{
    EspReq      *req;

    req = conn->reqData;
    req->flash = 0;
}


static void setupFlash(HttpConn *conn)
{
    EspReq      *req;

    req = conn->reqData;
    if (httpGetSession(conn, 0)) {
        req->flash = httpGetSessionObj(conn, ESP_FLASH_VAR);
        req->lastFlash = 0;
        if (req->flash) {
            httpRemoveSessionVar(conn, ESP_FLASH_VAR);
            req->lastFlash = mprCloneHash(req->flash);
        }
    }
}


static void pruneFlash(HttpConn *conn)
{
    EspReq  *req;
    MprKey  *kp, *lp;

    req = conn->reqData;
    if (req->flash && req->lastFlash) {
        for (ITERATE_KEYS(req->flash, kp)) {
            for (ITERATE_KEYS(req->lastFlash, lp)) {
                if (smatch(kp->key, lp->key)) {
                    mprRemoveKey(req->flash, kp->key);
                }
            }
        }
    }
}


static void finalizeFlash(HttpConn *conn)
{
    EspReq  *req;

    req = conn->reqData;
    if (req->flash && mprGetHashLength(req->flash) > 0) {
        /*
            If the session does not exist, this will create one. However, must not have
            emitted the headers, otherwise cannot inform the client of the session cookie.
        */
        httpSetSessionObj(conn, ESP_FLASH_VAR, req->flash);
    }
}


/*
    Start the request. At this stage, body data may not have been fully received unless
    the request is a form (POST method and content type is application/x-www-form-urlencoded).
    Forms are a special case and delay invoking the start callback until all body data is received.
    WARNING: GC yield
 */
static void startEsp(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    EspReq      *req;

    conn = q->conn;
    rx = conn->rx;
    route = rx->route;
    eroute = route->eroute;
    req = conn->reqData;

    if (req) {
        mprSetThreadData(req->esp->local, conn);
        httpAuthenticate(conn);
        setupFlash(conn);
        /*
            See if the esp configuration or app needs to be reloaded.
         */
        if (route->update && eroute->appName && espLoadConfig(route) < 0) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot load esp config for %s", eroute->appName);
            return;
        }
#if !ME_STATIC
        /* WARNING: GC yield */
        if (!loadApp(route, conn->dispatcher)) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot load esp module for %s", eroute->appName);
            return;
        }
#endif
        /* WARNING: GC yield */
        if (!runAction(conn)) {
            pruneFlash(conn);
        } else {
            if (req->autoFinalize) {
                if (!conn->tx->responded) {
                    /* WARNING: GC yield */
                    espRenderDocument(conn, rx->target);
                }
                if (req->autoFinalize) {
                    espFinalize(conn);
                }
            }
            pruneFlash(conn);
        }
        finalizeFlash(conn);
        mprSetThreadData(req->esp->local, NULL);
    }
}


static int runAction(HttpConn *conn)
{
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    EspReq      *req;
    EspAction   action;
    cchar       *controllers;
    char        *controller;

    rx = conn->rx;
    req = conn->reqData;
    route = rx->route;
    eroute = route->eroute;
    controller = 0;
    assert(eroute);

    if (eroute->edi && eroute->edi->flags & EDI_PRIVATE) {
        cloneDatabase(conn);
    } else {
        req->edi = eroute->edi;
    }
    if (route->sourceName == 0 || *route->sourceName == '\0') {
        if (eroute->commonController) {
            (eroute->commonController)(conn);
        }
        return 1;
    }
    if ((controllers = httpGetDir(route, "CONTROLLERS")) == 0) {
        controllers = ".";
    }
    controllers = mprJoinPath(route->home, controllers);

#if !ME_STATIC
    if (!eroute->combine && (route->update || !mprLookupKey(eroute->actions, rx->target))) {
        cchar *errMsg;
        controller = schr(route->sourceName, '$') ? stemplateJson(route->sourceName, rx->params) : route->sourceName;
        controller = controllers ? mprJoinPath(controllers, controller) : mprJoinPath(route->home, controller);
        if (mprPathExists(controller, R_OK)) {
            if (espLoadModule(route, conn->dispatcher, "controller", controller, &errMsg) < 0) {
                httpError(conn, HTTP_CODE_NOT_FOUND, "%s", errMsg);
                return 0;
            }
            controllers = mprGetPathDir(controller);
        }
    }
#endif /* !ME_STATIC */
    
    assert(eroute->top);
    action = mprLookupKey(eroute->top->actions, rx->target);

    if (route->flags & HTTP_ROUTE_XSRF && !(rx->flags & HTTP_GET)) {
        if (!httpCheckSecurityToken(conn)) {
            httpSetStatus(conn, HTTP_CODE_UNAUTHORIZED);
            if (smatch(route->responseFormat, "json")) {
                httpTrace(conn, "esp.xsrf.error", "error", 0);
                espRenderString(conn,
                    "{\"retry\": true, \"success\": 0, \"feedback\": {\"error\": \"Security token is stale. Please retry.\"}}");
                espFinalize(conn);
            } else {
                httpError(conn, HTTP_CODE_UNAUTHORIZED, "Security token is stale. Please reload page.");
            }
            return 0;
        }
    }
    if (action) {
        if (eroute->commonController) {
            (eroute->commonController)(conn);
        }
        if (!httpIsFinalized(conn)) {
            (action)(conn);
        }
    }
    return 1;
}


PUBLIC void espRenderDocument(HttpConn *conn, cchar *document)
{
    HttpTx      *tx;
    cchar       *path;

    tx = conn->tx;
    path = httpMapContent(conn, mprJoinPath(conn->rx->route->documents, document));
    if (!httpSetFilename(conn, path, 0)) {
        if (conn->rx->flags & (HTTP_GET | HTTP_HEAD)) {
            if (!httpSetFilename(conn, mprJoinPathExt(path, ".esp"), 0)) {
#if DEPRECATE || 1
                path = httpMapContent(conn, mprJoinPaths(conn->rx->route->documents, "app", document, NULL));
                if (!httpSetFilename(conn, path, 0)) {
                    httpSetFilename(conn, mprJoinPathExt(path, ".esp"), 0);
                }
#endif
            }
        }
    }
    if (tx->fileInfo.isDir) {
        httpHandleDirectory(conn);
        if (tx->finalized) {
            return;
        }
    }
    if (smatch(tx->ext, "esp")) {
        espRenderView(conn, tx->filename);
    } else {
        httpSetFileHandler(conn, NULL);
    }
}


PUBLIC void espRenderView(HttpConn *conn, cchar *view)
{
    HttpRx      *rx;
    HttpRoute   *route;
    EspRoute    *eroute;
    EspViewProc viewProc;
    cchar       *path;

    rx = conn->rx;
    route = rx->route;
    eroute = route->eroute;

    path = mprJoinPathExt(mprJoinPath(route->documents, view), ".esp");
    
#if DEPRECATE || 1
    if (!mprPathExists(path, R_OK)) {
        path = mprJoinPathExt(mprJoinPaths(route->documents, "app", view, NULL), ".esp");
    }
#endif
    path = mprGetPortablePath(path);

#if !ME_STATIC
    assert(eroute->top);
    if (!eroute->combine && (route->update || !mprLookupKey(eroute->top->views, path))) {
        cchar *errMsg;
        /* WARNING: GC yield */
        if (espLoadModule(route, conn->dispatcher, "view", path, &errMsg) < 0) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "%s", errMsg);
            return;
        }
    }
#endif
    if ((viewProc = mprLookupKey(eroute->views, path)) == 0) {
        httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot find view");
        return;
    }
    if (rx->route->flags & HTTP_ROUTE_XSRF) {
        /* Add a new unique security token */
        httpAddSecurityToken(conn, 1);
    }
    httpSetContentType(conn, "text/html");
    
    /* WARNING: GC yield */
    (viewProc)(conn);
}


/************************************ Support *********************************/
/*
    Create a per user session database clone. Used for demos so one users updates to not change anothers view of the database
 */
static void pruneDatabases(Esp *esp)
{
    MprKey      *kp;

    lock(esp);
    for (ITERATE_KEYS(esp->databases, kp)) {
        if (!httpLookupSessionID(kp->key)) {
            mprRemoveKey(esp->databases, kp->key);
            /* Restart scan */
            kp = 0;
        }
    }
    unlock(esp);
}


/*
    This clones a database to give a private view per user.
 */
static int cloneDatabase(HttpConn *conn)
{
    Esp         *esp;
    EspRoute    *eroute;
    EspReq      *req;
    cchar       *id;

    req = conn->reqData;
    eroute = conn->rx->route->eroute;
    assert(eroute->edi);
    assert(eroute->edi->flags & EDI_PRIVATE);

    esp = req->esp;
    if (!esp->databases) {
        lock(esp);
        if (!esp->databases) {
            esp->databases = mprCreateHash(0, 0);
            esp->databasesTimer = mprCreateTimerEvent(NULL, "esp-databases", 60 * 1000, pruneDatabases, esp, 0);
        }
        unlock(esp);
    }
    /*
        If the user is logging in or out, this will create a redundant session here.
     */
    httpGetSession(conn, 1);
    id = httpGetSessionID(conn);
    if ((req->edi = mprLookupKey(esp->databases, id)) == 0) {
        if ((req->edi = ediClone(eroute->edi)) == 0) {
            mprLog("error esp", 0, "Cannot clone database: %s", eroute->edi->path);
            return MPR_ERR_CANT_OPEN;
        }
        mprAddKey(esp->databases, id, req->edi);
    }
    return 0;
}


#if !ME_STATIC
static char *getModuleEntry(EspRoute *eroute, cchar *kind, cchar *source, cchar *cacheName)
{
    char    *cp, *entry;

    if (smatch(kind, "view")) {
        entry = sfmt("esp_%s", cacheName);

    } else if (smatch(kind, "app")) {
        if (eroute->combine) {
            entry = sfmt("esp_%s_%s_combine", kind, eroute->appName);
        } else {
            entry = sfmt("esp_%s_%s", kind, eroute->appName);
        }
    } else {
        /* Controller */
        if (eroute->appName) {
            entry = sfmt("esp_%s_%s_%s", kind, eroute->appName, mprTrimPathExt(mprGetPathBase(source)));
        } else {
            entry = sfmt("esp_%s_%s", kind, mprTrimPathExt(mprGetPathBase(source)));
        }
    }
    for (cp = entry; *cp; cp++) {
        if (!isalnum((uchar) *cp) && *cp != '_') {
            *cp = '_';
        }
    }
    return entry;
}


/*
    WARNING: GC yield
 */
PUBLIC int espLoadModule(HttpRoute *route, MprDispatcher *dispatcher, cchar *kind, cchar *source, cchar **errMsg)
{
    EspRoute    *eroute;
    MprModule   *mp;
    cchar       *appName, *cache, *cacheName, *canonical, *entry, *module;
    int         isView, recompile;

    eroute = route->eroute;
    *errMsg = "";

#if VXWORKS
    /*
        Trim the drive for VxWorks where simulated host drives only exist on the target
     */
    source = mprTrimPathDrive(source);
#endif
    canonical = mprGetPortablePath(mprGetRelPath(source, route->documents));
    appName = eroute->appName ? eroute->appName : route->host->name;
    if (eroute->combine) {
        cacheName = eroute->appName;
    } else {
        cacheName = mprGetMD5WithPrefix(sfmt("%s:%s", appName, canonical), -1, sjoin(kind, "_", NULL));
    }
    if ((cache = httpGetDir(route, "CACHE")) == 0) {
        cache = "cache";
    }
    module = mprNormalizePath(sfmt("%s/%s%s", mprJoinPath(route->home, cache), cacheName, ME_SHOBJ));
    isView = smatch(kind, "view");

    lock(esp);
    if (route->update) {
        if (!mprPathExists(source, R_OK)) {
            *errMsg = sfmt("Cannot find %s \"%s\" to load", kind, source);
            unlock(esp);
            return MPR_ERR_CANT_FIND;
        }
        if (espModuleIsStale(source, module, &recompile) || (isView && layoutIsStale(eroute, source, module))) {
            if (recompile) {
                mprHoldBlocks(source, module, cacheName, NULL);
                if (!espCompile(route, dispatcher, source, module, cacheName, isView, (char**) errMsg)) {
                    mprReleaseBlocks(source, module, cacheName, NULL);
                    unlock(esp);
                    return MPR_ERR_CANT_WRITE;
                }
                mprReleaseBlocks(source, module, cacheName, NULL);
            }
        }
    }
    if (mprLookupModule(source) == 0) {
        entry = getModuleEntry(eroute, kind, source, cacheName);
        if ((mp = mprCreateModule(source, module, entry, route)) == 0) {
            *errMsg = "Memory allocation error loading module";
            unlock(esp);
            return MPR_ERR_MEMORY;
        }
        if (mprLoadModule(mp) < 0) {
            *errMsg = "Cannot load compiled esp module";
            unlock(esp);
            return MPR_ERR_CANT_READ;
        }
    }
    unlock(esp);
    return 0;
}


/*
    WARNING: GC yield
 */
static bool loadApp(HttpRoute *route, MprDispatcher *dispatcher)
{
    EspRoute    *eroute;
    cchar       *source, *errMsg;

    eroute = route->eroute;
    if (!eroute->appName) {
        return 1;
    }
    if (route->loaded && !route->update) {
        return 1;
    }
    if (eroute->combine) {
        source = mprJoinPath(httpGetDir(route, "CACHE"), sfmt("%s.c", eroute->appName));
    } else {
        source = mprJoinPath(httpGetDir(route, "SRC"), "app.c");
    }
    if (mprPathExists(source, R_OK)) {
        if (espLoadModule(route, dispatcher, "app", source, &errMsg) < 0) {
            mprLog("error esp", 0, "%s", errMsg);
            return 0;
        }
    }
    route->loaded = 1;
    return 1;
}


/*
    Test if a module has been updated (is stale).
    This will unload the module if it loaded but stale.
    Set recompile to true if the source is absent or more recent.
    Will return false if the source does not exist (important for testing layouts).
 */
PUBLIC bool espModuleIsStale(cchar *source, cchar *module, int *recompile)
{
    MprModule   *mp;
    MprPath     sinfo, minfo;

    *recompile = 0;
    mprGetPathInfo(module, &minfo);
    if (!minfo.valid) {
        if ((mp = mprLookupModule(source)) != 0) {
            if (!espUnloadModule(source, ME_ESP_RELOAD_TIMEOUT)) {
                mprLog("error esp", 0, "Cannot unload module %s. Connections still open. Continue using old version.",
                    source);
                return 0;
            }
        }
        *recompile = 1;
        mprLog("info esp", 4, "Source %s is newer than module %s, recompiling ...", source, module);
        return 1;
    }
    mprGetPathInfo(source, &sinfo);
    if (sinfo.valid && sinfo.mtime > minfo.mtime) {
        if ((mp = mprLookupModule(source)) != 0) {
            if (!espUnloadModule(source, ME_ESP_RELOAD_TIMEOUT)) {
                mprLog("warn esp", 4, "Cannot unload module %s. Connections still open. Continue using old version.",
                    source);
                return 0;
            }
        }
        *recompile = 1;
        mprLog("info esp", 4, "Source %s is newer than module %s, recompiling ...", source, module);
        return 1;
    }
    if ((mp = mprLookupModule(source)) != 0) {
        if (minfo.mtime > mp->modified) {
            /* Module file has been updated */
            if (!espUnloadModule(source, ME_ESP_RELOAD_TIMEOUT)) {
                mprLog("warn esp", 4, "Cannot unload module %s. Connections still open. Continue using old version.",
                    source);
                return 0;
            }
            mprLog("info esp", 4, "Module %s has been externally updated, reloading ...", module);
            return 1;
        }
    }
    /* Loaded module is current */
    return 0;
}


/*
    Check if the layout has changed. Returns false if the layout does not exist.
 */
static bool layoutIsStale(EspRoute *eroute, cchar *source, cchar *module)
{
    char    *data, *lpath, *quote;
    cchar   *layout, *layoutsDir;
    ssize   len;
    bool    stale;
    int     recompile;

    stale = 0;
    layoutsDir = httpGetDir(eroute->route, "LAYOUTS");
    if ((data = mprReadPathContents(source, &len)) != 0) {
        if ((lpath = scontains(data, "@ layout \"")) != 0) {
            lpath = strim(&lpath[10], " ", MPR_TRIM_BOTH);
            if ((quote = schr(lpath, '"')) != 0) {
                *quote = '\0';
            }
            layout = (layoutsDir && *lpath) ? mprJoinPath(layoutsDir, lpath) : 0;
        } else {
            layout = (layoutsDir) ? mprJoinPath(layoutsDir, "default.esp") : 0;
        }
        if (layout) {
            stale = espModuleIsStale(layout, module, &recompile);
            if (stale) {
                mprLog("info esp", 4, "esp layout %s is newer than module %s", layout, module);
            }
        }
    }
    return stale;
}
#else

PUBLIC bool espModuleIsStale(cchar *source, cchar *module, int *recompile)
{
    return 0;
}
#endif /* ME_STATIC */


/************************************ Esp Route *******************************/
/*
    Public so that esp.c can also call
 */
PUBLIC void espManageEspRoute(EspRoute *eroute, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(eroute->actions);
        mprMark(eroute->appName);
        mprMark(eroute->compile);
        mprMark(eroute->currentSession);
        mprMark(eroute->edi);
        mprMark(eroute->env);
        mprMark(eroute->link);
        mprMark(eroute->routeSet);
        mprMark(eroute->searchPath);
        mprMark(eroute->top);
        mprMark(eroute->views);
        mprMark(eroute->winsdk);
#if DEPRECATED || 1
        mprMark(eroute->combineScript);
        mprMark(eroute->combineSheet);
#endif
    }
}


PUBLIC EspRoute *espCreateRoute(HttpRoute *route)
{
    EspRoute    *eroute;

    if ((eroute = mprAllocObj(EspRoute, espManageEspRoute)) == 0) {
        return 0;
    }
    eroute->route = route;
    route->eroute = eroute;
#if ME_DEBUG
    eroute->compileMode = ESP_COMPILE_SYMBOLS;
#else
    eroute->compileMode = ESP_COMPILE_OPTIMIZED;
#endif
    if (route->parent) {
        eroute->top = ((EspRoute*)route->parent->eroute)->top;
    } else {
        eroute->top = eroute;
    }
    return eroute;
}


static EspRoute *createEspRoute(HttpRoute *route)
{
    EspRoute    *eroute;

    if (route->eroute) {
        eroute = route->eroute;
        return eroute;
    }
    return espCreateRoute(route);
}


static EspRoute *cloneEspRoute(HttpRoute *route, EspRoute *parent)
{
    EspRoute      *eroute;

    assert(parent);
    assert(route);

    if ((eroute = mprAllocObj(EspRoute, espManageEspRoute)) == 0) {
        return 0;
    }
    eroute->route = route;
    eroute->top = parent->top;
    eroute->searchPath = parent->searchPath;
    eroute->edi = parent->edi;
    eroute->commonController = parent->commonController;
    if (parent->compile) {
        eroute->compile = sclone(parent->compile);
    }
    if (parent->link) {
        eroute->link = sclone(parent->link);
    }
    if (parent->env) {
        eroute->env = mprCloneHash(parent->env);
    }
    eroute->appName = parent->appName;
    eroute->combine = parent->combine;
#if DEPRECATED || 1
    eroute->combineScript = parent->combineScript;
    eroute->combineSheet = parent->combineSheet;
#endif
    eroute->routeSet = parent->routeSet;
    route->eroute = eroute;
    return eroute;
}


/*
    Get an EspRoute. Allocate if required.
    It is expected that the caller will modify the EspRoute.
 */
PUBLIC EspRoute *espRoute(HttpRoute *route)
{
    HttpRoute   *rp;

    if (route->eroute && (!route->parent || route->parent->eroute != route->eroute)) {
        return route->eroute;
    }
    /*
        Lookup up the route chain for any configured EspRoutes
     */
    for (rp = route; rp; rp = rp->parent) {
        if (rp->eroute) {
            return cloneEspRoute(route, rp->eroute);
        }
    }
    return createEspRoute(route);
}


/*
    Manage all links for EspReq for the garbage collector
 */
static void manageReq(EspReq *req, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(req->commandLine);
        mprMark(req->flash);
        mprMark(req->lastFlash);
        mprMark(req->feedback);
        mprMark(req->route);
        mprMark(req->data);
        mprMark(req->edi);
    }
}


/*
    Manage all links for Esp for the garbage collector
 */
static void manageEsp(Esp *esp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(esp->databases);
        mprMark(esp->databasesTimer);
        mprMark(esp->ediService);
        mprMark(esp->internalOptions);
        mprMark(esp->local);
        mprMark(esp->mutex);
    }
}


/*********************************** Directives *******************************/

PUBLIC int espDefineApp(HttpRoute *route, cchar *name, cchar *prefix, cchar *home, cchar *documents, cchar *routeSet)
{
    EspRoute    *eroute;

    if ((eroute = espRoute(route)) == 0) {
        return MPR_ERR_MEMORY;
    }
    espSetDefaultDirs(route);
    if (home) {
        httpSetRouteHome(route, home);
    }
    if (documents) {
        httpSetRouteDocuments(route, documents);
    }
    eroute->top = eroute;
    if (name) {
        eroute->appName = sclone(name);
    }
    if (routeSet) {
        eroute->routeSet = sclone(routeSet);
    }
    if (prefix) {
        if (*prefix != '/') {
            mprLog("warn esp", 0, "Prefix name should start with a \"/\"");
            prefix = sjoin("/", prefix, NULL);
        }
        prefix = stemplate(prefix, route->vars);
        httpSetRoutePrefix(route, prefix);
        httpSetRoutePattern(route, sfmt("^%s.*$", prefix), 0);
    } else {
        httpSetRoutePattern(route, "^.*$", 0);
    }
    if (!route->cookie && eroute->appName && *eroute->appName) {
        httpSetRouteCookie(route, eroute->appName);
    }
    httpSetRouteXsrf(route, 1);
    httpAddRouteHandler(route, "espHandler", "");

    httpAddRouteIndex(route, "index.esp");
    httpAddRouteIndex(route, "index.html");

    httpSetRouteVar(route, "NAME", name);
    httpSetRouteVar(route, "TITLE", stitle(name));
    return 0;
}


static void ifConfigModified(HttpRoute *route, cchar *path, bool *modified)
{
    EspRoute    *eroute;
    MprPath     info;

    eroute = route->eroute;
    path = mprJoinPath(route->home, path);
    mprGetPathInfo(path, &info);
    if (info.mtime > eroute->loaded) {
        *modified = 1;
        eroute->loaded = info.mtime;
    }
}


PUBLIC int espLoadConfig(HttpRoute *route)
{
    cchar       *files[] = { "package.json", "esp.json", "expansive.json", 0 };
    EspRoute    *eroute;
    cchar       **file, *path;
    bool        modified;

    modified = 0;
    eroute = route->eroute;
    for (file = files; *file; file++) {
        ifConfigModified(route, *file, &modified);
    }
    if (modified) {
        lock(esp);
        httpInitConfig(route);
        for (file = files; *file; file++) {
            path = mprJoinPath(route->home, *file);
            if (mprPathExists(path, R_OK)) {
                if (httpLoadConfig(route, path) < 0) {
                    unlock(esp);
                    return MPR_ERR_CANT_LOAD;
                }
            }
        }
        path = mprJoinPath(mprGetAppDir(), "esp-compile.json");
        if (httpLoadConfig(route, path) < 0) {
            unlock(esp);
            return MPR_ERR_CANT_LOAD;
        }
        httpFinalizeConfig(route);
        unlock(esp);
    }
    return 0;
}


PUBLIC int espConfigureApp(HttpRoute *route) 
{
    EspRoute    *eroute;

    eroute = route->eroute;

    if (espLoadConfig(route) < 0) {
        return MPR_ERR_CANT_LOAD;
    }
    eroute->appName = espGetConfig(route, "name", eroute->appName);
    if (eroute->routeSet) {
        httpAddRouteSet(route, eroute->routeSet);
    }
    if (route->database && !eroute->edi) {
        if (espOpenDatabase(route, route->database) < 0) {
            mprLog("error esp", 0, "Cannot open database %s", route->database);
            return MPR_ERR_CANT_LOAD;
        }
    }
    return 0;
}


PUBLIC int espLoadApp(HttpRoute *route)
{
#if !ME_STATIC
    EspRoute    *eroute;

    eroute = route->eroute;
    if (!eroute->skipApps) {
        MprJson     *preload, *item;
        cchar       *errMsg, *source;
        char        *kind;
        int         i;

        /*
            Note: the config parser pauses GC, so this will never yield
         */
        if (!loadApp(route, NULL)) {
            return MPR_ERR_CANT_LOAD;
        }
        if (!eroute->combine && (preload = mprGetJsonObj(route->config, "esp.preload")) != 0) {
            for (ITERATE_JSON(preload, item, i)) {
                source = ssplit(sclone(item->value), ":", &kind);
                if (*kind == '\0') {
                    kind = "controller";
                }
                source = mprJoinPath(httpGetDir(route, "CONTROLLERS"), source);
                if (espLoadModule(route, NULL, kind, source, &errMsg) < 0) {
                    mprLog("error esp", 0, "Cannot preload esp module %s. %s", source, errMsg);
                    return MPR_ERR_CANT_LOAD;
                }
            }
        }
    }
#endif
    return 0;
}


PUBLIC int espOpenDatabase(HttpRoute *route, cchar *spec)
{
    EspRoute    *eroute;
    char        *provider, *path, *dir;
    int         flags;

    eroute = route->eroute;
    if (eroute->edi) {
        return 0;
    }
    flags = EDI_CREATE | EDI_AUTO_SAVE;
    if (smatch(spec, "default")) {
#if ME_COM_SQLITE
        spec = sfmt("sdb://%s.sdb", eroute->appName);
#elif ME_COM_MDB
        spec = sfmt("mdb://%s.mdb", eroute->appName);
#endif
    }
    provider = ssplit(sclone(spec), "://", &path);
    if (*provider == '\0' || *path == '\0') {
        return MPR_ERR_BAD_ARGS;
    }
    path = mprJoinPaths(route->home, httpGetDir(route, "DB"), path, NULL);
    dir = mprGetPathDir(path);
    if (!mprPathExists(dir, X_OK)) {
        mprMakeDir(dir, 0755, -1, -1, 1);
    }
    if ((eroute->edi = ediOpen(mprGetRelPath(path, NULL), provider, flags)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    route->database = sclone(spec);
    return 0;
}


PUBLIC void espSetDefaultDirs(HttpRoute *route)
{
    httpSetDir(route, "CACHE", 0);
    httpSetDir(route, "CONTROLLERS", 0);
    httpSetDir(route, "DB", 0);
    httpSetDir(route, "DOCUMENTS", route->documents);
    httpSetDir(route, "HOME", route->home);
    httpSetDir(route, "LAYOUTS", 0);
    httpSetDir(route, "LIB", "lib");
    httpSetDir(route, "PAKS", 0);
    httpSetDir(route, "PARTIALS", 0);
    httpSetDir(route, "SOURCE", 0);
    httpSetDir(route, "SRC", 0);
    httpSetDir(route, "UPLOAD", "/tmp");
#if UNUSED
    httpSetDir(route, "TOP", mprGetCurrentPath());
#endif
}


/*
    Initialize and load a statically linked ESP module
 */
PUBLIC int espStaticInitialize(EspModuleEntry entry, cchar *appName, cchar *routeName)
{
    HttpRoute   *route;

    if ((route = httpLookupRoute(NULL, routeName)) == 0) {
        mprLog("error esp", 0, "Cannot find route %s", routeName);
        return MPR_ERR_CANT_ACCESS;
    }
    return (entry)(route, NULL);
}


PUBLIC int espBindProc(HttpRoute *parent, cchar *pattern, void *proc)
{
    HttpRoute   *route;

    if ((route = httpDefineRoute(parent, "ALL", pattern, "$&", "unused")) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    httpSetRouteHandler(route, "espHandler");

    route->update = 0;
    espDefineAction(route, pattern, proc);
    return 0;
}


/************************************ Init ************************************/
/*
    ESP module
 */
PUBLIC int espOpen(MprModule *module)
{
    HttpStage   *handler;

    if ((handler = httpCreateHandler("espHandler", module)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->espHandler = handler;
    handler->open = openEsp;
    handler->close = closeEsp;
    handler->start = startEsp;
    if ((esp = mprAllocObj(Esp, manageEsp)) == 0) {
        return MPR_ERR_MEMORY;
    }
    handler->stageData = esp;
    MPR->espService = esp;
    esp->mutex = mprCreateLock();
    esp->local = mprCreateThreadLocal();
    if (module) {
        mprSetModuleFinalizer(module, unloadEsp);
    }
    if (espInitParser() < 0) {
        return 0;
    }
    if ((esp->ediService = ediCreateService()) == 0) {
        return 0;
    }
#if ME_COM_MDB
    /* Memory database */
    mdbInit();
#endif
#if ME_COM_SQLITE
    sdbInit();
#endif
    return 0;
}


static int unloadEsp(MprModule *mp)
{
    HttpStage   *stage;

    if (esp->inUse) {
       return MPR_ERR_BUSY;
    }
    if (mprIsStopping()) {
        return 0;
    }
    if ((stage = httpLookupStage(mp->name)) != 0) {
        stage->flags |= HTTP_STAGE_UNLOADED;
    }
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
