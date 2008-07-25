/*
 * xm_internal.h: helper routines for dealing with inactive domains
 *
 * Copyright (C) 2006-2007 Red Hat
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 *
 */

#ifdef WITH_XEN
#include <config.h>

#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <stdint.h>
#include <xen/dom0_ops.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#ifndef NAME_MAX
#define	NAME_MAX	255
#endif

#include "xen_unified.h"
#include "xm_internal.h"
#include "xend_internal.h"
#include "hash.h"
#include "internal.h"
#include "xml.h"
#include "buf.h"
#include "uuid.h"
#include "util.h"
#include "memory.h"

/* The true Xen limit varies but so far is always way
   less than 1024, which is the Linux kernel limit according
   to sched.h, so we'll match that for now */
#define XEN_MAX_PHYSICAL_CPU 1024

static int xenXMConfigSetString(virConfPtr conf, const char *setting,
                                const char *str);
static int xenXMDiskCompare(virDomainDiskDefPtr a,
                            virDomainDiskDefPtr b);

typedef struct xenXMConfCache *xenXMConfCachePtr;
typedef struct xenXMConfCache {
    time_t refreshedAt;
    char filename[PATH_MAX];
    virDomainDefPtr def;
} xenXMConfCache;

static char configDir[PATH_MAX];
/* Config file name to config object */
static virHashTablePtr configCache = NULL;
/* Name to config file name */
static virHashTablePtr nameConfigMap = NULL;
static int nconnections = 0;
static time_t lastRefresh = 0;

char * xenXMAutoAssignMac(void);
static int xenXMDomainAttachDevice(virDomainPtr domain, const char *xml);
static int xenXMDomainDetachDevice(virDomainPtr domain, const char *xml);

#define XM_REFRESH_INTERVAL 10

#define XM_CONFIG_DIR "/etc/xen"
#define XM_EXAMPLE_PREFIX "xmexample"
#define XEND_CONFIG_FILE "xend-config.sxp"
#define XEND_PCI_CONFIG_PREFIX "xend-pci-"
#define QEMU_IF_SCRIPT "qemu-ifup"
#define XM_XML_ERROR "Invalid xml"

struct xenUnifiedDriver xenXMDriver = {
    xenXMOpen, /* open */
    xenXMClose, /* close */
    NULL, /* version */
    NULL, /* hostname */
    NULL, /* URI */
    NULL, /* nodeGetInfo */
    NULL, /* getCapabilities */
    NULL, /* listDomains */
    NULL, /* numOfDomains */
    NULL, /* domainCreateLinux */
    NULL, /* domainSuspend */
    NULL, /* domainResume */
    NULL, /* domainShutdown */
    NULL, /* domainReboot */
    NULL, /* domainDestroy */
    NULL, /* domainGetOSType */
    xenXMDomainGetMaxMemory, /* domainGetMaxMemory */
    xenXMDomainSetMaxMemory, /* domainSetMaxMemory */
    xenXMDomainSetMemory, /* domainMaxMemory */
    xenXMDomainGetInfo, /* domainGetInfo */
    NULL, /* domainSave */
    NULL, /* domainRestore */
    NULL, /* domainCoreDump */
    xenXMDomainSetVcpus, /* domainSetVcpus */
    xenXMDomainPinVcpu, /* domainPinVcpu */
    NULL, /* domainGetVcpus */
    NULL, /* domainGetMaxVcpus */
    xenXMListDefinedDomains, /* listDefinedDomains */
    xenXMNumOfDefinedDomains, /* numOfDefinedDomains */
    xenXMDomainCreate, /* domainCreate */
    xenXMDomainDefineXML, /* domainDefineXML */
    xenXMDomainUndefine, /* domainUndefine */
    xenXMDomainAttachDevice, /* domainAttachDevice */
    xenXMDomainDetachDevice, /* domainDetachDevice */
    NULL, /* domainGetAutostart */
    NULL, /* domainSetAutostart */
    NULL, /* domainGetSchedulerType */
    NULL, /* domainGetSchedulerParameters */
    NULL, /* domainSetSchedulerParameters */
};

static void
xenXMError(virConnectPtr conn, int code, const char *fmt, ...)
{
    va_list args;
    char errorMessage[1024];
    const char *virerr;

    if (fmt) {
        va_start(args, fmt);
        vsnprintf(errorMessage, sizeof(errorMessage)-1, fmt, args);
        va_end(args);
    } else {
        errorMessage[0] = '\0';
    }

    virerr = __virErrorMsg(code, (errorMessage[0] ? errorMessage : NULL));
    __virRaiseError(conn, NULL, NULL, VIR_FROM_XENXM, code, VIR_ERR_ERROR,
                    virerr, errorMessage, NULL, -1, -1, virerr, errorMessage);
}

int
xenXMInit (void)
{
    char *envConfigDir;
    int safeMode = 0;

    /* Disable use of env variable if running setuid */
    if ((geteuid() != getuid()) ||
        (getegid() != getgid()))
        safeMode = 1;

    if (!safeMode &&
        (envConfigDir = getenv("LIBVIRT_XM_CONFIG_DIR")) != NULL) {
        strncpy(configDir, envConfigDir, PATH_MAX-1);
        configDir[PATH_MAX-1] = '\0';
    } else {
        strcpy(configDir, XM_CONFIG_DIR);
    }

    return 0;
}


/* Convenience method to grab a int from the config file object */
static int xenXMConfigGetBool(virConnectPtr conn,
                              virConfPtr conf,
                              const char *name,
                              int *value,
                              int def) {
    virConfValuePtr val;

    *value = 0;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type == VIR_CONF_LONG) {
        *value = val->l ? 1 : 0;
    } else if (val->type == VIR_CONF_STRING) {
        if (!val->str) {
            *value = def;
        }
        *value = STREQ(val->str, "1") ? 1 : 0;
    } else {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config value %s was malformed"), name);
        return -1;
    }
    return 0;
}


/* Convenience method to grab a int from the config file object */
static int xenXMConfigGetULong(virConnectPtr conn,
                               virConfPtr conf,
                               const char *name,
                               unsigned long *value,
                               int def) {
    virConfValuePtr val;

    *value = 0;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type == VIR_CONF_LONG) {
        *value = val->l;
    } else if (val->type == VIR_CONF_STRING) {
        char *ret;
        if (!val->str) {
            *value = def;
        }
        *value = strtol(val->str, &ret, 10);
        if (ret == val->str) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was malformed"), name);
            return -1;
        }
    } else {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config value %s was malformed"), name);
        return -1;
    }
    return 0;
}


/* Convenience method to grab a string from the config file object */
static int xenXMConfigGetString(virConnectPtr conn,
                                virConfPtr conf,
                                const char *name,
                                const char **value,
                                const char *def) {
    virConfValuePtr val;

    *value = NULL;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type != VIR_CONF_STRING) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config value %s was malformed"), name);
        return -1;
    }
    if (!val->str)
        *value = def;
    else
        *value = val->str;
    return 0;
}

static int xenXMConfigCopyStringInternal(virConnectPtr conn,
                                         virConfPtr conf,
                                         const char *name,
                                         char **value,
                                         int allowMissing) {
    virConfValuePtr val;

    *value = NULL;
    if (!(val = virConfGetValue(conf, name))) {
        if (allowMissing)
            return 0;
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config value %s was missing"), name);
        return -1;
    }

    if (val->type != VIR_CONF_STRING) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config value %s was not a string"), name);
        return -1;
    }
    if (!val->str) {
        if (allowMissing)
            return 0;
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config value %s was missing"), name);
        return -1;
    }

    if (!(*value = strdup(val->str))) {
        xenXMError(conn, VIR_ERR_NO_MEMORY, NULL);
        return -1;
    }

    return 0;
}


static int xenXMConfigCopyString(virConnectPtr conn,
                                 virConfPtr conf,
                                 const char *name,
                                 char **value) {
    return xenXMConfigCopyStringInternal(conn, conf, name, value, 0);
}

static int xenXMConfigCopyStringOpt(virConnectPtr conn,
                                    virConfPtr conf,
                                    const char *name,
                                    char **value) {
    return xenXMConfigCopyStringInternal(conn, conf, name, value, 1);
}


/* Convenience method to grab a string UUID from the config file object */
static int xenXMConfigGetUUID(virConfPtr conf, const char *name, unsigned char *uuid) {
    virConfValuePtr val;
    if (!uuid || !name || !conf)
        return (-1);
    if (!(val = virConfGetValue(conf, name))) {
        return (-1);
    }

    if (val->type != VIR_CONF_STRING)
        return (-1);
    if (!val->str)
        return (-1);

    if (virUUIDParse(val->str, uuid) < 0)
        return (-1);

    return (0);
}


/* Release memory associated with a cached config object */
static void xenXMConfigFree(void *payload, const char *key ATTRIBUTE_UNUSED) {
    xenXMConfCachePtr entry = (xenXMConfCachePtr)payload;
    virDomainDefFree(entry->def);
    VIR_FREE(entry);
}


/* Remove any configs which were not refreshed recently */
static int xenXMConfigReaper(const void *payload, const char *key ATTRIBUTE_UNUSED, const void *data) {
    time_t now = *(const time_t *)data;
    xenXMConfCachePtr entry = (xenXMConfCachePtr)payload;

    /* We're going to purge this config file, so check if it
       is currently mapped as owner of a named domain. */
    if (entry->refreshedAt != now) {
        const char *olddomname = entry->def->name;
        char *nameowner = (char *)virHashLookup(nameConfigMap, olddomname);
        if (nameowner && STREQ(nameowner, key)) {
            virHashRemoveEntry(nameConfigMap, olddomname, NULL);
        }
        return (1);
    }
    return (0);
}


static virDomainDefPtr
xenXMConfigReadFile(virConnectPtr conn, const char *filename) {
    virConfPtr conf;
    virDomainDefPtr def;

    if (!(conf = virConfReadFile(filename)))
        return NULL;

    def = xenXMDomainConfigParse(conn, conf);
    virConfFree(conf);

    return def;
}

static int
xenXMConfigSaveFile(virConnectPtr conn, const char *filename, virDomainDefPtr def) {
    virConfPtr conf;
    int ret;

    if (!(conf = xenXMDomainConfigFormat(conn, def)))
        return -1;

    ret = virConfWriteFile(filename, conf);
    virConfFree(conf);
    return ret;
}


/* This method is called by various methods to scan /etc/xen
   (or whatever directory was set by  LIBVIRT_XM_CONFIG_DIR
   environment variable) and process any domain configs. It
   has rate-limited so never rescans more frequently than
   once every X seconds */
static int xenXMConfigCacheRefresh (virConnectPtr conn) {
    DIR *dh;
    struct dirent *ent;
    time_t now = time(NULL);
    int ret = -1;

    if (now == ((time_t)-1)) {
        xenXMError (conn, VIR_ERR_SYSTEM_ERROR, strerror (errno));
        return (-1);
    }

    /* Rate limit re-scans */
    if ((now - lastRefresh) < XM_REFRESH_INTERVAL)
        return (0);

    lastRefresh = now;

    /* Process the files in the config dir */
    if (!(dh = opendir(configDir))) {
        xenXMError (conn, VIR_ERR_SYSTEM_ERROR, strerror (errno));
        return (-1);
    }

    while ((ent = readdir(dh))) {
        xenXMConfCachePtr entry;
        struct stat st;
        int newborn = 0;
        char path[PATH_MAX];

        /*
         * Skip a bunch of crufty files that clearly aren't config files
         */

        /* Like 'dot' files... */
        if (STRPREFIX(ent->d_name, "."))
            continue;
        /* ...and the XenD server config file */
        if (STRPREFIX(ent->d_name, XEND_CONFIG_FILE))
            continue;
        /* ...and random PCI config cruft */
        if (STRPREFIX(ent->d_name, XEND_PCI_CONFIG_PREFIX))
            continue;
        /* ...and the example domain configs */
        if (STRPREFIX(ent->d_name, XM_EXAMPLE_PREFIX))
            continue;
        /* ...and the QEMU networking script */
        if (STRPREFIX(ent->d_name, QEMU_IF_SCRIPT))
            continue;

        /* ...and editor backups */
        if (ent->d_name[0] == '#')
            continue;
        if (ent->d_name[strlen(ent->d_name)-1] == '~')
            continue;

        /* Build the full file path */
        if ((strlen(configDir) + 1 + strlen(ent->d_name) + 1) > PATH_MAX)
            continue;
        strcpy(path, configDir);
        strcat(path, "/");
        strcat(path, ent->d_name);

        /* Skip anything which isn't a file (takes care of scripts/ subdir */
        if ((stat(path, &st) < 0) ||
            (!S_ISREG(st.st_mode))) {
            continue;
        }

        /* If we already have a matching entry and it is not
           modified, then carry on to next one*/
        if ((entry = virHashLookup(configCache, path))) {
            char *nameowner;

            if (entry->refreshedAt >= st.st_mtime) {
                entry->refreshedAt = now;
                continue;
            }

            /* If we currently own the name, then release it and
               re-acquire it later - just in case it was renamed */
            nameowner = (char *)virHashLookup(nameConfigMap, entry->def->name);
            if (nameowner && STREQ(nameowner, path)) {
                virHashRemoveEntry(nameConfigMap, entry->def->name, NULL);
            }

            /* Clear existing config entry which needs refresh */
            virDomainDefFree(entry->def);
            entry->def = NULL;
        } else { /* Completely new entry */
            newborn = 1;
            if (VIR_ALLOC(entry) < 0) {
                xenXMError (conn, VIR_ERR_NO_MEMORY, strerror (errno));
                goto cleanup;
            }
            memcpy(entry->filename, path, PATH_MAX);
        }
        entry->refreshedAt = now;

        if (!(entry->def = xenXMConfigReadFile(conn, entry->filename))) {
            if (!newborn)
                virHashRemoveEntry(configCache, path, NULL);
            VIR_FREE(entry);
            continue;
        }

        /* If its a completely new entry, it must be stuck into
           the cache (refresh'd entries are already registered) */
        if (newborn) {
            if (virHashAddEntry(configCache, entry->filename, entry) < 0) {
                virDomainDefFree(entry->def);
                VIR_FREE(entry);
                xenXMError (conn, VIR_ERR_INTERNAL_ERROR,
                            _("xenXMConfigCacheRefresh: virHashAddEntry"));
                goto cleanup;
            }
        }

        /* See if we need to map this config file in as the primary owner
         * of the domain in question
         */
        if (!virHashLookup(nameConfigMap, entry->def->name)) {
            if (virHashAddEntry(nameConfigMap, entry->def->name, entry->filename) < 0) {
                virHashRemoveEntry(configCache, ent->d_name, NULL);
                virDomainDefFree(entry->def);
                VIR_FREE(entry);
            }
        }
    }

    /* Reap all entries which were not changed, by comparing
       their refresh timestamp - the timestamp should match
       'now' if they were refreshed. If timestamp doesn't match
       then the config is no longer on disk */
    virHashRemoveSet(configCache, xenXMConfigReaper, xenXMConfigFree, (const void*) &now);
    ret = 0;

 cleanup:
    if (dh)
        closedir(dh);

    return (ret);
}


/*
 * Open a 'connection' to the config file directory ;-)
 * We just create a hash table to store config files in.
 * We only support a single directory, so repeated calls
 * to open all end up using the same cache of files
 */
int
xenXMOpen (virConnectPtr conn ATTRIBUTE_UNUSED,
           xmlURIPtr uri ATTRIBUTE_UNUSED,
           virConnectAuthPtr auth ATTRIBUTE_UNUSED,
           int flags ATTRIBUTE_UNUSED)
{
    if (configCache == NULL) {
        configCache = virHashCreate(50);
        if (!configCache)
            return (-1);
        nameConfigMap = virHashCreate(50);
        if (!nameConfigMap) {
            virHashFree(configCache, NULL);
            configCache = NULL;
            return (-1);
        }
        /* Force the cache to be reloaded next time that
         * xenXMConfigCacheRefresh is called.
         */
        lastRefresh = 0;
    }
    nconnections++;

    return (0);
}

/*
 * Free the config files in the cache if this is the
 * last connection
 */
int xenXMClose(virConnectPtr conn ATTRIBUTE_UNUSED) {
    nconnections--;
    if (nconnections <= 0) {
        virHashFree(nameConfigMap, NULL);
        nameConfigMap = NULL;
        virHashFree(configCache, xenXMConfigFree);
        configCache = NULL;
    }
    return (0);
}

/*
 * Since these are all offline domains, we only return info about
 * VCPUs and memory.
 */
int xenXMDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info) {
    const char *filename;
    xenXMConfCachePtr entry;
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return(-1);
    }

    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    memset(info, 0, sizeof(virDomainInfo));
    info->maxMem = entry->def->maxmem;
    info->memory = entry->def->memory;
    info->nrVirtCpu = entry->def->vcpus;
    info->state = VIR_DOMAIN_SHUTOFF;
    info->cpuTime = 0;

    return (0);

}

#define MAX_VFB 1024
/*
 * Turn a config record into a lump of XML describing the
 * domain, suitable for later feeding for virDomainCreateLinux
 */
virDomainDefPtr
xenXMDomainConfigParse(virConnectPtr conn, virConfPtr conf) {
    const char *str;
    int hvm = 0;
    int val;
    virConfValuePtr list;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainDiskDefPtr disk = NULL;
    virDomainNetDefPtr net = NULL;
    virDomainGraphicsDefPtr graphics = NULL;
    int i;
    const char *defaultArch, *defaultMachine;

    if (VIR_ALLOC(def) < 0)
        return NULL;

    def->virtType = VIR_DOMAIN_VIRT_XEN;
    def->id = -1;

    if (xenXMConfigCopyString(conn, conf, "name", &def->name) < 0)
        goto cleanup;
    if (xenXMConfigGetUUID(conf, "uuid", def->uuid) < 0)
        goto cleanup;


    if ((xenXMConfigGetString(conn, conf, "builder", &str, "linux") == 0) &&
        STREQ(str, "hvm"))
        hvm = 1;

    if (!(def->os.type = strdup(hvm ? "hvm" : "xen")))
        goto no_memory;

    defaultArch = virCapabilitiesDefaultGuestArch(priv->caps, def->os.type);
    if (defaultArch == NULL) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("no supported architecture for os type '%s'"),
                   def->os.type);
        goto cleanup;
    }
    if (!(def->os.arch = strdup(defaultArch)))
        goto no_memory;

    defaultMachine = virCapabilitiesDefaultGuestMachine(priv->caps,
                                                        def->os.type,
                                                        def->os.arch);
    if (defaultMachine != NULL) {
        if (!(def->os.machine = strdup(defaultMachine)))
            goto no_memory;
    }

    if (hvm) {
        const char *boot;
        if (xenXMConfigCopyString(conn, conf, "kernel", &def->os.loader) < 0)
            goto cleanup;

        if (xenXMConfigGetString(conn, conf, "boot", &boot, "c") < 0)
            goto cleanup;

        for (i = 0 ; i < VIR_DOMAIN_BOOT_LAST && boot[i] ; i++) {
            switch (*boot) {
            case 'a':
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_FLOPPY;
                break;
            case 'd':
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_CDROM;
                break;
            case 'n':
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_NET;
                break;
            case 'c':
            default:
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_DISK;
                break;
            }
            def->os.nBootDevs++;
        }
    } else {
        if (xenXMConfigCopyStringOpt(conn, conf, "bootloader", &def->os.bootloader) < 0)
            goto cleanup;
        if (xenXMConfigCopyStringOpt(conn, conf, "bootargs", &def->os.bootloaderArgs) < 0)
            goto cleanup;

        if (xenXMConfigCopyStringOpt(conn, conf, "kernel", &def->os.kernel) < 0)
            goto cleanup;
        if (xenXMConfigCopyStringOpt(conn, conf, "ramdisk", &def->os.initrd) < 0)
            goto cleanup;
        if (xenXMConfigCopyStringOpt(conn, conf, "extra", &def->os.cmdline) < 0)
            goto cleanup;
    }

    if (xenXMConfigGetULong(conn, conf, "memory", &def->memory, MIN_XEN_GUEST_SIZE * 2) < 0)
        goto cleanup;

    if (xenXMConfigGetULong(conn, conf, "maxmem", &def->maxmem, def->memory) < 0)
        goto cleanup;

    def->memory *= 1024;
    def->maxmem *= 1024;


    if (xenXMConfigGetULong(conn, conf, "vcpus", &def->vcpus, 1) < 0)
        goto cleanup;

    if (xenXMConfigGetString(conn, conf, "cpus", &str, NULL) < 0)
        goto cleanup;
    if (str) {
        def->cpumasklen = 4096;
        if (VIR_ALLOC_N(def->cpumask, def->cpumasklen) < 0)
            goto no_memory;

        if (virDomainCpuSetParse(conn, &str, 0,
                                 def->cpumask, def->cpumasklen) < 0)
            goto cleanup;
    }


    if (xenXMConfigGetString(conn, conf, "on_poweroff", &str, "destroy") < 0)
        goto cleanup;
    if ((def->onPoweroff = virDomainLifecycleTypeFromString(str)) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unexpected value %s for on_poweroff"), str);
        goto cleanup;
    }

    if (xenXMConfigGetString(conn, conf, "on_reboot", &str, "restart") < 0)
        goto cleanup;
    if ((def->onReboot = virDomainLifecycleTypeFromString(str)) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unexpected value %s for on_reboot"), str);
        goto cleanup;
    }

    if (xenXMConfigGetString(conn, conf, "on_crash", &str, "restart") < 0)
        goto cleanup;
    if ((def->onCrash = virDomainLifecycleTypeFromString(str)) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unexpected value %s for on_crash"), str);
        goto cleanup;
    }



    if (hvm) {
        if (xenXMConfigGetBool(conn, conf, "pae", &val, 0) < 0)
            goto cleanup;
        else if (val)
            def->features |= (1 << VIR_DOMAIN_FEATURE_PAE);
        if (xenXMConfigGetBool(conn, conf, "acpi", &val, 0) < 0)
            goto cleanup;
        else if (val)
            def->features |= (1 << VIR_DOMAIN_FEATURE_ACPI);
        if (xenXMConfigGetBool(conn, conf, "apic", &val, 0) < 0)
            goto cleanup;
        else if (val)
            def->features |= (1 << VIR_DOMAIN_FEATURE_APIC);

        if (xenXMConfigGetBool(conn, conf, "localtime", &def->localtime, 0) < 0)
            goto cleanup;
    }
    if (xenXMConfigCopyStringOpt(conn, conf, "device_model", &def->emulator) < 0)
        goto cleanup;

    if (def->emulator == NULL) {
        const char *type = virDomainVirtTypeToString(def->virtType);
        if (!type) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       "%s", _("unknown virt type"));
            goto cleanup;
        }
        const char *emulator = virCapabilitiesDefaultGuestEmulator(priv->caps,
                                                                   def->os.type,
                                                                   def->os.arch,
                                                                   type);
        if (!emulator) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       "%s", _("unsupported guest type"));
            goto cleanup;
        }
        if (!(def->emulator = strdup(emulator)))
            goto no_memory;
    }

    list = virConfGetValue(conf, "disk");
    if (list && list->type == VIR_CONF_LIST) {
        list = list->list;
        while (list) {
            char *head;
            char *offset;
            char *tmp, *tmp1;

            if ((list->type != VIR_CONF_STRING) || (list->str == NULL))
                goto skipdisk;
            head = list->str;

            if (VIR_ALLOC(disk) < 0)
                goto no_memory;

            /*
             * Disks have 3 components, SOURCE,DEST-DEVICE,MODE
             * eg, phy:/dev/HostVG/XenGuest1,xvda,w
             * The SOURCE is usually prefixed with a driver type,
             * and optionally driver sub-type
             * The DEST-DEVICE is optionally post-fixed with disk type
             */

            /* Extract the source file path*/
            if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                goto skipdisk;
            if ((offset - head) >= (PATH_MAX-1))
                goto skipdisk;
            if (VIR_ALLOC_N(disk->src, (offset - head) + 1) < 0)
                goto no_memory;
            strncpy(disk->src, head, (offset - head));
            disk->src[(offset-head)] = '\0';
            head = offset + 1;

            /* Remove legacy ioemu: junk */
            if (STRPREFIX(head, "ioemu:"))
                head = head + 6;

            /* Extract the dest device name */
            if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                goto skipdisk;
            if (VIR_ALLOC_N(disk->dst, (offset - head) + 1) < 0)
                goto no_memory;
            strncpy(disk->dst, head, (offset - head));
            disk->dst[(offset-head)] = '\0';
            head = offset + 1;


            /* Extract source driver type */
            if (disk->src &&
                (tmp = strchr(disk->src, ':')) != NULL) {
                if (VIR_ALLOC_N(disk->driverName, (tmp - disk->src) + 1) < 0)
                    goto no_memory;
                strncpy(disk->driverName, disk->src, (tmp - disk->src));
                disk->driverName[tmp - disk->src] = '\0';
            } else {
                if (!(disk->driverName = strdup("phy")))
                    goto no_memory;
                tmp = disk->src;
            }

            /* And the source driver sub-type */
            if (STRPREFIX(disk->driverName, "tap")) {
                if (!(tmp1 = strchr(tmp+1, ':')) || !tmp1[0])
                    goto skipdisk;
                if (VIR_ALLOC_N(disk->driverType, (tmp1-(tmp+1))) < 0)
                    goto no_memory;
                strncpy(disk->driverType, tmp+1, (tmp1-(tmp+1)));
                memmove(disk->src, disk->src+(tmp1-disk->src)+1, strlen(disk->src)-(tmp1-disk->src));
            } else {
                disk->driverType = NULL;
                if (disk->src[0] && tmp)
                    memmove(disk->src, disk->src+(tmp-disk->src)+1, strlen(disk->src)-(tmp-disk->src));
            }

            /* phy: type indicates a block device */
            disk->type = STREQ(disk->driverName, "phy") ?
                VIR_DOMAIN_DISK_TYPE_BLOCK : VIR_DOMAIN_DISK_TYPE_FILE;

            /* Check for a :cdrom/:disk postfix */
            disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
            if ((tmp = strchr(disk->dst, ':')) != NULL) {
                if (STREQ(tmp, ":cdrom"))
                    disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
                tmp[0] = '\0';
            }

            if (STRPREFIX(disk->dst, "xvd") || !hvm) {
                disk->bus = VIR_DOMAIN_DISK_BUS_XEN;
            } else if (STRPREFIX(disk->dst, "sd")) {
                disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
            } else {
                disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
            }

            if (STREQ(head, "r") ||
                STREQ(head, "ro"))
                disk->readonly = 1;
            else if ((STREQ(head, "w!")) ||
                     (STREQ(head, "!")))
                disk->shared = 1;

            /* Maintain list in sorted order according to target device name */
            if (def->disks == NULL) {
                disk->next = def->disks;
                def->disks = disk;
            } else {
                virDomainDiskDefPtr ptr = def->disks;
                while (ptr) {
                    if (!ptr->next || xenXMDiskCompare(disk, ptr->next) < 0) {
                        disk->next = ptr->next;
                        ptr->next = disk;
                        break;
                    }
                    ptr = ptr->next;
                }
            }
            disk = NULL;

            skipdisk:
            list = list->next;
            virDomainDiskDefFree(disk);
        }
    }

    if (hvm && priv->xendConfigVersion == 1) {
        if (xenXMConfigGetString(conn, conf, "cdrom", &str, NULL) < 0)
            goto cleanup;
        if (str) {
            if (VIR_ALLOC(disk) < 0)
                goto no_memory;

            disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
            disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
            if (!(disk->driverName = strdup("file")))
                goto no_memory;
            if (!(disk->src = strdup(str)))
                goto no_memory;
            if (!(disk->dst = strdup("hdc")))
                goto no_memory;
            disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
            disk->readonly = 1;


            /* Maintain list in sorted order according to target device name */
            if (def->disks == NULL) {
                disk->next = def->disks;
                def->disks = disk;
            } else {
                virDomainDiskDefPtr ptr = def->disks;
                while (ptr) {
                    if (!ptr->next || xenXMDiskCompare(disk, ptr->next) < 0) {
                        disk->next = ptr->next;
                        ptr->next = disk;
                        break;
                    }
                    ptr = ptr->next;
                }
            }
            disk = NULL;
        }
    }

    list = virConfGetValue(conf, "vif");
    if (list && list->type == VIR_CONF_LIST) {
        list = list->list;
        while (list) {
            int type = -1;
            char script[PATH_MAX];
            char model[10];
            char ip[16];
            char mac[18];
            char bridge[50];
            char *key;

            bridge[0] = '\0';
            mac[0] = '\0';
            script[0] = '\0';
            ip[0] = '\0';
            model[0] = '\0';

            if ((list->type != VIR_CONF_STRING) || (list->str == NULL))
                goto skipnic;

            key = list->str;
            while (key) {
                char *data;
                char *nextkey = strchr(key, ',');

                if (!(data = strchr(key, '=')) || (data[0] == '\0'))
                    goto skipnic;
                data++;

                if (STRPREFIX(key, "mac=")) {
                    int len = nextkey ? (nextkey - data) : 17;
                    if (len > 17)
                        len = 17;
                    strncpy(mac, data, len);
                    mac[len] = '\0';
                } else if (STRPREFIX(key, "bridge=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(bridge)-1;
                    type = 1;
                    if (len > (sizeof(bridge)-1))
                        len = sizeof(bridge)-1;
                    strncpy(bridge, data, len);
                    bridge[len] = '\0';
                } else if (STRPREFIX(key, "script=")) {
                    int len = nextkey ? (nextkey - data) : PATH_MAX-1;
                    if (len > (PATH_MAX-1))
                        len = PATH_MAX-1;
                    strncpy(script, data, len);
                    script[len] = '\0';
                } else if (STRPREFIX(key, "model=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(model)-1;
                    if (len > (sizeof(model)-1))
                        len = sizeof(model)-1;
                    strncpy(model, data, len);
                    model[len] = '\0';
                } else if (STRPREFIX(key, "ip=")) {
                    int len = nextkey ? (nextkey - data) : 15;
                    if (len > 15)
                        len = 15;
                    strncpy(ip, data, len);
                    ip[len] = '\0';
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }

            /* XXX Forcing to pretend its a bridge */
            if (type == -1) {
                type = 1;
            }

            if (VIR_ALLOC(net) < 0)
                goto cleanup;

            if (mac[0]) {
                unsigned int rawmac[6];
                sscanf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                       (unsigned int*)&rawmac[0],
                       (unsigned int*)&rawmac[1],
                       (unsigned int*)&rawmac[2],
                       (unsigned int*)&rawmac[3],
                       (unsigned int*)&rawmac[4],
                       (unsigned int*)&rawmac[5]);
                net->mac[0] = rawmac[0];
                net->mac[1] = rawmac[1];
                net->mac[2] = rawmac[2];
                net->mac[3] = rawmac[3];
                net->mac[4] = rawmac[4];
                net->mac[5] = rawmac[5];
            }

            if (bridge[0] || STREQ(script, "vif-bridge"))
                net->type = VIR_DOMAIN_NET_TYPE_BRIDGE;
            else
                net->type = VIR_DOMAIN_NET_TYPE_ETHERNET;

            if (net->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
                if (bridge[0] &&
                    !(net->data.bridge.brname = strdup(bridge)))
                    goto no_memory;
            } else {
                if (script[0] &&
                    !(net->data.ethernet.script = strdup(script)))
                    goto no_memory;
                if (ip[0] &&
                    !(net->data.ethernet.ipaddr = strdup(ip)))
                    goto no_memory;
            }
            if (model[0] &&
                !(net->model = strdup(model)))
                goto no_memory;

            if (!def->nets) {
                net->next = NULL;
                def->nets = net;
            } else {
                virDomainNetDefPtr ptr = def->nets;
                while (ptr->next)
                    ptr = ptr->next;
                ptr->next = net;
            }
            net = NULL;

        skipnic:
            list = list->next;
            virDomainNetDefFree(net);
        }
    }

    if (hvm) {
        if (xenXMConfigGetString(conn, conf, "usbdevice", &str, NULL) < 0)
            goto cleanup;
        if (str &&
            (STREQ(str, "tablet") ||
             STREQ(str, "mouse"))) {
            virDomainInputDefPtr input;
            if (VIR_ALLOC(input) < 0)
                goto no_memory;
            input->bus = VIR_DOMAIN_INPUT_BUS_USB;
            input->type = STREQ(str, "tablet") ?
                VIR_DOMAIN_INPUT_TYPE_TABLET :
                VIR_DOMAIN_INPUT_TYPE_MOUSE;
            def->inputs = input;
        }
    }

    /* HVM guests, or old PV guests use this config format */
    if (hvm || priv->xendConfigVersion < 3) {
        if (xenXMConfigGetBool(conn, conf, "vnc", &val, 0) < 0)
            goto cleanup;

        if (val) {
            if (VIR_ALLOC(graphics) < 0)
                goto no_memory;
            graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;
            if (xenXMConfigGetBool(conn, conf, "vncunused", &val, 1) < 0)
                goto cleanup;
            graphics->data.vnc.autoport = val ? 1 : 0;

            if (!graphics->data.vnc.autoport) {
                unsigned long vncdisplay;
                if (xenXMConfigGetULong(conn, conf, "vncdisplay", &vncdisplay, 0) < 0)
                    goto cleanup;
                graphics->data.vnc.port = (int)vncdisplay + 5900;
            }
            if (xenXMConfigCopyStringOpt(conn, conf, "vnclisten", &graphics->data.vnc.listenAddr) < 0)
                goto cleanup;
            if (xenXMConfigCopyStringOpt(conn, conf, "vncpasswd", &graphics->data.vnc.passwd) < 0)
                goto cleanup;
            if (xenXMConfigCopyStringOpt(conn, conf, "keymap", &graphics->data.vnc.keymap) < 0)
                goto cleanup;

            def->graphics = graphics;
            graphics = NULL;
        } else {
            if (xenXMConfigGetBool(conn, conf, "sdl", &val, 0) < 0)
                goto cleanup;
            if (val) {
                if (VIR_ALLOC(graphics) < 0)
                    goto no_memory;
                graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
                if (xenXMConfigCopyStringOpt(conn, conf, "display", &graphics->data.sdl.display) < 0)
                    goto cleanup;
                if (xenXMConfigCopyStringOpt(conn, conf, "xauthority", &graphics->data.sdl.xauth) < 0)
                    goto cleanup;
                def->graphics = graphics;
                graphics = NULL;
            }
        }
    }

    if (!hvm && def->graphics == NULL) { /* New PV guests use this format */
        list = virConfGetValue(conf, "vfb");
        if (list && list->type == VIR_CONF_LIST &&
            list->list && list->list->type == VIR_CONF_STRING &&
            list->list->str) {
            char vfb[MAX_VFB];
            char *key = vfb;
            strncpy(vfb, list->list->str, MAX_VFB-1);
            vfb[MAX_VFB-1] = '\0';

            if (VIR_ALLOC(graphics) < 0)
                goto no_memory;

            if (strstr(key, "type=sdl"))
                graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
            else
                graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;

            while (key) {
                char *data;
                char *nextkey = strchr(key, ',');
                char *end = nextkey;
                if (nextkey) {
                    *end = '\0';
                    nextkey++;
                }

                if (!(data = strchr(key, '=')) || (data[0] == '\0'))
                    break;
                data++;

                if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
                    if (STRPREFIX(key, "vncunused=")) {
                        if (STREQ(key + 10, "1"))
                            graphics->data.vnc.autoport = 1;
                    } else if (STRPREFIX(key, "vnclisten=")) {
                        if (!(graphics->data.vnc.listenAddr = strdup(key + 10)))
                            goto no_memory;
                    } else if (STRPREFIX(key, "vncpasswd=")) {
                        if (!(graphics->data.vnc.passwd = strdup(key + 10)))
                            goto no_memory;
                    } else if (STRPREFIX(key, "keymap=")) {
                        if (!(graphics->data.vnc.keymap = strdup(key + 7)))
                            goto no_memory;
                    } else if (STRPREFIX(key, "vncdisplay=")) {
                        graphics->data.vnc.port = strtol(key+11, NULL, 10) + 5900;
                    }
                } else {
                    if (STRPREFIX(key, "display=")) {
                        if (!(graphics->data.sdl.display = strdup(key + 8)))
                            goto no_memory;
                    } else if (STRPREFIX(key, "xauthority=")) {
                        if (!(graphics->data.sdl.xauth = strdup(key + 11)))
                            goto no_memory;
                    }
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }
            def->graphics = graphics;
            graphics = NULL;
        }
    }

    if (hvm) {
        if (xenXMConfigGetString(conn, conf, "parallel", &str, NULL) < 0)
            goto cleanup;
        if (str && STRNEQ(str, "none") &&
            !(def->parallels = xenDaemonParseSxprChar(conn, str, NULL)))
            goto cleanup;

        if (xenXMConfigGetString(conn, conf, "serial", &str, NULL) < 0)
            goto cleanup;
        if (str && STRNEQ(str, "none") &&
            !(def->serials = xenDaemonParseSxprChar(conn, str, NULL)))
            goto cleanup;
    } else {
        if (!(def->console = xenDaemonParseSxprChar(conn, "pty", NULL)))
            goto cleanup;
    }

    if (hvm) {
        if (xenXMConfigGetString(conn, conf, "soundhw", &str, NULL) < 0)
            goto cleanup;

        if (str &&
            xenDaemonParseSxprSound(conn, def, str) < 0)
            goto cleanup;
    }

    return def;

no_memory:
    xenXMError(conn, VIR_ERR_NO_MEMORY, NULL);
    /* fallthrough */
  cleanup:
    virDomainGraphicsDefFree(graphics);
    virDomainNetDefFree(net);
    virDomainDiskDefFree(disk);
    virDomainDefFree(def);
    return NULL;
}


/*
 * Turn a config record into a lump of XML describing the
 * domain, suitable for later feeding for virDomainCreateLinux
 */
char *xenXMDomainDumpXML(virDomainPtr domain, int flags) {
    const char *filename;
    xenXMConfCachePtr entry;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return(NULL);
    }
    if (domain->id != -1)
        return (NULL);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (NULL);

    if (!(entry = virHashLookup(configCache, filename)))
        return (NULL);

    return virDomainDefFormat(domain->conn, entry->def, flags);
}


/*
 * Update amount of memory in the config file
 */
int xenXMDomainSetMemory(virDomainPtr domain, unsigned long memory) {
    const char *filename;
    xenXMConfCachePtr entry;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    entry->def->memory = memory;
    if (entry->def->memory > entry->def->maxmem)
        entry->def->memory = entry->def->maxmem;

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (xenXMConfigSaveFile(domain->conn, entry->filename, entry->def) < 0)
        return (-1);

    return (0);
}

/*
 * Update maximum memory limit in config
 */
int xenXMDomainSetMaxMemory(virDomainPtr domain, unsigned long memory) {
    const char *filename;
    xenXMConfCachePtr entry;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    entry->def->maxmem = memory;
    if (entry->def->memory > entry->def->maxmem)
        entry->def->memory = entry->def->maxmem;

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (xenXMConfigSaveFile(domain->conn, entry->filename, entry->def) < 0)
        return (-1);

    return (0);
}

/*
 * Get max memory limit from config
 */
unsigned long xenXMDomainGetMaxMemory(virDomainPtr domain) {
    const char *filename;
    xenXMConfCachePtr entry;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    return entry->def->maxmem;
}

/*
 * Set the VCPU count in config
 */
int xenXMDomainSetVcpus(virDomainPtr domain, unsigned int vcpus) {
    const char *filename;
    xenXMConfCachePtr entry;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    entry->def->vcpus = vcpus;

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (xenXMConfigSaveFile(domain->conn, entry->filename, entry->def) < 0)
        return (-1);

    return (0);
}

/**
 * xenXMDomainPinVcpu:
 * @domain: pointer to domain object
 * @vcpu: virtual CPU number (reserved)
 * @cpumap: pointer to a bit map of real CPUs (in 8-bit bytes)
 * @maplen: length of cpumap in bytes
 *
 * Set the vcpu affinity in config
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int xenXMDomainPinVcpu(virDomainPtr domain,
                       unsigned int vcpu ATTRIBUTE_UNUSED,
                       unsigned char *cpumap, int maplen)
{
    const char *filename;
    xenXMConfCachePtr entry;
    virBuffer mapbuf = VIR_BUFFER_INITIALIZER;
    char *mapstr = NULL;
    int i, j, n, comma = 0;
    int ret = -1;
    char *cpuset = NULL;
    int maxcpu = XEN_MAX_PHYSICAL_CPU;

    if (domain == NULL || domain->conn == NULL || domain->name == NULL
        || cpumap == NULL || maplen < 1 || maplen > (int)sizeof(cpumap_t)) {
        xenXMError(domain ? domain->conn : NULL, VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return -1;
    }
    if (domain->conn->flags & VIR_CONNECT_RO) {
        xenXMError (domain->conn, VIR_ERR_INVALID_ARG,
                    _("read only connection"));
        return -1;
    }
    if (domain->id != -1) {
        xenXMError (domain->conn, VIR_ERR_INVALID_ARG,
                    _("not inactive domain"));
        return -1;
    }

    if (!(filename = virHashLookup(nameConfigMap, domain->name))) {
        xenXMError (domain->conn, VIR_ERR_INTERNAL_ERROR, _("virHashLookup"));
        return -1;
    }
    if (!(entry = virHashLookup(configCache, filename))) {
        xenXMError (domain->conn, VIR_ERR_INTERNAL_ERROR,
                    _("can't retrieve config file for domain"));
        return -1;
    }

    /* from bit map, build character string of mapped CPU numbers */
    for (i = 0; i < maplen; i++)
        for (j = 0; j < 8; j++)
            if ((cpumap[i] & (1 << j))) {
                n = i*8 + j;

                if (comma)
                    virBufferAddLit (&mapbuf, ",");
                comma = 1;

                virBufferVSprintf (&mapbuf, "%d", n);
            }

    if (virBufferError(&mapbuf)) {
        xenXMError(domain->conn, VIR_ERR_NO_MEMORY, _("allocate buffer"));
        return -1;
    }

    mapstr = virBufferContentAndReset(&mapbuf);

    if (VIR_ALLOC_N(cpuset, maxcpu) < 0) {
        xenXMError(domain->conn, VIR_ERR_NO_MEMORY, _("allocate buffer"));
        goto cleanup;
    }
    if (virDomainCpuSetParse(domain->conn,
                             (const char **)&mapstr, 0,
                             cpuset, maxcpu) < 0)
        goto cleanup;

    VIR_FREE(entry->def->cpumask);
    entry->def->cpumask = cpuset;
    entry->def->cpumasklen = maxcpu;
    cpuset = NULL;

    if (xenXMConfigSaveFile(domain->conn, entry->filename, entry->def) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(mapstr);
    VIR_FREE(cpuset);
    return (ret);
}

/*
 * Find an inactive domain based on its name
 */
virDomainPtr xenXMDomainLookupByName(virConnectPtr conn, const char *domname) {
    const char *filename;
    xenXMConfCachePtr entry;
    virDomainPtr ret;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (domname == NULL) {
        xenXMError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (NULL);

    if (!(filename = virHashLookup(nameConfigMap, domname)))
        return (NULL);

    if (!(entry = virHashLookup(configCache, filename))) {
        return (NULL);
    }

    if (!(ret = virGetDomain(conn, domname, entry->def->uuid))) {
        return (NULL);
    }

    /* Ensure its marked inactive, because may be cached
       handle to a previously active domain */
    ret->id = -1;

    return (ret);
}


/*
 * Hash table iterator to search for a domain based on UUID
 */
static int xenXMDomainSearchForUUID(const void *payload, const char *name ATTRIBUTE_UNUSED, const void *data) {
    const unsigned char *wantuuid = (const unsigned char *)data;
    const xenXMConfCachePtr entry = (const xenXMConfCachePtr)payload;

    if (!memcmp(entry->def->uuid, wantuuid, VIR_UUID_BUFLEN))
        return (1);

    return (0);
}

/*
 * Find an inactive domain based on its UUID
 */
virDomainPtr xenXMDomainLookupByUUID(virConnectPtr conn,
                                     const unsigned char *uuid) {
    xenXMConfCachePtr entry;
    virDomainPtr ret;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (uuid == NULL) {
        xenXMError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (NULL);

    if (!(entry = virHashSearch(configCache, xenXMDomainSearchForUUID, (const void *)uuid))) {
        return (NULL);
    }

    if (!(ret = virGetDomain(conn, entry->def->name, uuid))) {
        return (NULL);
    }

    /* Ensure its marked inactive, because may be cached
       handle to a previously active domain */
    ret->id = -1;

    return (ret);
}


/*
 * Start a domain from an existing defined config file
 */
int xenXMDomainCreate(virDomainPtr domain) {
    char *sexpr;
    int ret;
    xenUnifiedPrivatePtr priv;
    const char *filename;
    xenXMConfCachePtr entry;

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (!(sexpr = xenDaemonFormatSxpr(domain->conn, entry->def, priv->xendConfigVersion))) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("failed to build sexpr"));
        return (-1);
    }

    ret = xenDaemonDomainCreateLinux(domain->conn, sexpr);
    VIR_FREE(sexpr);
    if (ret != 0) {
        return (-1);
    }

    if ((ret = xenDaemonDomainLookupByName_ids(domain->conn, domain->name,
                                               entry->def->uuid)) < 0) {
        return (-1);
    }
    domain->id = ret;

    if ((ret = xend_wait_for_devices(domain->conn, domain->name)) < 0)
        goto cleanup;

    if ((ret = xenDaemonDomainResume(domain)) < 0)
        goto cleanup;

    return (0);

 cleanup:
    if (domain->id != -1) {
        xenDaemonDomainDestroy(domain);
        domain->id = -1;
    }
    return (-1);
}


static
int xenXMConfigSetInt(virConfPtr conf, const char *setting, long l) {
    virConfValuePtr value = NULL;

    if (VIR_ALLOC(value) < 0)
        return -1;

    value->type = VIR_CONF_LONG;
    value->next = NULL;
    value->l = l;

    return virConfSetValue(conf, setting, value);
}


static
int xenXMConfigSetString(virConfPtr conf, const char *setting, const char *str) {
    virConfValuePtr value = NULL;

    if (VIR_ALLOC(value) < 0)
        return -1;

    value->type = VIR_CONF_STRING;
    value->next = NULL;
    if (!(value->str = strdup(str))) {
        VIR_FREE(value);
        return -1;
    }

    return virConfSetValue(conf, setting, value);
}


/*
 * Convenience method to set an int config param
 * based on an XPath expression
 */
static
int xenXMConfigSetIntFromXPath(virConnectPtr conn,
                               virConfPtr conf, xmlXPathContextPtr ctxt,
                               const char *setting, const char *xpath,
                               long scale, int allowMissing, const char *error) {
    xmlXPathObjectPtr obj;
    long intval;
    char *strend;
    int ret = -1;

    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        if (allowMissing)
            ret = 0;
        else
            xenXMError(conn, VIR_ERR_XML_ERROR, error);
        goto error;
    }

    intval = strtol((char *)obj->stringval, &strend, 10);
    if (strend == (char *)obj->stringval) {
        xenXMError(conn, VIR_ERR_XML_ERROR, error);
        goto error;
    }

    if (scale > 0)
        intval = intval * scale;
    else if (scale < 0)
        intval = intval / (-1*scale);

    if (xenXMConfigSetInt(conf, setting, intval) < 0)
        goto error;

    ret = 0;

 error:
    xmlXPathFreeObject(obj);

    return ret;
}

/*
 * Convenience method to set a string param
 * based on an XPath expression
 */
static
int xenXMConfigSetStringFromXPath(virConnectPtr conn,
                                  virConfPtr conf, xmlXPathContextPtr ctxt,
                                  const char *setting, const char *xpath,
                                  int allowMissing, const char *error) {
    xmlXPathObjectPtr obj;
    int ret = -1;

    obj = xmlXPathEval(BAD_CAST xpath, ctxt);

    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        if (allowMissing)
            ret = 0;
        else
            xenXMError(conn, VIR_ERR_XML_ERROR, error);
        goto error;
    }

    if (xenXMConfigSetString(conf, setting, (const char *)obj->stringval) < 0)
        goto error;

    ret = 0;

 error:
    xmlXPathFreeObject(obj);

    return ret;
}

static int xenXMParseXMLDisk(xmlNodePtr node, int hvm, int xendConfigVersion, char **disk) {
    xmlNodePtr cur;
    xmlChar *type = NULL;
    xmlChar *device = NULL;
    xmlChar *source = NULL;
    xmlChar *target = NULL;
    xmlChar *drvName = NULL;
    xmlChar *drvType = NULL;
    int readonly = 0;
    int shareable = 0;
    int typ = 0;
    int cdrom = 0;
    int ret = -1;
    int buflen = 0;
    char *buf = NULL;

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "file"))
            typ = 0;
        else if (xmlStrEqual(type, BAD_CAST "block"))
            typ = 1;
        xmlFree(type);
    }
    device = xmlGetProp(node, BAD_CAST "device");

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {

                if (typ == 0)
                    source = xmlGetProp(cur, BAD_CAST "file");
                else
                    source = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((target == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                target = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((drvName == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "driver"))) {
                drvName = xmlGetProp(cur, BAD_CAST "name");
                if (drvName && STREQ((const char *)drvName, "tap"))
                    drvType = xmlGetProp(cur, BAD_CAST "type");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                readonly = 1;
            } else if (xmlStrEqual(cur->name, BAD_CAST "shareable")) {
                shareable = 1;
            }
        }
        cur = cur->next;
    }

    if (target == NULL) {
        xmlFree(source);
        xmlFree(device);
        return (-1);
    }

    /* Xend (all versions) put the floppy device config
     * under the hvm (image (os)) block
     */
    if (hvm &&
        device &&
        STREQ((const char *)device, "floppy")) {
        ret = 0;
        goto cleanup;
    }

    /* Xend <= 3.0.2 doesn't include cdrom config here */
    if (hvm &&
        device &&
        STREQ((const char *)device, "cdrom")) {
        if (xendConfigVersion == 1) {
            ret = 0;
            goto cleanup;
        } else {
            cdrom = 1;
        }
    }

    if (source == NULL && !cdrom) {
        xmlFree(target);
        xmlFree(device);
        return (-1);
    }

    if (drvName) {
        buflen += strlen((const char*)drvName) + 1;
        if (STREQ((const char*)drvName, "tap")) {
            if (drvType)
                buflen += strlen((const char*)drvType) + 1;
            else
                buflen += 4;
        }
    } else {
        if (typ == 0)
            buflen += 5;
        else
            buflen += 4;
    }

    if(source)
        buflen += strlen((const char*)source) + 1;
    else
        buflen += 1;
    buflen += strlen((const char*)target) + 1;
    if (hvm && xendConfigVersion == 1) /* ioemu: */
        buflen += 6;

    if (cdrom) /* :cdrom */
        buflen += 6;

    buflen += 2; /* mode */

    if (VIR_ALLOC_N(buf, buflen) < 0)
        goto cleanup;

    if(source) {
        if (drvName) {
            strcpy(buf, (const char*)drvName);
            if (STREQ((const char*)drvName, "tap")) {
                strcat(buf, ":");
                if (drvType)
                    strcat(buf, (const char*)drvType);
                else
                    strcat(buf, "aio");
            }
        } else {
            if (typ == 0)
                strcpy(buf, "file");
            else
                strcpy(buf, "phy");
        }
        strcat(buf, ":");
        strcat(buf, (const char*)source);
    } else {
        strcpy(buf, "");
    }
    strcat(buf, ",");
    if (hvm && xendConfigVersion == 1)
        strcat(buf, "ioemu:");
    strcat(buf, (const char*)target);
    if (cdrom)
        strcat(buf, ":cdrom");

    if (readonly)
        strcat(buf, ",r");
    else if (shareable)
        strcat(buf, ",!");
    else
        strcat(buf, ",w");
    ret = 0;
 cleanup:
    xmlFree(drvType);
    xmlFree(drvName);
    xmlFree(device);
    xmlFree(target);
    xmlFree(source);
    *disk = buf;

    return (ret);
}
static char *xenXMParseXMLVif(virConnectPtr conn, xmlNodePtr node, int hvm) {
    xmlNodePtr cur;
    xmlChar *type = NULL;
    xmlChar *source = NULL;
    xmlChar *mac = NULL;
    xmlChar *script = NULL;
    xmlChar *model = NULL;
    xmlChar *ip = NULL;
    int typ = 0;
    char *buf = NULL;
    int buflen = 0;
    char *bridge = NULL;

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "bridge"))
            typ = 0;
        else if (xmlStrEqual(type, BAD_CAST "ethernet"))
            typ = 1;
        else if (xmlStrEqual(type, BAD_CAST "network"))
            typ = 2;
        xmlFree(type);
    }
    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {

                if (typ == 0)
                    source = xmlGetProp(cur, BAD_CAST "bridge");
                else if (typ == 1)
                    source = xmlGetProp(cur, BAD_CAST "dev");
                else
                    source = xmlGetProp(cur, BAD_CAST "network");
            } else if ((mac == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "mac"))) {
                mac = xmlGetProp(cur, BAD_CAST "address");
            } else if ((model == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "model"))) {
                model = xmlGetProp(cur, BAD_CAST "type");
            } else if ((ip == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "ip"))) {
                ip = xmlGetProp(cur, BAD_CAST "address");
            } else if ((script == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "script"))) {
                script = xmlGetProp(cur, BAD_CAST "path");
            }
        }
        cur = cur->next;
    }

    if (!mac) {
        goto cleanup;
    }
    buflen += 5 + strlen((const char *)mac);
    if (source) {
        if (typ == 0) {
            buflen += 8 + strlen((const char *)source);
        } else if (typ == 1) {
            buflen += 5 + strlen((const char *)source);
        } else {
            virNetworkPtr network = virNetworkLookupByName(conn, (const char *) source);
            if (!network || !(bridge = virNetworkGetBridgeName(network))) {
                if (network)
                    virNetworkFree(network);
                goto cleanup;
            }
            virNetworkFree(network);
            buflen += 8 + strlen(bridge);
        }
    }
    if (hvm)
        buflen += 11;
    if (script)
        buflen += 8 + strlen((const char*)script);
    if (model)
        buflen += 7 + strlen((const char*)model);
    if (ip)
        buflen += 4 + strlen((const char*)ip);

    if (VIR_ALLOC_N(buf, buflen) < 0)
        goto cleanup;

    strcpy(buf, "mac=");
    strcat(buf, (const char*)mac);
    if (source) {
        if (typ == 0) {
            strcat(buf, ",bridge=");
            strcat(buf, (const char*)source);
        } else if (typ == 1) {
            strcat(buf, ",dev=");
            strcat(buf, (const char*)source);
        } else {
            strcat(buf, ",bridge=");
            strcat(buf, bridge);
        }
    }
    if (hvm) {
        strcat(buf, ",type=ioemu");
    }
    if (script) {
        strcat(buf, ",script=");
        strcat(buf, (const char*)script);
    }
    if (model) {
        strcat(buf, ",model=");
        strcat(buf, (const char*)model);
    }
    if (ip) {
        strcat(buf, ",ip=");
        strcat(buf, (const char*)ip);
    }

 cleanup:
    VIR_FREE(bridge);
    xmlFree(mac);
    xmlFree(source);
    xmlFree(script);
    xmlFree(ip);
    xmlFree(model);

    return buf;
}


virConfPtr xenXMDomainConfigFormat(virConnectPtr conn,
                                   virDomainDefPtr def) {
    xmlDocPtr doc = NULL;
    xmlNodePtr node;
    xmlXPathObjectPtr obj = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlChar *prop = NULL;
    virConfPtr conf = NULL;
    int hvm = 0, i;
    xenUnifiedPrivatePtr priv;
    char *cpus;
    char *xml;

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (!(xml = virDomainDefFormat(conn, def, VIR_DOMAIN_XML_SECURE)))
        return NULL;

    doc = xmlReadDoc((const xmlChar *) xml, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    VIR_FREE(xml);
    if (doc == NULL) {
        xenXMError(conn, VIR_ERR_XML_ERROR,
                   _("cannot read XML domain definition"));
        return (NULL);
    }
    node = xmlDocGetRootElement(doc);
    if ((node == NULL) || (!xmlStrEqual(node->name, BAD_CAST "domain"))) {
        xenXMError(conn, VIR_ERR_XML_ERROR,
                   _("missing top level domain element"));
        goto error;
    }

    prop = xmlGetProp(node, BAD_CAST "type");
    if (prop != NULL) {
        if (!xmlStrEqual(prop, BAD_CAST "xen")) {
            xenXMError(conn, VIR_ERR_XML_ERROR,
                       _("domain type is invalid"));
            goto error;
        }
        xmlFree(prop);
        prop = NULL;
    }
    if (!(ctxt = xmlXPathNewContext(doc))) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("cannot create XPath context"));
        goto error;
    }
    if (!(conf = virConfNew()))
        goto error;


    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "name", "string(/domain/name)", 0,
                                      "domain name element missing") < 0)
        goto error;

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "uuid", "string(/domain/uuid)", 0,
                                      "domain uuid element missing") < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "maxmem", "string(/domain/memory)", -1024, 0,
                                   "domain memory element missing") < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "memory", "string(/domain/memory)", -1024, 0,
                                   "domain memory element missing") < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "memory", "string(/domain/currentMemory)", -1024, 1,
                                   "domain currentMemory element missing") < 0)
        goto error;

    if (xenXMConfigSetInt(conf, "vcpus", 1) < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "vcpus", "string(/domain/vcpu)", 0, 1,
                                   "cannot set vcpus config parameter") < 0)
        goto error;

    cpus = virXPathString("string(/domain/vcpu/@cpuset)", ctxt);
    if (cpus != NULL) {
        char *ranges;

        ranges = virConvertCpuSet(conn, cpus, 0);
        VIR_FREE(cpus);
        if (ranges == NULL) {
            goto error;
        }
        if (xenXMConfigSetString(conf, "cpus", ranges) < 0) {
            VIR_FREE(ranges);
            goto error;
        }
        VIR_FREE(ranges);
    }

    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type)", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && STREQ((char*)obj->stringval, "hvm"))
        hvm = 1;
    xmlXPathFreeObject(obj);
    obj = NULL;

    if (hvm) {
        const char *boot = "c";
        int clockLocal = 0;
        if (xenXMConfigSetString(conf, "builder", "hvm") < 0)
            goto error;

        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "kernel", "string(/domain/os/loader)", 1,
                                          "cannot set the os loader parameter") < 0)
            goto error;

        obj = xmlXPathEval(BAD_CAST "string(/domain/os/boot/@dev)", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_STRING) &&
            (obj->stringval != NULL)) {
            if (STREQ((const char*)obj->stringval, "fd"))
                boot = "a";
            else if (STREQ((const char*)obj->stringval, "hd"))
                boot = "c";
            else if (STREQ((const char*)obj->stringval, "cdrom"))
                boot = "d";
        }
        xmlXPathFreeObject(obj);
        obj = NULL;
        if (xenXMConfigSetString(conf, "boot", boot) < 0)
            goto error;

        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "pae", "string(count(/domain/features/pae))", 0, 0,
                                       "cannot set the pae parameter") < 0)
            goto error;

        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "acpi", "string(count(/domain/features/acpi))", 0, 0,
                                       "cannot set the acpi parameter") < 0)
            goto error;

        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "apic", "string(count(/domain/features/apic))", 0, 0,
                                       "cannot set the apic parameter") < 0)
            goto error;

        obj = xmlXPathEval(BAD_CAST "string(/domain/clock/@offset)", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_STRING) &&
            (obj->stringval != NULL)) {
            if (STREQ((const char*)obj->stringval, "localtime"))
                clockLocal = 1;
        }
        xmlXPathFreeObject(obj);
        obj = NULL;
        if (xenXMConfigSetInt(conf, "localtime", clockLocal) < 0)
            goto error;

        if (priv->xendConfigVersion == 1) {
            if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "cdrom", "string(/domain/devices/disk[@device='cdrom']/source/@file)", 1,
                                              "cannot set the cdrom parameter") < 0)
                goto error;
        }
    } else {
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "bootloader", "string(/domain/bootloader)", 1,
                                          "cannot set the bootloader parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "bootargs", "string(/domain/bootloader_args)", 1,
                                          "cannot set the bootloader_args parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "kernel", "string(/domain/os/kernel)", 1,
                                          "cannot set the kernel parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "ramdisk", "string(/domain/os/initrd)", 1,
                                          "cannot set the ramdisk parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "extra", "string(/domain/os/cmdline)", 1,
                                          "cannot set the cmdline parameter") < 0)
            goto error;

    }

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "on_poweroff", "string(/domain/on_poweroff)", 1,
                                      "cannot set the on_poweroff parameter") < 0)
        goto error;

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "on_reboot", "string(/domain/on_reboot)", 1,
                                      "cannot set the on_reboot parameter") < 0)
        goto error;

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "on_crash", "string(/domain/on_crash)", 1,
                                      "cannot set the on_crash parameter") < 0)
        goto error;


    if (hvm) {
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "device_model", "string(/domain/devices/emulator)", 1,
                                          "cannot set the device_model parameter") < 0)
            goto error;

        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "usbdevice", "string(/domain/devices/input[@bus='usb' or (not(@bus) and @type='tablet')]/@type)", 1,
                                          "cannot set the usbdevice parameter") < 0)
            goto error;
    }

    if (hvm || priv->xendConfigVersion < 3) {
        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "sdl", "string(count(/domain/devices/graphics[@type='sdl']))", 0, 0,
                                       "cannot set the sdl parameter") < 0)
            goto error;
        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "vnc", "string(count(/domain/devices/graphics[@type='vnc']))", 0, 0,
                                       "cannot set the vnc parameter") < 0)
            goto error;
        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "vncunused", "string(count(/domain/devices/graphics[@type='vnc' and @port='-1']))", 0, 0,
                                       "cannot set the vncunused parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "vnclisten", "string(/domain/devices/graphics[@type='vnc']/@listen)", 1,
                                          "cannot set the vnclisten parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "vncpasswd", "string(/domain/devices/graphics[@type='vnc']/@passwd)", 1,
                                          "cannot set the vncpasswd parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "keymap", "string(/domain/devices/graphics[@type='vnc']/@keymap)", 1,
                                          "cannot set the keymap parameter") < 0)
            goto error;

        obj = xmlXPathEval(BAD_CAST "string(/domain/devices/graphics[@type='vnc']/@port)", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_STRING) &&
            (obj->stringval != NULL)) {
            int port = strtol((const char *)obj->stringval, NULL, 10);
            if (port != -1) {
                char portstr[50];
                snprintf(portstr, sizeof(portstr), "%d", port-5900);
                if (xenXMConfigSetString(conf, "vncdisplay", portstr) < 0)
                    goto error;
            }
        }
        xmlXPathFreeObject(obj);
    } else {
        virConfValuePtr vfb;
        obj = xmlXPathEval(BAD_CAST "/domain/devices/graphics", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
            (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
            if (VIR_ALLOC(vfb) < 0) {
                xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                goto error;
            }
            vfb->type = VIR_CONF_LIST;
            vfb->list = NULL;
            for (i = obj->nodesetval->nodeNr -1 ; i >= 0 ; i--) {
                xmlChar *type;
                char *val = NULL;

                if (!(type = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "type"))) {
                    continue;
                }
                if (STREQ((const char*)type, "sdl")) {
                    val = strdup("type=sdl");
                } else if (STREQ((const char*)type, "vnc")) {
                    int len = 8 + 1; /* type=vnc & NULL */
                    xmlChar *vncport = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "port");
                    xmlChar *vnclisten = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "listen");
                    xmlChar *vncpasswd = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "passwd");
                    xmlChar *keymap = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "keymap");
                    int vncunused = vncport ? (STREQ((const char*)vncport, "-1") ? 1 : 0) : 1;
                    if (vncunused)
                        len += 12;
                    else
                        len += 12 + strlen((const char*)vncport);/* vncdisplay= */
                    if (vnclisten)
                        len += 11 + strlen((const char*)vnclisten);
                    if (vncpasswd)
                        len += 11 + strlen((const char*)vncpasswd);
                    if (keymap)
                        len += 8 + strlen((const char*)keymap);
                    if (VIR_ALLOC_N(val, len) < 0) {
                        xmlFree(type);
                        xmlFree(vncport);
                        xmlFree(vnclisten);
                        xmlFree(vncpasswd);
                        xmlFree(keymap);
                        VIR_FREE(vfb);
                        xenXMError (conn, VIR_ERR_NO_MEMORY, strerror (errno));
                        goto error;
                    }
                    strcpy(val, "type=vnc");
                    if (vncunused) {
                        strcat(val, ",vncunused=1");
                    } else {
                        char portstr[50];
                        int port = atoi((const char*)vncport);
                        snprintf(portstr, sizeof(portstr), "%d", port-5900);
                        strcat(val, ",vncdisplay=");
                        strcat(val, portstr);
                    }
                    xmlFree(vncport);
                    if (vnclisten) {
                        strcat(val, ",vnclisten=");
                        strcat(val, (const char*)vnclisten);
                        xmlFree(vnclisten);
                    }
                    if (vncpasswd) {
                        strcat(val, ",vncpasswd=");
                        strcat(val, (const char*)vncpasswd);
                        xmlFree(vncpasswd);
                    }
                    if (keymap) {
                        strcat(val, ",keymap=");
                        strcat(val, (const char*)keymap);
                        xmlFree(keymap);
                    }
                }
                xmlFree(type);
                if (val) {
                    virConfValuePtr disp;
                    if (VIR_ALLOC(disp) < 0) {
                        VIR_FREE(val);
                        VIR_FREE(vfb);
                        xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                        goto error;
                    }
                    disp->type = VIR_CONF_STRING;
                    disp->str = val;
                    disp->next = vfb->list;
                    vfb->list = disp;
                }
            }
            if (virConfSetValue(conf, "vfb", vfb) < 0)
                goto error;
        }
        xmlXPathFreeObject(obj);
    }

    /* analyze of the devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/disk", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        virConfValuePtr disks;
        if (VIR_ALLOC(disks) < 0) {
            xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
            goto error;
        }
        disks->type = VIR_CONF_LIST;
        disks->list = NULL;
        for (i = obj->nodesetval->nodeNr -1 ; i >= 0 ; i--) {
            virConfValuePtr thisDisk;
            char *disk = NULL;
            if (xenXMParseXMLDisk(obj->nodesetval->nodeTab[i], hvm, priv->xendConfigVersion, &disk) < 0) {
                virConfFreeValue(disks);
                goto error;
            }
            if (disk) {
                if (VIR_ALLOC(thisDisk) < 0) {
                    VIR_FREE(disk);
                    virConfFreeValue(disks);
                    xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                    goto error;
                }
                thisDisk->type = VIR_CONF_STRING;
                thisDisk->str = disk;
                thisDisk->next = disks->list;
                disks->list = thisDisk;
            }
        }
        if (virConfSetValue(conf, "disk", disks) < 0)
            goto error;
    }
    xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "/domain/devices/interface", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        virConfValuePtr vifs;
        if (VIR_ALLOC(vifs) < 0) {
            xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
            goto error;
        }
        vifs->type = VIR_CONF_LIST;
        vifs->list = NULL;
        for (i = obj->nodesetval->nodeNr - 1; i >= 0; i--) {
            virConfValuePtr thisVif;
            char *vif = xenXMParseXMLVif(conn, obj->nodesetval->nodeTab[i], hvm);
            if (!vif) {
                virConfFreeValue(vifs);
                goto error;
            }
            if (VIR_ALLOC(thisVif) < 0) {
                VIR_FREE(vif);
                virConfFreeValue(vifs);
                xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                goto error;
            }
            thisVif->type = VIR_CONF_STRING;
            thisVif->str = vif;
            thisVif->next = vifs->list;
            vifs->list = thisVif;
        }
        if (virConfSetValue(conf, "vif", vifs) < 0)
            goto error;
    }
    xmlXPathFreeObject(obj);
    obj = NULL;

    if (hvm) {
        xmlNodePtr cur;
        cur = virXPathNode("/domain/devices/parallel[1]", ctxt);
        if (cur != NULL) {
            char scratch[PATH_MAX];

            if (virDomainParseXMLOSDescHVMChar(conn, scratch, sizeof(scratch), cur) < 0) {
                goto error;
            }

            if (xenXMConfigSetString(conf, "parallel", scratch) < 0)
                goto error;
        } else {
            if (xenXMConfigSetString(conf, "parallel", "none") < 0)
                goto error;
        }

        cur = virXPathNode("/domain/devices/serial[1]", ctxt);
        if (cur != NULL) {
            char scratch[PATH_MAX];
            if (virDomainParseXMLOSDescHVMChar(conn, scratch, sizeof(scratch), cur) < 0)
                goto error;
            if (xenXMConfigSetString(conf, "serial", scratch) < 0)
                goto error;
        } else {
            if (virXPathBoolean("count(/domain/devices/console) > 0", ctxt)) {
                if (xenXMConfigSetString(conf, "serial", "pty") < 0)
                    goto error;
            } else {
                if (xenXMConfigSetString(conf, "serial", "none") < 0)
                    goto error;
            }
        }

        if (virXPathNode("/domain/devices/sound", ctxt)) {
            char *soundstr;
            if (!(soundstr = virBuildSoundStringFromXML(conn, ctxt)))
                goto error;
            if (xenXMConfigSetString(conf, "soundhw", soundstr) < 0) {
                VIR_FREE(soundstr);
                goto error;
            }
            VIR_FREE(soundstr);
        }
    }

    xmlFreeDoc(doc);
    xmlXPathFreeContext(ctxt);

    return conf;

 error:
    if (conf)
        virConfFree(conf);
    xmlFree(prop);
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (doc != NULL)
        xmlFreeDoc(doc);
    return (NULL);
}

/*
 * Create a config file for a domain, based on an XML
 * document describing its config
 */
virDomainPtr xenXMDomainDefineXML(virConnectPtr conn, const char *xml) {
    virDomainPtr ret;
    virDomainPtr olddomain;
    char filename[PATH_MAX];
    const char * oldfilename;
    virDomainDefPtr def = NULL;
    xenXMConfCachePtr entry = NULL;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (xml == NULL) {
        xenXMError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }
    if (conn->flags & VIR_CONNECT_RO)
        return (NULL);

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (NULL);

    if (!(def = virDomainDefParseString(conn, priv->caps, xml)))
        return (NULL);

    if (virHashLookup(nameConfigMap, def->name)) {
        /* domain exists, we will overwrite it */

        if (!(oldfilename = (char *)virHashLookup(nameConfigMap, def->name))) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("can't retrieve config filename for domain to overwrite"));
            goto error;
        }

        if (!(entry = virHashLookup(configCache, oldfilename))) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("can't retrieve config entry for domain to overwrite"));
            goto error;
        }

        /* XXX wtf.com is this line for - it appears to be amemory leak */
        if (!(olddomain = virGetDomain(conn, def->name, entry->def->uuid)))
            goto error;

        /* Remove the name -> filename mapping */
        if (virHashRemoveEntry(nameConfigMap, def->name, NULL) < 0) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("failed to remove old domain from config map"));
            goto error;
        }

        /* Remove the config record itself */
        if (virHashRemoveEntry(configCache, oldfilename, xenXMConfigFree) < 0) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("failed to remove old domain from config map"));
            goto error;
        }

        entry = NULL;
    }

    if ((strlen(configDir) + 1 + strlen(def->name) + 1) > PATH_MAX) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config file name is too long"));
        goto error;
    }

    strcpy(filename, configDir);
    strcat(filename, "/");
    strcat(filename, def->name);

    if (xenXMConfigSaveFile(conn, filename, def) < 0)
        goto error;

    if (VIR_ALLOC(entry) < 0) {
        xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
        goto error;
    }

    if ((entry->refreshedAt = time(NULL)) == ((time_t)-1)) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to get current time"));
        goto error;
    }

    memmove(entry->filename, filename, PATH_MAX);
    entry->def = def;

    if (virHashAddEntry(configCache, filename, entry) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to store config file handle"));
        goto error;
    }

    if (virHashAddEntry(nameConfigMap, def->name, entry->filename) < 0) {
        virHashRemoveEntry(configCache, filename, NULL);
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to store config file handle"));
        goto error;
    }

    if (!(ret = virGetDomain(conn, def->name, def->uuid)))
        return NULL;

    ret->id = -1;

    return (ret);

 error:
    VIR_FREE(entry);
    virDomainDefFree(def);
    return (NULL);
}

/*
 * Delete a domain from disk
 */
int xenXMDomainUndefine(virDomainPtr domain) {
    const char *filename;
    xenXMConfCachePtr entry;
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }

    if (domain->id != -1)
        return (-1);
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (unlink(entry->filename) < 0)
        return (-1);

    /* Remove the name -> filename mapping */
    if (virHashRemoveEntry(nameConfigMap, domain->name, NULL) < 0)
        return(-1);

    /* Remove the config record itself */
    if (virHashRemoveEntry(configCache, entry->filename, xenXMConfigFree) < 0)
        return (-1);

    return (0);
}

struct xenXMListIteratorContext {
    virConnectPtr conn;
    int max;
    int count;
    char ** names;
};

static void xenXMListIterator(const void *payload ATTRIBUTE_UNUSED, const char *name, const void *data) {
    struct xenXMListIteratorContext *ctx = (struct xenXMListIteratorContext *)data;
    virDomainPtr dom = NULL;

    if (ctx->count == ctx->max)
        return;

    dom = xenDaemonLookupByName(ctx->conn, name);
    if (!dom) {
        ctx->names[ctx->count] = strdup(name);
        ctx->count++;
    } else {
        virDomainFree(dom);
    }
}


/*
 * List all defined domains, filtered to remove any which
 * are currently running
 */
int xenXMListDefinedDomains(virConnectPtr conn, char **const names, int maxnames) {
    struct xenXMListIteratorContext ctx;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (-1);

    if (maxnames > virHashSize(configCache))
        maxnames = virHashSize(configCache);

    ctx.conn = conn;
    ctx.count = 0;
    ctx.max = maxnames;
    ctx.names = names;

    virHashForEach(nameConfigMap, xenXMListIterator, &ctx);
    return (ctx.count);
}

/*
 * Return the maximum number of defined domains - not filtered
 * based on number running
 */
int xenXMNumOfDefinedDomains(virConnectPtr conn) {
    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (-1);

    return virHashSize(nameConfigMap);
}

static int xenXMDiskCompare(virDomainDiskDefPtr a,
                            virDomainDiskDefPtr b) {
    if (a->bus == b->bus)
        return virDiskNameToIndex(a->dst) - virDiskNameToIndex(b->dst);
    else
        return a->bus - b->bus;
}


/**
 * xenXMDomainAttachDevice:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 *
 * Create a virtual device attachment to backend.
 * XML description is translated into config file.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenXMDomainAttachDevice(virDomainPtr domain, const char *xml) {
    const char *filename = NULL;
    xenXMConfCachePtr entry = NULL;
    int ret = -1;
    virDomainDeviceDefPtr dev = NULL;

    if ((!domain) || (!domain->conn) || (!domain->name) || (!xml)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return -1;
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return -1;
    if (domain->id != -1)
        return -1;

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return -1;
    if (!(entry = virHashLookup(configCache, filename)))
        return -1;

    if (!(dev = virDomainDeviceDefParse(domain->conn,
                                        entry->def,
                                        xml)))
        return -1;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
    {
        /* Maintain list in sorted order according to target device name */
        if (entry->def->disks == NULL) {
            dev->data.disk->next = entry->def->disks;
            entry->def->disks = dev->data.disk;
        } else {
            virDomainDiskDefPtr ptr = entry->def->disks;
            while (ptr) {
                if (!ptr->next || xenXMDiskCompare(dev->data.disk, ptr->next) < 0) {
                    dev->data.disk->next = ptr->next;
                    ptr->next = dev->data.disk;
                    break;
                }
                ptr = ptr->next;
            }
        }
        dev->data.disk = NULL;
    }
    break;

    case VIR_DOMAIN_DEVICE_NET:
    {
        virDomainNetDefPtr net = entry->def->nets;
        while (net && net->next)
            net = net->next;
        if (net)
            net->next = dev->data.net;
        else
            entry->def->nets = dev->data.net;
        dev->data.net = NULL;
        break;
    }

    default:
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("unknown device"));
        goto cleanup;
    }

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (xenXMConfigSaveFile(domain->conn, entry->filename, entry->def) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virDomainDeviceDefFree(dev);

    return ret;
}


/**
 * xenXMAutoAssignMac:
 * @mac: pointer to Mac String
 *
 * a mac is assigned automatically.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
char *
xenXMAutoAssignMac() {
    char *buf;

    if (VIR_ALLOC_N(buf, 18) < 0)
        return 0;
    srand((unsigned)time(NULL));
    sprintf(buf, "00:16:3e:%02x:%02x:%02x"
            ,1 + (int)(256*(rand()/(RAND_MAX+1.0)))
            ,1 + (int)(256*(rand()/(RAND_MAX+1.0)))
            ,1 + (int)(256*(rand()/(RAND_MAX+1.0))));
    return buf;
}

/**
 * xenXMDomainDetachDevice:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 *
 * Destroy a virtual device attachment to backend.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenXMDomainDetachDevice(virDomainPtr domain, const char *xml) {
    const char *filename = NULL;
    xenXMConfCachePtr entry = NULL;
    virDomainDeviceDefPtr dev = NULL;
    int ret = -1;

    if ((!domain) || (!domain->conn) || (!domain->name) || (!xml)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return -1;
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return -1;
    if (domain->id != -1)
        return -1;
    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return -1;
    if (!(entry = virHashLookup(configCache, filename)))
        return -1;

    if (!(dev = virDomainDeviceDefParse(domain->conn,
                                        entry->def,
                                        xml)))
        return -1;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
    {
        virDomainDiskDefPtr disk = entry->def->disks;
        virDomainDiskDefPtr prev = NULL;
        while (disk) {
            if (disk->dst &&
                dev->data.disk->dst &&
                STREQ(disk->dst, dev->data.disk->dst)) {
                if (prev) {
                    prev->next = disk->next;
                } else {
                    entry->def->disks = disk->next;
                }
                virDomainDiskDefFree(disk);
                break;
            }
            prev = disk;
            disk = disk->next;
        }
        break;
    }

    case VIR_DOMAIN_DEVICE_NET:
    {
        virDomainNetDefPtr net = entry->def->nets;
        virDomainNetDefPtr prev = NULL;
        while (net) {
            if (!memcmp(net->mac, dev->data.net->mac, VIR_DOMAIN_NET_MAC_SIZE)) {
                if (prev) {
                    prev->next = net->next;
                } else {
                    entry->def->nets = net->next;
                }
                virDomainNetDefFree(net);
                break;
            }
            prev = net;
            net = net->next;
        }
        break;
    }
    default:
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("unknown device"));
        goto cleanup;
    }

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (xenXMConfigSaveFile(domain->conn, entry->filename, entry->def) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virDomainDeviceDefFree(dev);
    return (ret);
}

int
xenXMDomainBlockPeek (virDomainPtr dom,
                      const char *path ATTRIBUTE_UNUSED,
                      unsigned long long offset ATTRIBUTE_UNUSED,
                      size_t size ATTRIBUTE_UNUSED,
                      void *buffer ATTRIBUTE_UNUSED)
{
    xenXMError (dom->conn, VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

#endif /* WITH_XEN */
