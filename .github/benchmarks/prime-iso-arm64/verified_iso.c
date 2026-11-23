#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static const char url[] =
    "https://software-static.download.prss.microsoft.com/dbazure/988969d5-f34g-4e03-ac9d-1f9786c66750/"
    "19045.2006.220908-0225.22h2_release_svc_refresh_CLIENTENTERPRISEEVAL_OEMRET_x64FRE_en-us.iso";
static const char etag[] =
    "\"0x10B3D1548F58FA2A3866686CB570F06A16F6734DD99BBA1C7F0542A849EDD8E4\"";
static const uint64_t image_size = UINT64_C(5550497792);
enum { chunk_bytes = 1024 * 1024, slots = 12, max_download_chunks = 8192 };
struct chunk {
    uint64_t start, size, last_use;
    unsigned char data[chunk_bytes];
};
struct download {
    struct chunk* slot;
    uint64_t start, end;
    size_t received;
    unsigned etag_match, range_match;
};

static struct stat stub;
static struct chunk chunks[slots];
static CURL* transfer;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t clock_id, requests, downloads, delivered, downloaded_bytes;
static uint64_t first_fetched_offset = UINT64_MAX, last_fetched_byte;
static int active, failed;

static size_t write_bytes(char* data, size_t size, size_t count, void* opaque) {
    struct download* dl = opaque;
    const size_t amount = size * count;
    if (amount > dl->end + 1 - dl->start - dl->received) return 0;
    memcpy(dl->slot->data + dl->received, data, amount);
    dl->received += amount;
    return amount;
}

static size_t read_header(char* line, size_t size, size_t count, void* opaque) {
    struct download* dl = opaque;
    const size_t length = size * count;
    const char prefix[] = "content-range: bytes ";
    if (length >= sizeof(prefix) && !strncasecmp(line, prefix, sizeof(prefix) - 1)) {
        char range[120];
        const int expected = snprintf(range, sizeof(range), "%s%" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n",
            prefix, dl->start, dl->end, image_size);
        dl->range_match = expected >= 0 && (size_t)expected == length && !strncasecmp(line, range, length);
    }
    const char identity[] = "etag: ";
    if (length >= sizeof(identity) && !strncasecmp(line, identity, sizeof(identity) - 1)) {
        const size_t start = sizeof(identity) - 1;
        dl->etag_match = length == start + sizeof(etag) + 1 &&
            !memcmp(line + start, etag, sizeof(etag) - 1);
    }
    return length;
}

static struct chunk* get_chunk(uint64_t address) {
    const uint64_t start = address / chunk_bytes * chunk_bytes;
    struct chunk* expired = &chunks[0];
    for (unsigned i = 0; i < slots; ++i) {
        struct chunk* candidate = &chunks[i];
        if (candidate->size && candidate->start == start) {
            candidate->last_use = ++clock_id;
            return candidate;
        }
        if (candidate->last_use < expired->last_use) expired = candidate;
    }
    if (downloads >= max_download_chunks) {
        fprintf(stderr, "ISO range read reached the download budget\n");
        return NULL;
    }
    expired->size = 0;
    const uint64_t end = image_size - start < chunk_bytes ? image_size - 1 : start + chunk_bytes - 1;
    struct download dl = { .slot = expired, .start = start, .end = end };
    char range[64], request_url[sizeof(url) + 60];
    snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, start, end);
    snprintf(request_url, sizeof(request_url), "%s?range=%" PRIu64, url, start);
    curl_easy_setopt(transfer, CURLOPT_URL, request_url);
    curl_easy_setopt(transfer, CURLOPT_RANGE, range);
    curl_easy_setopt(transfer, CURLOPT_WRITEDATA, &dl);
    curl_easy_setopt(transfer, CURLOPT_HEADERDATA, &dl);
    ++requests;
    const CURLcode result = curl_easy_perform(transfer);
    long status = 0;
    curl_easy_getinfo(transfer, CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK || status != 206 || !dl.etag_match || !dl.range_match ||
            dl.received != end - start + 1) {
        fprintf(stderr,
            "ISO range verification failed: %s status=%ld curl=%d range=%u etag=%u bytes=%zu\n",
            range, status, result, dl.range_match, dl.etag_match, dl.received);
        return NULL;
    }
    expired->start = start;
    expired->size = dl.received;
    expired->last_use = ++clock_id;
    ++downloads;
    downloaded_bytes += dl.received;
    if (start < first_fetched_offset) first_fetched_offset = start;
    if (end > last_fetched_byte) last_fetched_byte = end;
    return expired;
}

__attribute__((constructor))
static void open_remote_iso(void) {
    const char* path = getenv("R110_REMOTE_ISO_STUB");
    if (!path || stat(path, &stub) || !S_ISREG(stub.st_mode) || (uint64_t)stub.st_size != image_size)
        _exit(127);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) _exit(127);
    transfer = curl_easy_init();
    if (!transfer) _exit(127);
    curl_easy_setopt(transfer, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(transfer, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(transfer, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(transfer, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(transfer, CURLOPT_WRITEFUNCTION, write_bytes);
    curl_easy_setopt(transfer, CURLOPT_HEADERFUNCTION, read_header);
    active = 1;
}

__attribute__((destructor))
static void close_remote_iso(void) {
    fprintf(stderr, "REMOTE_ISO requests=%" PRIu64 " verified_ranges=%" PRIu64
        " downloaded=%" PRIu64 " delivered=%" PRIu64 " failed=%d"
        " first_offset=%" PRIu64 " last_byte=%" PRIu64 "\n",
        requests, downloads, downloaded_bytes, delivered, failed,
        downloads ? first_fetched_offset : 0, last_fetched_byte);
    if (transfer) curl_easy_cleanup(transfer);
    curl_global_cleanup();
}


static int remote_fd(int fd) {
    struct stat info;
    return active && !fstat(fd, &info) && info.st_ino == stub.st_ino && info.st_dev == stub.st_dev;
}

static ssize_t read_remote_locked(void* into, size_t size, off64_t offset) {
    if (offset < 0) { errno = EINVAL; return -1; }
    if ((uint64_t)offset >= image_size) return 0;
    if (size > image_size - (uint64_t)offset) size = image_size - (uint64_t)offset;
    size_t copied = 0;
    while (copied < size) {
        const uint64_t address = (uint64_t)offset + copied;
        struct chunk* chunk = get_chunk(address);
        if (!chunk) {
            failed = 1;
            errno = EIO;
            return -1;
        }
        const uint64_t start = address - chunk->start;
        size_t take = chunk->size - start;
        if (take > size - copied) take = size - copied;
        memcpy((unsigned char*)into + copied, chunk->data + start, take);
        copied += take;
    }
    delivered += copied;
    return (ssize_t)copied;
}

ssize_t read(int fd, void* into, size_t size) {
    if (!remote_fd(fd)) return (ssize_t)syscall(SYS_read, fd, into, size);
    if ((fcntl(fd,F_GETFL) & O_ACCMODE) != O_RDONLY) { errno = EBADF; return -1; }
    pthread_mutex_lock(&lock);
    const off_t current = lseek(fd, 0, SEEK_CUR);
    ssize_t got = current < 0 ? -1 : read_remote_locked(into, size, current);
    if (got > 0 && lseek(fd, got, SEEK_CUR) < 0) { failed = 1; got = -1; }
    pthread_mutex_unlock(&lock);
    return got;
}

ssize_t pread64(int fd, void* into, size_t size, off64_t offset) {
    if (!remote_fd(fd)) return (ssize_t)syscall(SYS_pread64, fd, into, size, offset);
    if ((fcntl(fd,F_GETFL) & O_ACCMODE) != O_RDONLY) { errno = EBADF; return -1; }
    pthread_mutex_lock(&lock);
    const ssize_t got = read_remote_locked(into, size, offset);
    pthread_mutex_unlock(&lock);
    return got;
}

ssize_t pread(int fd, void* into, size_t size, off_t offset) {
    return pread64(fd, into, size, offset);
}
