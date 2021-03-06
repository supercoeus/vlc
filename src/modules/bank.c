/*****************************************************************************
 * bank.c : Modules list
 *****************************************************************************
 * Copyright (C) 2001-2011 VLC authors and VideoLAN
 *
 * Authors: Sam Hocevar <sam@zoy.org>
 *          Ethan C. Baldridge <BaldridgeE@cadmus.com>
 *          Hans-Peter Jansen <hpj@urpla.net>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_modules.h>
#include <vlc_fs.h>
#include <vlc_block.h>
#include "libvlc.h"
#include "config/configuration.h"
#include "modules/modules.h"

static struct
{
    vlc_mutex_t lock;
    vlc_plugin_t *libs;
    block_t *caches;
    unsigned usage;
} modules = { VLC_STATIC_MUTEX, NULL, NULL, 0 };

static void module_StoreBank(vlc_plugin_t *lib)
{
    /*vlc_assert_locked (&modules.lock);*/
    lib->next = modules.libs;
    modules.libs = lib;
}

/**
 * Registers a statically-linked plug-in.
 */
static vlc_plugin_t *module_InitStatic(vlc_plugin_cb entry)
{
    /* Initializes the statically-linked library */
    vlc_plugin_t *lib = vlc_plugin_describe (entry);
    if (unlikely(lib == NULL))
        return NULL;

    assert(lib->module != NULL);
    lib->module->b_loaded = true;
    lib->module->b_unloadable = false;
    return lib;
}

#if defined(__ELF__) || !HAVE_DYNAMIC_PLUGINS
# ifdef __GNUC__
__attribute__((weak))
# else
#  pragma weak vlc_static_modules
# endif
extern vlc_plugin_cb vlc_static_modules[];

static void module_InitStaticModules(void)
{
    if (!vlc_static_modules)
        return;

    for (unsigned i = 0; vlc_static_modules[i]; i++)
    {
        vlc_plugin_t *lib = module_InitStatic(vlc_static_modules[i]);
        if (likely(lib != NULL))
            module_StoreBank(lib);
    }
}
#else
static void module_InitStaticModules(void) { }
#endif

#ifdef HAVE_DYNAMIC_PLUGINS
#ifdef __OS2__
#   define EXTERN_PREFIX "_"
#else
#   define EXTERN_PREFIX
#endif

/**
 * Loads a dynamically-linked plug-in into memory and initialize it.
 *
 * The module can then be handled by module_need() and module_unneed().
 *
 * \param path file path of the shared object
 * \param fast whether to optimize loading for speed or safety
 *             (fast is used when the plug-in is registered but not used)
 */
static vlc_plugin_t *module_InitDynamic(vlc_object_t *obj, const char *path,
                                        bool fast)
{
    module_handle_t handle;

    if (module_Load (obj, path, &handle, fast))
        return NULL;

    /* Try to resolve the symbol */
    static const char entry_name[] = EXTERN_PREFIX "vlc_entry" MODULE_SUFFIX;
    vlc_plugin_cb entry =
        (vlc_plugin_cb) module_Lookup (handle, entry_name);
    if (entry == NULL)
    {
        msg_Warn (obj, "cannot find plug-in entry point in %s", path);
        goto error;
    }

    /* We can now try to call the symbol */
    vlc_plugin_t *plugin = vlc_plugin_describe(entry);
    if (unlikely(plugin == NULL))
    {
        /* With a well-written module we shouldn't have to print an
         * additional error message here, but just make sure. */
        msg_Err (obj, "cannot initialize plug-in %s", path);
        goto error;
    }

    assert(plugin->module != NULL);

    plugin->module->psz_filename = strdup (path);
    if (unlikely(plugin->module->psz_filename == NULL))
    {
        vlc_plugin_destroy(plugin);
        goto error;
    }
    plugin->module->handle = handle;
    plugin->module->b_loaded = true;
    return plugin;
error:
    module_Unload( handle );
    return NULL;
}

typedef enum { CACHE_USE, CACHE_RESET, CACHE_IGNORE } cache_mode_t;

typedef struct module_bank
{
    vlc_object_t *obj;
    const char   *base;
    cache_mode_t  mode;

    size_t        size;
    vlc_plugin_t **plugins;
    vlc_plugin_t *cache;
} module_bank_t;

/**
 * Scans a plug-in from a file.
 */
static int AllocatePluginFile (module_bank_t *bank, const char *abspath,
                               const char *relpath, const struct stat *st)
{
    vlc_plugin_t *plugin = NULL;

    /* Check our plugins cache first then load plugin if needed */
    if (bank->mode == CACHE_USE)
    {
        vlc_plugin_t *cache = vlc_cache_lookup(&bank->cache, relpath, st);
        if (cache != NULL)
        {
            assert(cache->module != NULL);
            cache->module->psz_filename = strdup(abspath);
            if (likely(cache->module->psz_filename != NULL))
                plugin = cache;
            else
                vlc_plugin_destroy(cache);
        }
    }
    if (plugin == NULL)
    {
        plugin = module_InitDynamic(bank->obj, abspath, true);
        plugin->path = xstrdup(relpath);
        plugin->mtime = st->st_mtime;
        plugin->size = st->st_size;
    }
    if (plugin == NULL)
        return -1;

    module_t *module = plugin->module;
    assert(module != NULL);

    /* For now we force loading if the module's config contains callbacks.
     * Could be optimized by adding an API call.*/
    for (size_t n = module->confsize, i = 0; i < n; i++)
         if (!module->b_loaded
          && module->p_config[i].list_count == 0
          && (module->p_config[i].list.psz_cb != NULL || module->p_config[i].list.i_cb != NULL))
         {
             /* !unloadable not allowed for plugins with callbacks */
             vlc_plugin_destroy(plugin);

             assert(bank->mode != CACHE_RESET);
             plugin = module_InitDynamic(bank->obj, abspath, false);
             if (unlikely(plugin == NULL))
                 return -1;
             break;
         }

    module_StoreBank(plugin);

    if (bank->mode != CACHE_IGNORE) /* Add entry to bank */
        CacheAdd(&bank->plugins, &bank->size, plugin);
    /* TODO: deal with errors */
    return  0;
}

/**
 * Recursively browses a directory to look for plug-ins.
 */
static void AllocatePluginDir (module_bank_t *bank, unsigned maxdepth,
                               const char *absdir, const char *reldir)
{
    if (maxdepth == 0)
        return;
    maxdepth--;

    DIR *dh = vlc_opendir (absdir);
    if (dh == NULL)
        return;

    /* Parse the directory and try to load all files it contains. */
    for (;;)
    {
        char *relpath = NULL, *abspath = NULL;
        const char *file = vlc_readdir (dh);
        if (file == NULL)
            break;

        /* Skip ".", ".." */
        if (!strcmp (file, ".") || !strcmp (file, ".."))
            continue;

        /* Compute path relative to plug-in base directory */
        if (reldir != NULL)
        {
            if (asprintf (&relpath, "%s"DIR_SEP"%s", reldir, file) == -1)
                relpath = NULL;
        }
        else
            relpath = strdup (file);
        if (unlikely(relpath == NULL))
            continue;

        /* Compute absolute path */
        if (asprintf (&abspath, "%s"DIR_SEP"%s", bank->base, relpath) == -1)
        {
            abspath = NULL;
            goto skip;
        }

        struct stat st;
        if (vlc_stat (abspath, &st) == -1)
            goto skip;

        if (S_ISREG (st.st_mode))
        {
            static const char prefix[] = "lib";
            static const char suffix[] = "_plugin"LIBEXT;
            size_t len = strlen (file);

#ifndef __OS2__
            /* Check that file matches the "lib*_plugin"LIBEXT pattern */
            if (len > strlen (suffix)
             && !strncmp (file, prefix, strlen (prefix))
             && !strcmp (file + len - strlen (suffix), suffix))
#else
            /* We load all the files ending with LIBEXT on OS/2,
             * because OS/2 has a 8.3 length limitation for DLL name */
            if (len > strlen (LIBEXT)
             && !strcasecmp (file + len - strlen (LIBEXT), LIBEXT))
#endif
                AllocatePluginFile (bank, abspath, relpath, &st);
        }
        else if (S_ISDIR (st.st_mode))
            /* Recurse into another directory */
            AllocatePluginDir (bank, maxdepth, abspath, relpath);
    skip:
        free (relpath);
        free (abspath);
    }
    closedir (dh);
}

/**
 * Scans for plug-ins within a file system hierarchy.
 * \param path base directory to browse
 */
static void AllocatePluginPath(vlc_object_t *obj, const char *path,
                               cache_mode_t mode)
{
    module_bank_t bank =
    {
        .obj = obj,
        .base = path,
        .mode = mode,
    };

    if (mode == CACHE_USE)
        bank.cache = vlc_cache_load(obj, path, &modules.caches);
    else
        msg_Dbg(bank.obj, "ignoring plugins cache file");

    msg_Dbg(obj, "recursively browsing `%s'", bank.base);

    /* Don't go deeper than 5 subdirectories */
    AllocatePluginDir (&bank, 5, path, NULL);

    /* Discard unmatched cache entries */
    while (bank.cache != NULL)
    {
        vlc_plugin_t *plugin = bank.cache;

        bank.cache = plugin->next;
        vlc_plugin_destroy(plugin);
    }

    if (mode == CACHE_RESET)
        CacheSave(obj, path, bank.plugins, bank.size);

    free(bank.plugins);
}

/**
 * Enumerates all dynamic plug-ins that can be found.
 *
 * This function will recursively browse the default plug-ins directory and any
 * directory listed in the VLC_PLUGIN_PATH environment variable.
 * For performance reasons, a cache is normally used so that plug-in shared
 * objects do not need to loaded and linked into the process.
 */
static void AllocateAllPlugins (vlc_object_t *p_this)
{
    char *paths;
    cache_mode_t mode;

    if( !var_InheritBool( p_this, "plugins-cache" ) )
        mode = CACHE_IGNORE;
    else if( var_InheritBool( p_this, "reset-plugins-cache" ) )
        mode = CACHE_RESET;
    else
        mode = CACHE_USE;

#if VLC_WINSTORE_APP
    /* Windows Store Apps can not load external plugins with absolute paths. */
    AllocatePluginPath (p_this, "plugins", mode);
#else
    /* Contruct the special search path for system that have a relocatable
     * executable. Set it to <vlc path>/plugins. */
    char *vlcpath = config_GetLibDir ();
    if (likely(vlcpath != NULL)
     && likely(asprintf (&paths, "%s" DIR_SEP "plugins", vlcpath) != -1))
    {
        AllocatePluginPath (p_this, paths, mode);
        free( paths );
    }
    free (vlcpath);
#endif /* VLC_WINSTORE_APP */

    /* If the user provided a plugin path, we add it to the list */
    paths = getenv( "VLC_PLUGIN_PATH" );
    if( paths == NULL )
        return;

    paths = strdup( paths ); /* don't harm the environment ! :) */
    if( unlikely(paths == NULL) )
        return;

    for( char *buf, *path = strtok_r( paths, PATH_SEP, &buf );
         path != NULL;
         path = strtok_r( NULL, PATH_SEP, &buf ) )
        AllocatePluginPath (p_this, path, mode);

    free( paths );
}
#endif /* HAVE_DYNAMIC_PLUGINS */

/**
 * Init bank
 *
 * Creates a module bank structure which will be filled later
 * on with all the modules found.
 */
void module_InitBank (void)
{
    vlc_mutex_lock (&modules.lock);

    if (modules.usage == 0)
    {
        /* Fills the module bank structure with the core module infos.
         * This is very useful as it will allow us to consider the core
         * library just as another module, and for instance the configuration
         * options of core will be available in the module bank structure just
         * as for every other module. */
        vlc_plugin_t *plugin = module_InitStatic(vlc_entry__core);
        assert(plugin != NULL);
        if (likely(plugin != NULL))
            module_StoreBank(plugin);
        config_SortConfig ();
    }
    modules.usage++;

    /* We do retain the module bank lock until the plugins are loaded as well.
     * This is ugly, this staged loading approach is needed: LibVLC gets
     * some configuration parameters relevant to loading the plugins from
     * the core (builtin) module. The module bank becomes shared read-only data
     * once it is ready, so we need to fully serialize initialization.
     * DO NOT UNCOMMENT the following line unless you managed to squeeze
     * module_LoadPlugins() before you unlock the mutex. */
    /*vlc_mutex_unlock (&modules.lock);*/
}

/**
 * Unloads all unused plugin modules and empties the module
 * bank in case of success.
 */
void module_EndBank (bool b_plugins)
{
    vlc_plugin_t *libs = NULL;
    block_t *caches = NULL;

    /* If plugins were _not_ loaded, then the caller still has the bank lock
     * from module_InitBank(). */
    if( b_plugins )
        vlc_mutex_lock (&modules.lock);
    /*else
        vlc_assert_locked (&modules.lock); not for static mutexes :( */

    assert (modules.usage > 0);
    if (--modules.usage == 0)
    {
        config_UnsortConfig ();
        libs = modules.libs;
        caches = modules.caches;
        modules.libs = NULL;
        modules.caches = NULL;
    }
    vlc_mutex_unlock (&modules.lock);

    while (libs != NULL)
    {
        vlc_plugin_t *lib = libs;

        libs = lib->next;
#ifdef HAVE_DYNAMIC_PLUGINS
        assert(lib->module != NULL);
        if (lib->module->b_loaded && lib->module->b_unloadable)
        {
            module_Unload(lib->module->handle);
            lib->module->b_loaded = false;
        }
#endif
        vlc_plugin_destroy(lib);
    }

    block_ChainRelease(caches);
}

#undef module_LoadPlugins
/**
 * Loads module descriptions for all available plugins.
 * Fills the module bank structure with the plugin modules.
 *
 * \param p_this vlc object structure
 * \return total number of modules in bank after loading all plug-ins
 */
size_t module_LoadPlugins (vlc_object_t *obj)
{
    /*vlc_assert_locked (&modules.lock); not for static mutexes :( */

    if (modules.usage == 1)
    {
        module_InitStaticModules ();
#ifdef HAVE_DYNAMIC_PLUGINS
        msg_Dbg (obj, "searching plug-in modules");
        AllocateAllPlugins (obj);
#endif
        config_UnsortConfig ();
        config_SortConfig ();
    }
    vlc_mutex_unlock (&modules.lock);

    size_t count;
    module_t **list = module_list_get (&count);
    module_list_free (list);
    msg_Dbg (obj, "plug-ins loaded: %zu modules", count);
    return count;
}

/**
 * Frees the flat list of VLC modules.
 * @param list list obtained by module_list_get()
 * @param length number of items on the list
 * @return nothing.
 */
void module_list_free (module_t **list)
{
    free (list);
}

/**
 * Gets the flat list of VLC modules.
 * @param n [OUT] pointer to the number of modules
 * @return table of module pointers (release with module_list_free()),
 *         or NULL in case of error (in that case, *n is zeroed).
 */
module_t **module_list_get (size_t *n)
{
    module_t **tab = NULL;
    size_t i = 0;

    assert (n != NULL);

    for (vlc_plugin_t *lib = modules.libs; lib != NULL; lib = lib->next)
    {
        module_t *mod = lib->module;
        assert(mod != NULL);

         module_t **nt;
         nt  = realloc (tab, (i + 1 + mod->submodule_count) * sizeof (*tab));
         if (unlikely(nt == NULL))
         {
             free (tab);
             *n = 0;
             return NULL;
         }

         tab = nt;
         tab[i++] = mod;
         for (module_t *subm = mod->submodule; subm; subm = subm->next)
             tab[i++] = subm;
    }
    *n = i;
    return tab;
}

static int modulecmp (const void *a, const void *b)
{
    const module_t *const *ma = a, *const *mb = b;
    /* Note that qsort() uses _ascending_ order,
     * so the smallest module is the one with the biggest score. */
    return (*mb)->i_score - (*ma)->i_score;
}

/**
 * Builds a sorted list of all VLC modules with a given capability.
 * The list is sorted from the highest module score to the lowest.
 * @param list pointer to the table of modules [OUT]
 * @param cap capability of modules to look for
 * @return the number of matching found, or -1 on error (*list is then NULL).
 * @note *list must be freed with module_list_free().
 */
ssize_t module_list_cap (module_t ***restrict list, const char *cap)
{
    /* TODO: This is quite inefficient. List should be sorted by capability. */
    ssize_t n = 0;

    assert (list != NULL);

    for (vlc_plugin_t *lib = modules.libs; lib != NULL; lib = lib->next)
    {
         module_t *mod = lib->module;
         assert(mod != NULL);

         if (module_provides (mod, cap))
             n++;
         for (module_t *subm = mod->submodule; subm != NULL; subm = subm->next)
             if (module_provides (subm, cap))
                 n++;
    }

    module_t **tab = malloc (sizeof (*tab) * n);
    *list = tab;
    if (unlikely(tab == NULL))
        return -1;

    for (vlc_plugin_t *lib = modules.libs; lib != NULL; lib = lib->next)
    {
         module_t *mod = lib->module;
         assert(mod != NULL);

         if (module_provides (mod, cap))
             *(tab++)= mod;
         for (module_t *subm = mod->submodule; subm != NULL; subm = subm->next)
             if (module_provides (subm, cap))
                 *(tab++) = subm;
    }

    assert (tab == *list + n);
    qsort (*list, n, sizeof (*tab), modulecmp);
    return n;
}

/**
 * Makes sure the module is loaded in memory.
 * \return 0 on success, -1 on failure
 */
int module_Map (vlc_object_t *obj, module_t *module)
{
    static vlc_mutex_t lock = VLC_STATIC_MUTEX;
    vlc_plugin_t *plugin = module->plugin;

    assert(plugin != NULL);
    module = plugin->module;
    assert(module != NULL);

    vlc_mutex_lock(&lock);
    if (!module->b_loaded)
    {
        vlc_plugin_t *uncache;

        assert (module->psz_filename != NULL);
#ifdef HAVE_DYNAMIC_PLUGINS
        uncache = module_InitDynamic (obj, module->psz_filename, false);
        if (uncache != NULL)
        {
            assert(uncache->module != NULL);
            CacheMerge(obj, module, uncache->module);
            vlc_plugin_destroy(uncache);
        }
        else
#endif
        {
            msg_Err (obj, "corrupt module: %s", module->psz_filename);
            module = NULL;
        }
    }
    vlc_mutex_unlock(&lock);
    return -(module == NULL);
}
