#define _POSIX_C_SOURCE 200809L
/* macOS hides mkdtemp in strict POSIX mode; Linux ignores this. */
#define _DARWIN_C_SOURCE

#include "storage/log_writer.h"
#include "storage/record.h"
#include "../src/storage/log_writer_sys.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * Two kinds of test. Every record is built by the test; none is a
 * measurement, and no sensor is used.
 * - FILE: real syscalls against a fresh directory from mkdtemp, removed at
 *   the end. These check what actually reaches the file.
 * - SIMULATED: a fake syscall table scripts failures and records every
 *   call, so each error path runs deterministically.
 */

#define NS_PER_SEC UINT64_C(1000000000)
#define PATH_CAP 512
#define FAKE_DIR_FD 50
#define FAKE_FILE_FD 51
#define FAKE_MAX_CLOSES 4
#define FAKE_WRITTEN_CAP (4 * STORAGE_LOG_BATCH_BYTES)

/* ---------- shared helpers ---------- */

/* test_record: a valid light record with sequence seq; value tracks seq. */
static storage_record test_record(uint64_t seq, int simulated)
{
    storage_record r;

    memset(&r, 0, sizeof r);
    r.record_type = STORAGE_RECORD_TYPE_LIGHT;
    r.flags = STORAGE_FLAG_VALID | STORAGE_FLAG_CLOCK_UNTRUSTED;
    if (simulated)
        r.flags |= STORAGE_FLAG_SIMULATED;
    r.sequence = seq;
    r.wall_clock_ns = seq * NS_PER_SEC;
    r.monotonic_ns = seq * NS_PER_SEC + 7;
    r.sensor_value = (uint32_t)(seq * 1000u);
    r.source_sequence = (uint32_t)seq;
    return r;
}

/* check_bytes_hold: bytes hold n records with sequences first, first+1... */
static void check_bytes_hold(const uint8_t *bytes, size_t len_bytes,
                             uint64_t first, size_t n, int simulated)
{
    size_t i;

    assert(len_bytes == n * STORAGE_RECORD_SIZE_BYTES);
    for (i = 0; i < n; i++) {
        storage_record out;
        storage_record want = test_record(first + i, simulated);

        assert(storage_record_decode(bytes + i * STORAGE_RECORD_SIZE_BYTES,
                                     &out) == STORAGE_RECORD_OK);
        assert(out.sequence == want.sequence);
        assert(out.flags == want.flags);
        assert(out.sensor_value == want.sensor_value);
        assert(out.monotonic_ns == want.monotonic_ns);
    }
}

/* expect: assert a result's status, stage, and errno. */
static void expect(storage_log_result r, storage_log_status status,
                   storage_log_stage stage, int os_errno)
{
    assert(r.status == status);
    assert(r.stage == stage);
    assert(r.os_errno == os_errno);
}

/* expect_ok: assert a result is STORAGE_LOG_OK. */
static void expect_ok(storage_log_result r)
{
    expect(r, STORAGE_LOG_OK, STORAGE_LOG_STAGE_NONE, 0);
}

/* fd_is_closed: nonzero if fd is no longer an open descriptor. */
static int fd_is_closed(int fd)
{
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

/* ---------- FILE tests: real syscalls in a temporary directory ---------- */

static char temp_dir[PATH_CAP];

/* make_temp_dir: create a fresh, empty directory for one test. */
static void make_temp_dir(void)
{
    const char *base = getenv("TMPDIR");
    int n;

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    n = snprintf(temp_dir, sizeof temp_dir, "%s/storage-log-test.XXXXXX",
                 base);
    assert(n > 0 && (size_t)n < sizeof temp_dir);
    assert(mkdtemp(temp_dir) != NULL);
}

/* file_path: write temp_dir/name into buf. */
static void file_path(char *buf, size_t size, const char *name)
{
    int n = snprintf(buf, size, "%s/%s", temp_dir, name);

    assert(n > 0 && (size_t)n < size);
}

/* remove_temp_file: delete temp_dir/name. */
static void remove_temp_file(const char *name)
{
    char path[PATH_CAP];

    file_path(path, sizeof path, name);
    assert(unlink(path) == 0);
}

/* remove_temp_dir: delete the (now empty) temporary directory. */
static void remove_temp_dir(void)
{
    assert(rmdir(temp_dir) == 0);
}

/* read_file: read all of temp_dir/name into buf; return its length. */
static size_t read_file(const char *name, uint8_t *buf, size_t cap)
{
    char path[PATH_CAP];
    FILE *f;
    size_t n;

    file_path(path, sizeof path, name);
    f = fopen(path, "rb");
    assert(f != NULL);
    n = fread(buf, 1, cap, f);
    assert(!ferror(f));
    assert(fgetc(f) == EOF);  /* the whole file fit in buf */
    assert(fclose(f) == 0);
    return n;
}

/* write_file: replace temp_dir/name with len_bytes of buf. */
static void write_file(const char *name, const uint8_t *buf, size_t len)
{
    char path[PATH_CAP];
    FILE *f;

    file_path(path, sizeof path, name);
    f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(buf, 1, len, f) == len);
    assert(fclose(f) == 0);
}

/* file_size: size of temp_dir/name in bytes. */
static off_t file_size(const char *name)
{
    char path[PATH_CAP];
    struct stat st;

    file_path(path, sizeof path, name);
    assert(stat(path, &st) == 0);
    return st.st_size;
}

/* FILE: appends stay in memory until flush, then reach the file in order. */
static void test_file_flush_writes_in_order(void)
{
    static storage_log_writer w;
    uint8_t buf[FAKE_WRITTEN_CAP];
    storage_record r;
    uint64_t seq;
    int fd;

    make_temp_dir();
    expect_ok(storage_log_open(&w, temp_dir, "a.log", 10, 0));
    assert(w.fd >= 0);
    assert(w.file_size_bytes == 0);
    assert(file_size("a.log") == 0);

    for (seq = 10; seq < 13; seq++) {
        r = test_record(seq, 0);
        expect_ok(storage_log_append(&w, &r, seq));
    }
    assert(w.buffered_records == 3);
    assert(file_size("a.log") == 0);

    expect_ok(storage_log_flush(&w));
    assert(w.buffered_records == 0);
    assert(w.file_size_bytes == 3 * STORAGE_RECORD_SIZE_BYTES);
    check_bytes_hold(buf, read_file("a.log", buf, sizeof buf), 10, 3, 0);

    fd = w.fd;
    expect_ok(storage_log_close(&w));
    assert(w.fd == -1);
    assert(fd_is_closed(fd));

    remove_temp_file("a.log");
    remove_temp_dir();
}

/* FILE: the 32nd record writes the whole batch; the 31st does not. */
static void test_file_full_batch_writes(void)
{
    static storage_log_writer w;
    uint8_t buf[FAKE_WRITTEN_CAP];
    storage_record r;
    uint64_t seq;

    make_temp_dir();
    expect_ok(storage_log_open(&w, temp_dir, "b.log", 0, 1));
    for (seq = 0; seq < STORAGE_LOG_BATCH_RECORDS - 1; seq++) {
        r = test_record(seq, 1);
        expect_ok(storage_log_append(&w, &r, 0));
    }
    assert(file_size("b.log") == 0);

    r = test_record(STORAGE_LOG_BATCH_RECORDS - 1, 1);
    expect_ok(storage_log_append(&w, &r, 0));
    assert(w.buffered_records == 0);
    assert(file_size("b.log") == STORAGE_LOG_BATCH_BYTES);
    check_bytes_hold(buf, read_file("b.log", buf, sizeof buf), 0,
                     STORAGE_LOG_BATCH_RECORDS, 1);

    expect_ok(storage_log_close(&w));
    remove_temp_file("b.log");
    remove_temp_dir();
}

/* FILE: flush_if_due waits 60 s from the oldest buffered record. */
static void test_file_flush_if_due(void)
{
    static storage_log_writer w;
    const uint64_t t0 = 5 * NS_PER_SEC;
    storage_record r;

    make_temp_dir();
    expect_ok(storage_log_open(&w, temp_dir, "c.log", 1, 0));

    /* Nothing buffered: never due. */
    expect_ok(storage_log_flush_if_due(&w, UINT64_MAX));

    r = test_record(1, 0);
    expect_ok(storage_log_append(&w, &r, t0));
    r = test_record(2, 0);
    expect_ok(storage_log_append(&w, &r, t0 + 30 * NS_PER_SEC));
    assert(w.oldest_buffered_ns == t0);

    expect_ok(storage_log_flush_if_due(&w, t0 + 60 * NS_PER_SEC - 1));
    assert(file_size("c.log") == 0);
    /* A clock reading before the oldest record is never due. */
    expect_ok(storage_log_flush_if_due(&w, t0 - 1));
    assert(file_size("c.log") == 0);

    expect_ok(storage_log_flush_if_due(&w, t0 + 60 * NS_PER_SEC));
    assert(w.buffered_records == 0);
    assert(file_size("c.log") == 2 * STORAGE_RECORD_SIZE_BYTES);

    /* The deadline restarts from the next record after a flush. */
    r = test_record(3, 0);
    expect_ok(storage_log_append(&w, &r, t0 + 100 * NS_PER_SEC));
    expect_ok(storage_log_flush_if_due(&w, t0 + 159 * NS_PER_SEC));
    assert(w.buffered_records == 1);
    expect_ok(storage_log_flush_if_due(&w, t0 + 160 * NS_PER_SEC));
    assert(w.buffered_records == 0);

    expect_ok(storage_log_close(&w));
    remove_temp_file("c.log");
    remove_temp_dir();
}

/* FILE: close writes what is still buffered. */
static void test_file_close_flushes(void)
{
    static storage_log_writer w;
    uint8_t buf[FAKE_WRITTEN_CAP];
    storage_record r = test_record(4, 0);

    make_temp_dir();
    expect_ok(storage_log_open(&w, temp_dir, "d.log", 4, 0));
    expect_ok(storage_log_append(&w, &r, 0));
    expect_ok(storage_log_close(&w));
    check_bytes_hold(buf, read_file("d.log", buf, sizeof buf), 4, 1, 0);

    remove_temp_file("d.log");
    remove_temp_dir();
}

/* FILE: reopening an aligned file appends after its existing records. */
static void test_file_reopen_appends(void)
{
    static storage_log_writer w;
    uint8_t buf[FAKE_WRITTEN_CAP];
    storage_record r;

    make_temp_dir();
    expect_ok(storage_log_open(&w, temp_dir, "e.log", 0, 0));
    r = test_record(0, 0);
    expect_ok(storage_log_append(&w, &r, 0));
    r = test_record(1, 0);
    expect_ok(storage_log_append(&w, &r, 0));
    expect_ok(storage_log_close(&w));

    expect_ok(storage_log_open(&w, temp_dir, "e.log", 2, 0));
    assert(w.file_size_bytes == 2 * STORAGE_RECORD_SIZE_BYTES);
    r = test_record(2, 0);
    expect_ok(storage_log_append(&w, &r, 0));
    expect_ok(storage_log_close(&w));

    check_bytes_hold(buf, read_file("e.log", buf, sizeof buf), 0, 3, 0);
    remove_temp_file("e.log");
    remove_temp_dir();
}

/* FILE: a file with a partial record is refused and left unchanged. */
static void test_file_misaligned_refused(void)
{
    static storage_log_writer w;
    uint8_t partial[STORAGE_RECORD_SIZE_BYTES + 5];
    uint8_t after[sizeof partial + 1];

    make_temp_dir();
    memset(partial, 0xA5, sizeof partial);
    write_file("f.log", partial, sizeof partial);

    expect(storage_log_open(&w, temp_dir, "f.log", 0, 0),
           STORAGE_LOG_MISALIGNED, STORAGE_LOG_STAGE_NONE, 0);
    assert(w.fd == -1);
    assert(read_file("f.log", after, sizeof after) == sizeof partial);
    assert(memcmp(after, partial, sizeof partial) == 0);

    remove_temp_file("f.log");
    remove_temp_dir();
}

/* FILE: a missing directory is reported with its errno. */
static void test_file_missing_dir(void)
{
    static storage_log_writer w;
    char missing[PATH_CAP];

    make_temp_dir();
    file_path(missing, sizeof missing, "no-such-dir");
    expect(storage_log_open(&w, missing, "g.log", 0, 0),
           STORAGE_LOG_OS_ERROR, STORAGE_LOG_STAGE_OPEN_DIR, ENOENT);
    assert(w.fd == -1);
    remove_temp_dir();
}

/* FILE: bad records are rejected before anything is buffered. */
static void test_file_rejects_bad_records(void)
{
    static storage_log_writer w;
    storage_record r;

    make_temp_dir();
    expect_ok(storage_log_open(&w, temp_dir, "h.log", 7, 0));

    r = test_record(8, 0);  /* skips 7 */
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_SEQUENCE,
           STORAGE_LOG_STAGE_NONE, 0);
    r = test_record(6, 0);  /* goes back */
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_SEQUENCE,
           STORAGE_LOG_STAGE_NONE, 0);

    r = test_record(7, 1);  /* SIMULATED record into a live file */
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_SIMULATED_MISMATCH,
           STORAGE_LOG_STAGE_NONE, 0);

    r = test_record(7, 0);
    r.record_type = 0;
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_BAD_RECORD,
           STORAGE_LOG_STAGE_NONE, 0);
    r.record_type = STORAGE_RECORD_TYPE_START + 1;
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_BAD_RECORD,
           STORAGE_LOG_STAGE_NONE, 0);
    r = test_record(7, 0);
    r.flags |= UINT16_C(0x0010);  /* reserved bit 4 */
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_BAD_RECORD,
           STORAGE_LOG_STAGE_NONE, 0);

    assert(w.buffered_records == 0);
    assert(w.next_sequence == 7);

    /* The rejected attempts did not consume the sequence number. */
    r = test_record(7, 0);
    r.record_type = STORAGE_RECORD_TYPE_START;
    expect_ok(storage_log_append(&w, &r, 0));
    assert(w.next_sequence == 8);
    expect_ok(storage_log_close(&w));
    assert(file_size("h.log") == STORAGE_RECORD_SIZE_BYTES);

    /* A simulated file refuses a record without the SIMULATED flag. */
    expect_ok(storage_log_open(&w, temp_dir, "i.log", 0, 1));
    r = test_record(0, 0);
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_SIMULATED_MISMATCH,
           STORAGE_LOG_STAGE_NONE, 0);
    expect_ok(storage_log_close(&w));
    assert(file_size("i.log") == 0);

    remove_temp_file("h.log");
    remove_temp_file("i.log");
    remove_temp_dir();
}

/* FILE: NULL arguments and use after close are rejected. */
static void test_file_arguments(void)
{
    static storage_log_writer w;
    storage_record r = test_record(0, 0);

    make_temp_dir();
    expect(storage_log_open(NULL, temp_dir, "j.log", 0, 0),
           STORAGE_LOG_ARGUMENT, STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_open(&w, NULL, "j.log", 0, 0),
           STORAGE_LOG_ARGUMENT, STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_open(&w, temp_dir, NULL, 0, 0),
           STORAGE_LOG_ARGUMENT, STORAGE_LOG_STAGE_NONE, 0);

    expect_ok(storage_log_open(&w, temp_dir, "j.log", 0, 0));
    expect(storage_log_append(NULL, &r, 0), STORAGE_LOG_ARGUMENT,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_append(&w, NULL, 0), STORAGE_LOG_ARGUMENT,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_flush(NULL), STORAGE_LOG_ARGUMENT,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_flush_if_due(NULL, 0), STORAGE_LOG_ARGUMENT,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_close(NULL), STORAGE_LOG_ARGUMENT,
           STORAGE_LOG_STAGE_NONE, 0);
    expect_ok(storage_log_close(&w));

    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_NOT_OPEN,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_flush(&w), STORAGE_LOG_NOT_OPEN,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_flush_if_due(&w, 0), STORAGE_LOG_NOT_OPEN,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_close(&w), STORAGE_LOG_NOT_OPEN,
           STORAGE_LOG_STAGE_NONE, 0);

    remove_temp_file("j.log");
    remove_temp_dir();
}

/* ---------- SIMULATED tests: scripted syscall failures ---------- */

/* Fake syscall state: scripted results, then a record of each call. */
typedef struct {
    int open_dir_errno;     /* nonzero: open_dir fails with this errno */
    int open_file_errno;    /* nonzero: open_append_at fails */
    int fstat_errno;        /* nonzero: fstat fails */
    off_t existing_bytes;   /* size fstat reports */
    int fsync_errno;        /* nonzero: fsync (of the directory) fails */
    int write_eintr_count;  /* first n writes fail with EINTR */
    size_t write_chunk;     /* nonzero: each write accepts at most this */
    int write_errno;        /* nonzero: writes fail after the EINTRs */
    size_t write_errno_after;  /* ...once this many bytes were accepted */
    int fdatasync_errno;    /* nonzero: fdatasync fails */
    int close_fail_fd;      /* close of this fd fails with close_errno */
    int close_errno;

    unsigned open_dir_calls;
    unsigned open_file_calls;
    unsigned fstat_calls;
    unsigned write_calls;
    unsigned fdatasync_calls;
    unsigned fsync_calls;
    unsigned close_calls;
    int open_file_dir_fd;
    int fsync_fd;
    int fdatasync_fd;
    int closed_fds[FAKE_MAX_CLOSES];
    uint8_t written[FAKE_WRITTEN_CAP];
    size_t written_len;
} fake_state;

static fake_state fake;

/* fake_reset: clear all scripted results and call records. */
static void fake_reset(void)
{
    memset(&fake, 0, sizeof fake);
    fake.close_fail_fd = -1;
}

/* fake_open_dir: hand out FAKE_DIR_FD, or fail as scripted. */
static int fake_open_dir(const char *path)
{
    (void)path;
    fake.open_dir_calls++;
    if (fake.open_dir_errno != 0) {
        errno = fake.open_dir_errno;
        return -1;
    }
    return FAKE_DIR_FD;
}

/* fake_open_append_at: hand out FAKE_FILE_FD, or fail as scripted. */
static int fake_open_append_at(int dir_fd, const char *name)
{
    (void)name;
    fake.open_file_calls++;
    fake.open_file_dir_fd = dir_fd;
    if (fake.open_file_errno != 0) {
        errno = fake.open_file_errno;
        return -1;
    }
    return FAKE_FILE_FD;
}

/* fake_fstat: report existing_bytes, or fail as scripted. */
static int fake_fstat(int fd, struct stat *st)
{
    (void)fd;
    fake.fstat_calls++;
    if (fake.fstat_errno != 0) {
        errno = fake.fstat_errno;
        return -1;
    }
    memset(st, 0, sizeof *st);
    st->st_size = fake.existing_bytes;
    return 0;
}

/* fake_write: EINTR, fail, or accept up to write_chunk bytes. */
static ssize_t fake_write(int fd, const void *buf, size_t len)
{
    assert(fd == FAKE_FILE_FD);
    fake.write_calls++;
    if (fake.write_eintr_count > 0) {
        fake.write_eintr_count--;
        errno = EINTR;
        return -1;
    }
    if (fake.write_errno != 0 &&
        fake.written_len >= fake.write_errno_after) {
        errno = fake.write_errno;
        return -1;
    }
    if (fake.write_chunk != 0 && len > fake.write_chunk)
        len = fake.write_chunk;
    assert(fake.written_len + len <= sizeof fake.written);
    memcpy(fake.written + fake.written_len, buf, len);
    fake.written_len += len;
    return (ssize_t)len;
}

/* fake_fdatasync: record the descriptor; fail as scripted. */
static int fake_fdatasync(int fd)
{
    fake.fdatasync_calls++;
    fake.fdatasync_fd = fd;
    if (fake.fdatasync_errno != 0) {
        errno = fake.fdatasync_errno;
        return -1;
    }
    return 0;
}

/* fake_fsync: record the descriptor; fail as scripted. */
static int fake_fsync(int fd)
{
    fake.fsync_calls++;
    fake.fsync_fd = fd;
    if (fake.fsync_errno != 0) {
        errno = fake.fsync_errno;
        return -1;
    }
    return 0;
}

/* fake_close: record the descriptor; fail for close_fail_fd. */
static int fake_close(int fd)
{
    assert(fake.close_calls < FAKE_MAX_CLOSES);
    fake.closed_fds[fake.close_calls++] = fd;
    if (fd == fake.close_fail_fd) {
        errno = fake.close_errno;
        return -1;
    }
    return 0;
}

static const storage_log_sys fake_sys = {
    fake_open_dir, fake_open_append_at, fake_fstat, fake_write,
    fake_fdatasync, fake_fsync, fake_close
};

/* open_fake: open through the fake table. */
static storage_log_result open_fake(storage_log_writer *w, uint64_t first)
{
    return storage_log_open_sys(w, "/fake-dir", "fake.log", first, 0,
                                &fake_sys);
}

/* append_n: append n records starting at first; each must succeed. */
static void append_n(storage_log_writer *w, uint64_t first, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        storage_record r = test_record(first + i, 0);

        expect_ok(storage_log_append(w, &r, 0));
    }
}

/* SIMULATED: a successful open makes its calls in order, then closes the
 * directory and keeps only the file open. */
static void test_sim_open_sequence(void)
{
    static storage_log_writer w;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    assert(fake.open_dir_calls == 1);
    assert(fake.open_file_calls == 1);
    assert(fake.open_file_dir_fd == FAKE_DIR_FD);
    assert(fake.fstat_calls == 1);
    assert(fake.fsync_calls == 1 && fake.fsync_fd == FAKE_DIR_FD);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_DIR_FD);
    assert(w.fd == FAKE_FILE_FD);

    expect_ok(storage_log_close(&w));
    assert(fake.write_calls == 0);      /* empty batch: no write */
    assert(fake.fdatasync_calls == 0);  /* and no sync */
    assert(fake.close_calls == 2 && fake.closed_fds[1] == FAKE_FILE_FD);
}

/* SIMULATED: every open failure closes exactly what was opened. */
static void test_sim_open_failures(void)
{
    static storage_log_writer w;

    fake_reset();
    fake.open_dir_errno = EACCES;
    expect(open_fake(&w, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_OPEN_DIR, EACCES);
    assert(w.fd == -1 && fake.close_calls == 0);

    fake_reset();
    fake.open_file_errno = ENOSPC;
    expect(open_fake(&w, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_OPEN_FILE, ENOSPC);
    assert(w.fd == -1);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_DIR_FD);

    fake_reset();
    fake.fstat_errno = EIO;
    expect(open_fake(&w, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_STAT, EIO);
    assert(w.fd == -1);
    assert(fake.close_calls == 2);
    assert(fake.closed_fds[0] == FAKE_FILE_FD);
    assert(fake.closed_fds[1] == FAKE_DIR_FD);

    fake_reset();
    fake.existing_bytes = STORAGE_RECORD_SIZE_BYTES - 1;
    expect(open_fake(&w, 0), STORAGE_LOG_MISALIGNED,
           STORAGE_LOG_STAGE_NONE, 0);
    assert(w.fd == -1 && fake.close_calls == 2);
    assert(fake.fsync_calls == 0);

    fake_reset();
    fake.fsync_errno = EIO;
    expect(open_fake(&w, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_SYNC_DIR, EIO);
    assert(w.fd == -1 && fake.close_calls == 2);

    fake_reset();
    fake.close_fail_fd = FAKE_DIR_FD;
    fake.close_errno = EIO;
    expect(open_fake(&w, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_CLOSE_DIR, EIO);
    assert(w.fd == -1);
    assert(fake.close_calls == 2);
    assert(fake.closed_fds[0] == FAKE_DIR_FD);
    assert(fake.closed_fds[1] == FAKE_FILE_FD);

    /* An earlier error is reported even if a cleanup close also fails. */
    fake_reset();
    fake.fstat_errno = EIO;
    fake.close_fail_fd = FAKE_DIR_FD;
    fake.close_errno = EBADF;
    expect(open_fake(&w, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_STAT, EIO);
    assert(fake.close_calls == 2);
}

/* SIMULATED: EINTR is retried and short writes continue where they
 * stopped, so the file gets every byte once, in order. */
static void test_sim_eintr_and_short_writes(void)
{
    static storage_log_writer w;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    fake.write_eintr_count = 2;
    fake.write_chunk = 7;  /* not a divisor of 48 */
    append_n(&w, 0, 5);
    expect_ok(storage_log_flush(&w));
    check_bytes_hold(fake.written, fake.written_len, 0, 5, 0);
    assert(fake.write_calls ==
           2 + (5 * STORAGE_RECORD_SIZE_BYTES + 6) / 7);
    assert(fake.fdatasync_calls == 1);
    assert(fake.fdatasync_fd == FAKE_FILE_FD);
    assert(w.file_size_bytes == 5 * STORAGE_RECORD_SIZE_BYTES);
    expect_ok(storage_log_close(&w));
}

/* SIMULATED: a write error is reported once, then the writer stays
 * failed, never retries, and close still releases the descriptor. */
static void test_sim_write_failure_is_sticky(void)
{
    static storage_log_writer w;
    storage_record r;
    unsigned writes;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    append_n(&w, 0, STORAGE_LOG_BATCH_RECORDS - 1);
    fake.write_errno = ENOSPC;
    r = test_record(STORAGE_LOG_BATCH_RECORDS - 1, 0);
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_WRITE, ENOSPC);
    assert(w.failed);
    assert(fake.fdatasync_calls == 0);
    writes = fake.write_calls;

    fake.write_errno = 0;  /* even if the disk recovers */
    r = test_record(STORAGE_LOG_BATCH_RECORDS, 0);
    expect(storage_log_append(&w, &r, 0), STORAGE_LOG_FAILED,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_flush(&w), STORAGE_LOG_FAILED,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_flush_if_due(&w, UINT64_MAX), STORAGE_LOG_FAILED,
           STORAGE_LOG_STAGE_NONE, 0);
    expect(storage_log_close(&w), STORAGE_LOG_FAILED,
           STORAGE_LOG_STAGE_NONE, 0);
    assert(fake.write_calls == writes);
    assert(fake.written_len == 0);
    assert(w.fd == -1);
    assert(fake.closed_fds[fake.close_calls - 1] == FAKE_FILE_FD);
}

/* SIMULATED: a write that fails after part of the batch was accepted
 * counts only the accepted bytes, so the caller can log where the file
 * now ends. */
static void test_sim_partial_then_failure(void)
{
    static storage_log_writer w;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    append_n(&w, 0, 2);
    fake.write_chunk = 60;
    fake.write_errno = EIO;
    fake.write_errno_after = 60;
    expect(storage_log_flush(&w), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_WRITE, EIO);
    assert(fake.write_calls == 2);
    assert(fake.written_len == 60);
    assert(w.file_size_bytes == 60);
    assert(fake.fdatasync_calls == 0);
    expect(storage_log_close(&w), STORAGE_LOG_FAILED,
           STORAGE_LOG_STAGE_NONE, 0);
}

/* SIMULATED: an fdatasync error is sticky and fdatasync is not retried. */
static void test_sim_sync_failure_is_sticky(void)
{
    static storage_log_writer w;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    append_n(&w, 0, 3);
    fake.fdatasync_errno = EIO;
    expect(storage_log_flush(&w), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_SYNC, EIO);
    assert(fake.fdatasync_calls == 1);
    assert(w.failed);

    fake.fdatasync_errno = 0;
    expect(storage_log_close(&w), STORAGE_LOG_FAILED,
           STORAGE_LOG_STAGE_NONE, 0);
    assert(fake.fdatasync_calls == 1);
    assert(w.fd == -1);
}

/* SIMULATED: close reports a flush error first, but still closes. */
static void test_sim_close_flush_failure(void)
{
    static storage_log_writer w;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    append_n(&w, 0, 1);
    fake.fdatasync_errno = EIO;
    fake.close_fail_fd = FAKE_FILE_FD;
    fake.close_errno = EINTR;
    expect(storage_log_close(&w), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_SYNC, EIO);
    assert(w.fd == -1);
    assert(fake.closed_fds[fake.close_calls - 1] == FAKE_FILE_FD);
}

/* SIMULATED: a failed close is reported and not retried. */
static void test_sim_close_failure(void)
{
    static storage_log_writer w;

    fake_reset();
    expect_ok(open_fake(&w, 0));
    append_n(&w, 0, 1);
    fake.close_fail_fd = FAKE_FILE_FD;
    fake.close_errno = EIO;
    expect(storage_log_close(&w), STORAGE_LOG_OS_ERROR,
           STORAGE_LOG_STAGE_CLOSE_FILE, EIO);
    assert(fake.fdatasync_calls == 1);
    assert(fake.close_calls == 2);  /* directory at open, file now */
    assert(w.fd == -1);
}

/* main: run every storage log test; abort on the first failure. */
int main(void)
{
    test_file_flush_writes_in_order();
    test_file_full_batch_writes();
    test_file_flush_if_due();
    test_file_close_flushes();
    test_file_reopen_appends();
    test_file_misaligned_refused();
    test_file_missing_dir();
    test_file_rejects_bad_records();
    test_file_arguments();

    test_sim_open_sequence();
    test_sim_open_failures();
    test_sim_eintr_and_short_writes();
    test_sim_write_failure_is_sticky();
    test_sim_partial_then_failure();
    test_sim_sync_failure_is_sticky();
    test_sim_close_flush_failure();
    test_sim_close_failure();

    puts("storage log tests passed (test-built records; temporary files "
         "and SIMULATED syscall failures; no sensor used)");
    return 0;
}
