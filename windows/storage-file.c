/*
 * storage-file.c: file-based implementation of the interface defined
 * in storage.h, for Win32s.
 *
 * Win32s has no usable registry: the Reg*Ex functions that the
 * registry-based implementation needs are not supported there (see
 * WIN32S.DAT, which lists them among the APIs Win32s does not
 * support). So on Win32s the saved sessions, the host keys and the
 * random seed all live in files instead, under $PUTTYDIR, or failing
 * that $HOME\putty.
 *
 * Session names are mapped to file names, which on FAT filesystems
 * have to fit in the 8.3 format, so a session name is reduced to
 * eight uppercase characters. That means two different session names
 * could map to the same file name; the default session, being the one
 * most likely to be used, gets a name of its own that no other
 * session can be given.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "putty.h"
#include "storage.h"
#include "tree234.h"

/*
 * The places where the various pieces of persistent state live. Each
 * one is a path relative to the PuTTY directory.
 */
enum {
    INDEX_DIR,        /* base putty directory */
    INDEX_SESSIONDIR, /* sessions subdirectory */
    INDEX_SESSION,    /* individual session file */
    INDEX_HOSTKEYS,   /* host keys file */
    INDEX_RANDSEED,   /* random seed file */
};

/*
 * Create every directory in the given path, if it does not exist
 * already. The path may name a file as well as directories; only the
 * directory part of it is created.
 */
static void make_dir_path(const char *path)
{
    char *buf = dupstr(path);
    char *p;

    for (p = buf; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char old = *p;
            *p = '\0';
            _mkdir(buf);
            *p = old;
        }
    }
    _mkdir(buf);

    sfree(buf);
}

/*
 * Convert a session name to a file name. On FAT filesystems a file
 * name has to fit in the 8.3 format, so we reduce the session name to
 * eight uppercase characters made only of characters that are legal
 * in a file name.
 */
static char *make_session_filename(const char *in)
{
    strbuf *sb;
    const char *p;
    int count = 0;

    if (!strcmp(in, DEFAULT_SESSION_NAME))
        return dupstr(DEFAULT_SESSION_NAME);

    sb = strbuf_new();
    for (p = in; *p && count < 8; p++) {
        unsigned char c = (unsigned char)*p;

        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';

        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            put_byte(sb, c);
            count++;
        } else if (c == ' ' || c == '-' || c == '.') {
            put_byte(sb, '_');
            count++;
        }
        /* Anything else is dropped. */
    }
    if (sb->len == 0)
        put_datapl(sb, PTRLEN_LITERAL("_SESS"));

    return strbuf_to_str(sb);
}

/*
 * Construct the path of one of the things listed above. The result
 * is dynamically allocated.
 */
static char *make_filename(int index, const char *subname)
{
    char *env, *base, *ret;

    env = getenv("PUTTYDIR");
    if (env && *env) {
        base = dupstr(env);
    } else {
        env = getenv("HOME");
        if (!env || !*env)
            env = getenv("HOMEDRIVE");
        if (!env || !*env)
            env = "C:";
        base = dupcat(env, "\\putty", NULL);
    }

    switch (index) {
      case INDEX_DIR:
        ret = base;
        break;
      case INDEX_SESSIONDIR:
        ret = dupcat(base, "\\sessions", NULL);
        sfree(base);
        break;
      case INDEX_SESSION: {
        char *enc = make_session_filename(subname);
        ret = dupcat(base, "\\sessions\\", enc, NULL);
        sfree(enc);
        sfree(base);
        break;
      }
      case INDEX_HOSTKEYS:
        ret = dupcat(base, "\\HOSTKEYS", NULL);
        sfree(base);
        break;
      case INDEX_RANDSEED:
        ret = dupcat(base, "\\RNDSEED", NULL);
        sfree(base);
        break;
      default:
        sfree(base);
        ret = NULL;
        break;
    }

    return ret;
}

/* ----------------------------------------------------------------------
 * Sessions.
 */

struct settings_w {
    FILE *fp;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    settings_w *handle;
    char *dir, *filename;
    FILE *fp;

    *errmsg = NULL;

    if (!sessionname || !*sessionname)
        sessionname = DEFAULT_SESSION_NAME;

    dir = make_filename(INDEX_SESSIONDIR, NULL);
    make_dir_path(dir);
    sfree(dir);

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "w");
    if (!fp) {
        *errmsg = dupprintf("Unable to create session file\n%s", filename);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    handle = snew(settings_w);
    handle->fp = fp;
    return handle;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    if (handle)
        fprintf(handle->fp, "%s=%s\n", key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    if (handle)
        fprintf(handle->fp, "%s=%d\n", key, value);
}

void close_settings_w(settings_w *handle)
{
    if (!handle)
        return;

    fclose(handle->fp);
    sfree(handle);
}

/*
 * Reading a session back means parsing the file into a list of
 * key/value pairs, which we keep in a tree so that a lookup is quick.
 */
struct skv {
    char *key;
    char *value;
};

static int skvcmp(void *av, void *bv)
{
    const struct skv *a = (const struct skv *)av;
    const struct skv *b = (const struct skv *)bv;
    return strcmp(a->key, b->key);
}

struct settings_r {
    tree234 *t;
};

settings_r *open_settings_r(const char *sessionname)
{
    settings_r *handle;
    char *filename;
    FILE *fp;
    char *line;

    if (!sessionname || !*sessionname)
        sessionname = DEFAULT_SESSION_NAME;

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return NULL;

    handle = snew(settings_r);
    handle->t = newtree234(skvcmp);

    while ((line = fgetline(fp)) != NULL) {
        char *eq = strchr(line, '=');
        if (eq) {
            struct skv *kv = snew(struct skv);
            char *val = eq + 1;
            size_t len = strlen(val);

            *eq = '\0';
            kv->key = dupstr(line);

            /* Strip the trailing newline from the value. */
            if (len > 0 && val[len-1] == '\n')
                val[len-1] = '\0';
            kv->value = dupstr(val);

            if (add234(handle->t, kv) != kv) {
                /* A duplicate key: the later one wins. */
                sfree(kv->key);
                sfree(kv->value);
                sfree(kv);
            }
        }
        sfree(line);
    }
    fclose(fp);

    return handle;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    struct skv tmp, *found;

    if (!handle)
        return NULL;

    tmp.key = (char *)key;
    found = find234(handle->t, &tmp, NULL);
    if (found)
        return dupstr(found->value);
    return NULL;
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    struct skv tmp, *found;

    if (!handle)
        return defvalue;

    tmp.key = (char *)key;
    found = find234(handle->t, &tmp, NULL);
    if (found)
        return atoi(found->value);
    return defvalue;
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    char *settingname;
    char *fontname;
    FontSpec *ret;
    int isbold, height, charset;

    fontname = read_setting_s(handle, name);
    if (!fontname)
        return NULL;

    settingname = dupcat(name, "IsBold", NULL);
    isbold = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (isbold == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "CharSet", NULL);
    charset = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (charset == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "Height", NULL);
    height = read_setting_i(handle, settingname, INT_MIN);
    sfree(settingname);
    if (height == INT_MIN) {
        sfree(fontname);
        return NULL;
    }

    ret = fontspec_new(fontname, isbold != 0, height, charset);
    sfree(fontname);
    return ret;
}

void write_setting_fontspec(settings_w *handle,
                            const char *name, FontSpec *font)
{
    char *settingname;

    write_setting_s(handle, name, font->name);
    settingname = dupcat(name, "IsBold", NULL);
    write_setting_i(handle, settingname, font->isbold);
    sfree(settingname);
    settingname = dupcat(name, "CharSet", NULL);
    write_setting_i(handle, settingname, font->charset);
    sfree(settingname);
    settingname = dupcat(name, "Height", NULL);
    write_setting_i(handle, settingname, font->height);
    sfree(settingname);
}

Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    } else
        return NULL;
}

void write_setting_filename(settings_w *handle,
                            const char *name, Filename *result)
{
    write_setting_s(handle, name, result->cpath);
}

void close_settings_r(settings_r *handle)
{
    struct skv *kv;

    if (!handle)
        return;

    while ((kv = delpos234(handle->t, 0)) != NULL) {
        sfree(kv->key);
        sfree(kv->value);
        sfree(kv);
    }
    freetree234(handle->t);
    sfree(handle);
}

void del_settings(const char *sessionname)
{
    char *filename = make_filename(INDEX_SESSION, sessionname);
    remove(filename);
    sfree(filename);
}

/* ----------------------------------------------------------------------
 * Enumerating the saved sessions.
 */

struct settings_e {
    HANDLE hFind;
    WIN32_FIND_DATA fd;
    bool first;
};

settings_e *enum_settings_start(void)
{
    settings_e *e;
    char *dir, *wildcard;

    dir = make_filename(INDEX_SESSIONDIR, NULL);
    wildcard = dupcat(dir, "\\*.*", NULL);
    sfree(dir);

    e = snew(settings_e);
    e->hFind = FindFirstFile(wildcard, &e->fd);
    sfree(wildcard);

    if (e->hFind == INVALID_HANDLE_VALUE) {
        sfree(e);
        return NULL;
    }
    e->first = true;
    return e;
}

bool enum_settings_next(settings_e *e, strbuf *sb)
{
    while (1) {
        if (!e->first) {
            if (!FindNextFile(e->hFind, &e->fd))
                return false;
        }
        e->first = false;

        /* Skip the directories, and the . and .. entries. */
        if (e->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        put_datapl(sb, ptrlen_from_asciz(e->fd.cFileName));
        return true;
    }
}

void enum_settings_finish(settings_e *e)
{
    FindClose(e->hFind);
    sfree(e);
}

/* ----------------------------------------------------------------------
 * The host key database.
 */

/*
 * The name under which a host key is stored. This has to be usable
 * as part of a line in a text file, so it cannot contain a newline.
 */
static void hostkey_regname(strbuf *sb, const char *hostname,
                            int port, const char *keytype)
{
    BinarySink_put_fmt(BinarySink_UPCAST(sb), "%s@%d:%s", keytype, port, hostname);
}

int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key)
{
    char *filename = make_filename(INDEX_HOSTKEYS, NULL);
    FILE *fp = fopen(filename, "r");
    char *line;
    int ret = 1;                 /* not found */
    strbuf *expected;

    sfree(filename);

    if (!fp)
        return ret;              /* key does not exist */

    expected = strbuf_new();
    hostkey_regname(expected, hostname, port, keytype);

    while ((line = fgetline(fp)) != NULL) {
        size_t len = strlen(line);
        char *space;

        if (len > 0 && line[len-1] == '\n')
            line[--len] = '\0';

        /* Each line is: type@port:hostname keydata */
        space = strchr(line, ' ');
        if (space) {
            *space = '\0';
            if (!strcmp(line, expected->s)) {
                if (!strcmp(space + 1, key))
                    ret = 0;     /* key matches */
                else
                    ret = 2;     /* key is different */
                break;
            }
        }
        sfree(line);
    }

    fclose(fp);
    strbuf_free(expected);
    return ret;
}

bool have_ssh_host_key(const char *hostname, int port, const char *keytype)
{
    return check_stored_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{
    char *filename = make_filename(INDEX_HOSTKEYS, NULL);
    char *tmpfilename = dupcat(filename, ".tmp", NULL);
    char *dir;
    FILE *rfp, *wfp;
    strbuf *expected;

    /*
     * Read the existing host keys, replacing any existing entry for
     * this host and key type, and write the result back out.
     */
    expected = strbuf_new();
    hostkey_regname(expected, hostname, port, keytype);

    dir = make_filename(INDEX_DIR, NULL);
    make_dir_path(dir);
    sfree(dir);

    rfp = fopen(filename, "r");
    wfp = fopen(tmpfilename, "w");
    if (!wfp) {
        /* best-effort cleanup; nonfatal() is not available in every */
        /* program that links this storage backend */
        goto cleanup;
    }

    if (rfp) {
        char *line;
        while ((line = fgetline(rfp)) != NULL) {
            char *space = strchr(line, ' ');

            if (space) {
                /* Is this line for the same host key? */
                char saved = *space;
                *space = '\0';
                if (strcmp(line, expected->s) != 0) {
                    *space = saved;
                    fputs(line, wfp);
                }
                /* Otherwise skip it: we write the new one below. */
            } else {
                fputs(line, wfp);
            }
            sfree(line);
        }
    }
    fprintf(wfp, "%s %s\n", expected->s, key);
    fclose(wfp);
    wfp = NULL;

    /*
     * Windows will not delete or rename over an open file, so the
     * old file has to be closed first.
     */
    if (rfp) {
        fclose(rfp);
        rfp = NULL;
    }

    remove(filename);
    rename(tmpfilename, filename);

  cleanup:
    if (rfp)
        fclose(rfp);
    if (wfp)
        fclose(wfp);
    strbuf_free(expected);
    sfree(tmpfilename);
    sfree(filename);
}

/* ----------------------------------------------------------------------
 * The host CA database, which is not supported on Win32s.
 */

struct host_ca_enum {
    int dummy;
};

host_ca_enum *enum_host_ca_start(void) { return NULL; }
bool enum_host_ca_next(host_ca_enum *e, strbuf *sb) { return false; }
void enum_host_ca_finish(host_ca_enum *e) { }
host_ca *host_ca_load(const char *name) { return NULL; }

char *host_ca_save(host_ca *hca)
{
    return dupstr("Host CA storage is not supported on Win32s");
}

char *host_ca_delete(const char *name) { return NULL; }

/* ----------------------------------------------------------------------
 * The random seed file.
 */

void read_random_seed(noise_consumer_t consumer)
{
    char *fname = make_filename(INDEX_RANDSEED, NULL);
    FILE *fp = fopen(fname, "rb");

    sfree(fname);

    if (fp) {
        char buf[1024];
        size_t len;
        while ((len = fread(buf, 1, sizeof(buf), fp)) > 0)
            consumer(buf, len);
        fclose(fp);
    }
}

void write_random_seed(void *data, int len)
{
    char *dir, *fname;
    FILE *fp;

    dir = make_filename(INDEX_DIR, NULL);
    make_dir_path(dir);
    sfree(dir);

    fname = make_filename(INDEX_RANDSEED, NULL);
    fp = fopen(fname, "wb");
    sfree(fname);

    if (fp) {
        fwrite(data, 1, len, fp);
        fclose(fp);
    }
}

/* ----------------------------------------------------------------------
 * The jump list, which is a registry feature and so is not supported.
 */

int add_to_jumplist_registry(const char *item) { return JUMPLISTREG_OK; }
int remove_from_jumplist_registry(const char *item) { return JUMPLISTREG_OK; }

char *get_jumplist_registry_entries(void)
{
    char *list = snewn(2, char);
    list[0] = '\0';
    list[1] = '\0';
    return list;
}

/* ----------------------------------------------------------------------
 * Cleaning up all of our persistent state.
 */

void cleanup_all(void) { }
