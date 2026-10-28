#include "storage/record.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * storage-dump program: read a storage log record by record through
 * storage_record_decode and print every field, so what reached storage
 * can be checked by hand. Read-only; it never repairs a file.
 *
 * Each record line starts with SIMULATED or LIVE, from that record's own
 * SIMULATED flag, and the summary line with LIVE, SIMULATED, MIXED or
 * NO_RECORDS plus the count of simulated records. The summary checks that every record decoded, the
 * sequence numbers rise by exactly one, and the file is whole records.
 *
 * Exit status: 0 if every check passed, 1 if any failed or the file could
 * not be read, 2 for a usage error.
 */

/* type_name: a record type as text. */
static const char *type_name(uint8_t record_type)
{
    switch (record_type) {
    case STORAGE_RECORD_TYPE_LIGHT: return "light";
    case STORAGE_RECORD_TYPE_SOIL: return "soil";
    case STORAGE_RECORD_TYPE_START: return "START";
    default: return "UNKNOWN";
    }
}

/* decode_name: a storage_record_decode result as text. */
static const char *decode_name(storage_record_result r)
{
    switch (r) {
    case STORAGE_RECORD_OK: return "OK";
    case STORAGE_RECORD_ARGUMENT: return "ARGUMENT";
    case STORAGE_RECORD_BAD_MAGIC: return "BAD_MAGIC";
    case STORAGE_RECORD_BAD_VERSION: return "BAD_VERSION";
    case STORAGE_RECORD_BAD_CRC: return "BAD_CRC";
    default: return "UNKNOWN";
    }
}

/* unit_name: the unit of a record's sensor_value. */
static const char *unit_name(uint8_t record_type)
{
    switch (record_type) {
    case STORAGE_RECORD_TYPE_LIGHT: return "millilux";
    case STORAGE_RECORD_TYPE_SOIL: return "raw_counts";
    default: return "none";
    }
}

/* print_record: one decoded record, every field. */
static void print_record(uint64_t index, const storage_record *r)
{
    printf("%s record=%" PRIu64 " type=%s sequence=%" PRIu64
           " valid=%d simulated=%d clock_untrusted=%d time_unavailable=%d"
           " wall_ns=%" PRIu64 " mono_ns=%" PRIu64 " value=%" PRIu32
           " unit=%s i2c_status=%u failure_stage=%u errno=%u"
           " source_sequence=%" PRIu32 " crc=OK\n",
           (r->flags & STORAGE_FLAG_SIMULATED) ? "SIMULATED" : "LIVE",
           index, type_name(r->record_type), r->sequence,
           (r->flags & STORAGE_FLAG_VALID) != 0,
           (r->flags & STORAGE_FLAG_SIMULATED) != 0,
           (r->flags & STORAGE_FLAG_CLOCK_UNTRUSTED) != 0,
           (r->flags & STORAGE_FLAG_TIME_UNAVAILABLE) != 0,
           r->wall_clock_ns, r->monotonic_ns, r->sensor_value,
           unit_name(r->record_type), (unsigned)r->i2c_status,
           (unsigned)r->failure_stage, (unsigned)r->os_errno,
           r->source_sequence);
}

/* main: dump one file. */
int main(int argc, char **argv)
{
    uint8_t buf[STORAGE_RECORD_SIZE_BYTES];
    storage_record rec;
    uint64_t index = 0;
    uint64_t counts[4] = {0, 0, 0, 0};   /* by type; 0 is unknown */
    uint64_t valid_counts[4] = {0, 0, 0, 0};
    uint64_t bad = 0;
    uint64_t sequence_breaks = 0;
    uint64_t expect_sequence = 0;
    int have_expect = 0;             /* a good record set expect_sequence */
    uint64_t simulated = 0;
    uint64_t decoded = 0;
    const char *label;
    size_t tail_bytes = 0;
    int read_failed = 0;
    int read_errno = 0;
    FILE *f;
    int ok;

    if (argc != 2 || argv[1] == NULL || argv[1][0] == '\0' ||
        strcmp(argv[1], "--help") == 0) {
        fprintf(argc == 2 && strcmp(argv[1], "--help") == 0 ? stdout
                                                            : stderr,
                "usage: %s FILE\nPrint every record of a storage log and "
                "check it.\n", argc > 0 ? argv[0] : "storage-dump");
        return argc == 2 && strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }

    errno = 0;
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "%s: cannot open '%s': %s\n", argv[0], argv[1],
                strerror(errno != 0 ? errno : EIO));
        return 1;
    }
    for (;;) {
        size_t n;
        storage_record_result dr;

        errno = 0;
        n = fread(buf, 1, sizeof buf, f);

        if (n < sizeof buf) {
            if (ferror(f)) {
                read_failed = 1;
                read_errno = errno;
            }
            tail_bytes = n;
            break;
        }
        dr = storage_record_decode(buf, &rec);
        if (dr != STORAGE_RECORD_OK) {
            printf("BAD record=%" PRIu64 " decode=%s\n", index,
                   decode_name(dr));
            bad++;
            /* Assume it held the next number, so it is not also a break. */
            if (have_expect) {
                expect_sequence++;
            }
        } else {
            unsigned t = rec.record_type <= STORAGE_RECORD_TYPE_START
                             ? rec.record_type : 0;
            print_record(index, &rec);
            decoded++;
            counts[t]++;
            if (rec.flags & STORAGE_FLAG_VALID) {
                valid_counts[t]++;
            }
            if (rec.flags & STORAGE_FLAG_SIMULATED) {
                simulated++;
            }
            if (t == 0) {
                bad++;
            }
            if (have_expect && rec.sequence != expect_sequence) {
                sequence_breaks++;
            }
            expect_sequence = rec.sequence + 1;
            have_expect = 1;
        }
        index++;
    }
    errno = 0;
    if (fclose(f) != 0) {
        fprintf(stderr, "%s: closing '%s' failed: %s\n", argv[0], argv[1],
                strerror(errno != 0 ? errno : EIO));
        read_failed = 1;
    }
    if (read_failed) {
        fprintf(stderr, "%s: reading '%s' failed: %s\n", argv[0], argv[1],
                strerror(read_errno != 0 ? read_errno : EIO));
    }

    ok = !read_failed && bad == 0 && sequence_breaks == 0 &&
         tail_bytes == 0 && index > 0;
    label = decoded == 0 ? "NO_RECORDS"
            : simulated == 0 ? "LIVE"
            : simulated == decoded ? "SIMULATED" : "MIXED";
    printf("%s summary records=%" PRIu64 " simulated_records=%" PRIu64
           " start=%" PRIu64 " light=%" PRIu64
           " light_valid=%" PRIu64 " soil=%" PRIu64 " soil_valid=%" PRIu64
           " bad=%" PRIu64 " sequence_breaks=%" PRIu64
           " partial_tail_bytes=%zu result=%s\n",
           label, index, simulated, counts[STORAGE_RECORD_TYPE_START],
           counts[STORAGE_RECORD_TYPE_LIGHT],
           valid_counts[STORAGE_RECORD_TYPE_LIGHT],
           counts[STORAGE_RECORD_TYPE_SOIL],
           valid_counts[STORAGE_RECORD_TYPE_SOIL], bad, sequence_breaks,
           tail_bytes, ok ? "PASS" : "FAIL");
    if (fflush(stdout) != 0 || ferror(stdout)) {
        return 1;
    }
    return ok ? 0 : 1;
}
