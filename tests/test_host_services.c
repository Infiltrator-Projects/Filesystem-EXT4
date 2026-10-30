/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only, filesystem-neutral host services for canonical EXT.so tests.
 * No EXT layout, parsing, allocation or relocation code belongs here.
 * Destructive device operations deliberately fail closed in these tests.
 */
#include "ifs_defrag_services_v1.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int ifs_bind_services(const IfsDefragServicesV1 *);

static ssize_t test_read(int fd, void *out, size_t len, uint64_t offset) {
    if (offset > (uint64_t)INT64_MAX) { errno = EOVERFLOW; return -1; }
    size_t n = 0;
    while (n < len) {
        ssize_t got = pread(fd, (char *)out + n, len - n, (off_t)(offset + n));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        n += (size_t)got;
    }
    return (ssize_t)n;
}
static ssize_t test_write(int fd, const void *in, size_t len, uint64_t offset) {
    if (offset > (uint64_t)INT64_MAX) { errno = EOVERFLOW; return -1; }
    size_t n = 0;
    while (n < len) {
        ssize_t wrote = pwrite(fd, (const char *)in + n, len - n, (off_t)(offset + n));
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return -1;
        n += (size_t)wrote;
    }
    return (ssize_t)n;
}
static bool test_bitmap_size(uint64_t bits, size_t *bytes) {
    if (!bytes || bits > (uint64_t)SIZE_MAX * 8U) return false;
    *bytes = (size_t)(bits / 8U + (bits % 8U != 0U));
    return true;
}
static uint8_t *test_bitmap_calloc(uint64_t bits) {
    size_t n;
    return test_bitmap_size(bits, &n) ? calloc(n ? n : 1U, 1U) : NULL;
}
static bool test_bitmap_get(const uint8_t *p, uint64_t bit) {
    return p && (p[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}
static void test_bitmap_set(uint8_t *p, uint64_t bit, bool set) {
    if (!p) abort();
    uint8_t mask = (uint8_t)(1U << (bit % 8U));
    if (set) p[bit / 8U] |= mask; else p[bit / 8U] &= (uint8_t)~mask;
}
static int test_size(int fd, uint64_t *bytes) {
    struct stat st;
    if (!bytes || fstat(fd, &st) != 0 || st.st_size < 0) return -1;
    *bytes = (uint64_t)st.st_size;
    return 0;
}
static int deny_binding(const char *p, char **c, char **i, uint64_t *n) {
    (void)p; (void)c; (void)i; (void)n; errno=EPERM; return -1;
}
static int deny_verified(const char *p, bool w, const char *i, uint64_t n) {
    (void)p;(void)w;(void)i;(void)n; errno=EPERM; return -1;
}
static bool always_mounted(const char *p) { (void)p; return true; }
static char *deny_parent(const char *p) { (void)p; errno=EPERM; return NULL; }
static char *deny_suffix(const char *p, const char *s) { (void)p;(void)s; errno=EPERM; return NULL; }
static bool deny_derived(const char *a, const char *b, const char *s) {
    (void)a;(void)b;(void)s; return false;
}
static int deny_directory(const char *p) { (void)p; errno=EPERM; return -1; }
static int deny_space(const char *p,uint64_t n,uint64_t *a,uint64_t *r) {
    (void)p;(void)n;(void)a;(void)r;errno=EPERM;return -1;
}
static char *test_strdup(const char *s) { return s ? strdup(s) : NULL; }
static char *test_strndup(const char *s,size_t n) { return s ? strndup(s,n) : NULL; }
static int deny_event(FILE *f,const char *o,const char *s,const char *m) {
    (void)f;(void)o;(void)s;(void)m;return -1;
}
static bool not_stopped(void) { return false; }
static void no_op(void) {}
static void test_die(const char *s) {
    fprintf(stderr,"EXT test host fatal: %s\n",s ? s : "(null)");
    abort();
}
static bool mounted_dev(dev_t d) { (void)d; return true; }
static bool deny_identity_fd(int fd,const char *id,uint64_t n) {
    (void)fd;(void)id;(void)n;return false;
}
static LdDevice deny_open(const char *p,bool w) {
    (void)p;(void)w;test_die("device opened outside test fixture");
    abort();
}
static void deny_close(LdDevice *d) { (void)d; }
static void *test_xcalloc(size_t n,size_t bytes) {
    if (bytes && n > SIZE_MAX/bytes) test_die("allocation overflow");
    void *p=calloc(n ? n : 1U,bytes ? bytes : 1U);
    if (!p) test_die("allocation failure");
    return p;
}
static int deny_device_try(const char *p,bool w,LdDevice *d) {
    (void)p;(void)w;(void)d;errno=EPERM;return -1;
}
static int deny_format(const LdDevice *d,char *b,size_t n) {
    (void)d;(void)b;(void)n;errno=EPERM;return -1;
}
static bool deny_identity(const LdDevice *d,const char *i,uint64_t n) {
    (void)d;(void)i;(void)n;return false;
}
void ext_test_bind_services(void) {
    static const IfsDefragServicesV1 service={
        .abi_version=IFS_DEFRAG_SERVICES_V1,
        .structure_bytes=sizeof(IfsDefragServicesV1),
        .ld_pread_full=test_read,.ld_pwrite_full=test_write,
        .ld_bitmap_size=test_bitmap_size,.ld_bitmap_calloc=test_bitmap_calloc,
        .ld_bitmap_get=test_bitmap_get,.ld_bitmap_set=test_bitmap_set,
        .ld_sync_fd=fsync,.ld_fd_size_bytes=test_size,
        .ld_device_capture_binding=deny_binding,.ld_device_open_verified_fd=deny_verified,
        .ld_path_is_mounted=always_mounted,.ld_path_parent_directory=deny_parent,
        .ld_path_append_suffix=deny_suffix,.ld_path_is_derived_from=deny_derived,
        .ld_path_ensure_trusted_directory_tree=deny_directory,
        .ld_path_require_staging_capacity=deny_space,
        .ld_xstrdup=test_strdup,.ld_emit_result_event=deny_event,
        .ld_stop_requested=not_stopped,.ld_stop_clear=no_op,
        .ld_stop_install_handlers=no_op,.ld_stop_report_ready=no_op,
        .ld_die=test_die,.ld_xstrndup=test_strndup,
        .ld_device_number_is_mounted=mounted_dev,
        .ld_fd_matches_identity=deny_identity_fd,
        .ld_device_open=deny_open,.ld_device_close=deny_close,
        .ld_xcalloc=test_xcalloc,.ld_device_try_open=deny_device_try,
        .ld_device_format_identity=deny_format,
        .ld_device_matches_identity=deny_identity
    };
    if (ifs_bind_services(&service)!=0) test_die("cannot bind host callbacks");
}
